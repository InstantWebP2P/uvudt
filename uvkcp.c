//////////////////////////////////////////////////////
// KCP interfaces implementation
// Copyright 2020, tom zhou<appnet.link@gmail.com>
//////////////////////////////////////////////////////

#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>
#include "kcp/ikcp.h"
#include "uvkcp.h"

#define KCP_DEBUG 1

void kcp__stream_io(uv_poll_t *handle, int status, int events);

// Forward declarations for TCP handshake callbacks
static void tcp_connection_cb(uv_stream_t *server, int status);
static void tcp_handshake_read_cb(uv_stream_t *tcp_client, ssize_t nread, const uv_buf_t *buf);
static void tcp_handshake_write_cb(uv_write_t *req, int status);
static void tcp_client_handshake_write_cb(uv_write_t *req, int status);
static void tcp_client_handshake_read_cb(uv_stream_t *tcp_client, ssize_t nread, const uv_buf_t *buf);
static void tcp_connect_cb(uv_connect_t *req, int status);

// Forward declarations for conversation registry functions
static void init_conv_registry(kcp_context_t *ctx);
static int add_conv_to_registry(kcp_context_t *ctx, uint32_t conv_id, uvkcp_t *handle);
static int remove_conv_from_registry(kcp_context_t *ctx, uint32_t conv_id);
static void cleanup_conv_registry(kcp_context_t *ctx);
static int conv_exists_in_registry(kcp_context_t *ctx, uint32_t conv_id);


// KCP output function - sends data over UDP
static int kcp_output(const char *buf, int len, ikcpcb *kcp, void *user) {
    kcp_context_t *ctx = (kcp_context_t *)user;

    if (ctx->udp_fd == -1) {
        return -1;
    }

    // Try to send immediately
    ssize_t sent = sendto(ctx->udp_fd, buf, len, 0,
                         (struct sockaddr *)&ctx->peer_addr,
                         ctx->peer_addr_len);

    if (sent < 0) {
        // If EAGAIN/EWOULDBLOCK, we need to ensure writable event monitoring
        // KCP will handle retransmission, but we need to make sure the socket
        // is being monitored for writable events so we can flush pending data
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Find the uvkcp_t handle that contains this KCP context
            // Since uvkcp_t inherits from uv_poll_t, we need to search for it
            // For now, we'll rely on the KCP timer to retry later
            // In a more sophisticated implementation, we would track the association
            // between KCP contexts and their uvkcp_t handles
            return -1;
        }
        return -1;
    }

    // Track statistics
    ctx->pktSentTotal++;
    ctx->bytesSentTotal += sent;

    // KCP expects 0 on success, not the number of bytes sent
    return 0;
}

// Initialize KCP handle
int uvkcp_init(uv_loop_t *loop, uvkcp_t *handle) {
    UVKCP_LOG_FUNC("Initializing KCP handle");

    static int _initialized = 0;

    if (!_initialized) {
        // KCP library initialization if needed
        _initialized = 1;
        UVKCP_LOG("KCP library initialized");
    }

    // Initialize stream
    kcp__stream_init(loop, handle);

    // Allocate KCP context
    kcp_context_t *ctx = (kcp_context_t *)malloc(sizeof(kcp_context_t));
    if (!ctx) {
        UVKCP_LOG_ERROR("Failed to allocate KCP context");
        return UV_ENOMEM;
    }

    memset(ctx, 0, sizeof(kcp_context_t));
    ctx->loop = loop;
    ctx->udp_fd = -1;
    ctx->is_connected = 0;
    ctx->is_listening = 0;
    ctx->timer_active = 0;
    ctx->backlog = 0;
    ctx->connection_cb = NULL;
    memset(&ctx->server_addr, 0, sizeof(ctx->server_addr));
    ctx->server_addr_len = 0;
    ctx->next_conv = 1;
    ctx->pending_connect_req = NULL;

    // Initialize statistics
    ctx->pktSentTotal = 0;
    ctx->pktRecvTotal = 0;
    ctx->pktSndLossTotal = 0;
    ctx->pktRcvLossTotal = 0;
    ctx->pktRetransTotal = 0;
    ctx->bytesSentTotal = 0;
    ctx->bytesRecvTotal = 0;
    ctx->lastUpdateTime = 0;

    // Initialize conversation registry
    ctx->conv_registry = NULL;
    ctx->shared_udp_fd = -1;

    // Initialize timer handle
    if (uv_timer_init(loop, &ctx->timer_handle) < 0) {
        UVKCP_LOG_ERROR("Failed to initialize timer");
        free(ctx);
        return UV_ENOMEM;
    }
    ctx->timer_handle.data = ctx;

    // Store context in handle
    handle->kcp_ctx = ctx;

    UVKCP_LOG("KCP handle initialized successfully");
    return 0;
}

// Open KCP handle with existing UDP socket
int uvkcp_open(uvkcp_t *handle, uv_os_sock_t sock) {
    kcp_context_t *ctx = (kcp_context_t *)handle->kcp_ctx;
    if (!ctx) {
        return UV_EINVAL;
    }

    ctx->udp_fd = sock;

    // KCP instance creation deferred until TCP handshake completes
    // TCP handshake required to exchange conversation ID and peer address
    ctx->kcp = NULL;

    UVKCP_LOG("KCP handle opened, TCP handshake required for connection");
    return kcp__stream_open(handle, sock, 0); // No flags until connected
}

// Bind KCP to address
int uvkcp_bind(uvkcp_t *handle, const struct sockaddr *addr, int reuseaddr, int reuseable) {
    UVKCP_LOG_FUNC("Binding KCP handle");

    kcp_context_t *ctx = (kcp_context_t *)handle->kcp_ctx;
    if (!ctx) {
        UVKCP_LOG_ERROR("Invalid KCP context");
        return UV_EINVAL;
    }

    // Initialize TCP server for handshake
    if (uv_tcp_init(ctx->loop, &ctx->tcp_server) < 0) {
        UVKCP_LOG_ERROR("Failed to initialize TCP server");
        return UV_ENOMEM;
    }

    ctx->tcp_server.data = handle;

    // Set TCP socket options
    if (reuseaddr) {
        // Set SO_REUSEADDR on TCP socket
        uv_tcp_t *tcp = &ctx->tcp_server;
        if (uv_tcp_keepalive(tcp, 1, 60) != 0) {
            UVKCP_LOG("Failed to set TCP keepalive");
        }
        UVKCP_LOG("Set SO_REUSEADDR on TCP handshake socket");
    }

    // Bind TCP server to the specified address
    if (uv_tcp_bind(&ctx->tcp_server, addr, 0) < 0) {
        UVKCP_LOG_ERROR("Failed to bind TCP server");
        uv_close((uv_handle_t *)&ctx->tcp_server, NULL);
        return uv_translate_sys_error(errno);
    }

    // Store the server address for later use
    memcpy(&ctx->server_addr, addr, sizeof(ctx->server_addr));
    ctx->server_addr_len = (addr->sa_family == AF_INET) ?
                          sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);

    // UDP socket will be created later for each client connection
    ctx->udp_fd = -1;
    ctx->shared_udp_fd = -1;

    UVKCP_LOG("KCP handle bound to TCP handshake socket, ready for listening");
    return 0;
}

// Connect to remote address
int uvkcp_connect(uvkcp_connect_t *req, uvkcp_t *handle, const struct sockaddr *addr, uvkcp_connect_cb cb) {
    UVKCP_LOG_FUNC("Connecting KCP handle");

    kcp_context_t *ctx = (kcp_context_t *)handle->kcp_ctx;
    if (!ctx) {
        UVKCP_LOG_ERROR("Invalid KCP context");
        return UV_EINVAL;
    }

    // Setup connection request
    req->handle = handle;
    req->cb = cb;
    handle->connect_req = req;

    // Always use TCP handshake for KCP connection
    UVKCP_LOG("Using TCP handshake for KCP connection");

    // Store the connect request for later use
    ctx->pending_connect_req = req;

    // Store the server address for use in TCP connect callback
    memcpy(&ctx->server_addr, addr, sizeof(ctx->server_addr));
    ctx->server_addr_len = (addr->sa_family == AF_INET) ?
                          sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);

    // Initialize TCP client for handshake
    if (uv_tcp_init(ctx->loop, &ctx->tcp_client) < 0) {
        UVKCP_LOG_ERROR("Failed to initialize TCP client");
        return UV_ENOMEM;
    }

    ctx->tcp_client.data = ctx;

    // Connect to TCP server for handshake
    uv_connect_t *tcp_connect_req = malloc(sizeof(uv_connect_t));
    if (!tcp_connect_req) {
        UVKCP_LOG_ERROR("Failed to allocate TCP connect request");
        return UV_ENOMEM;
    }

    tcp_connect_req->data = ctx;

    int r = uv_tcp_connect(tcp_connect_req, &ctx->tcp_client, addr, tcp_connect_cb);
    if (r != 0) {
        UVKCP_LOG_ERROR("Failed to connect to TCP server: %d", r);
        free(tcp_connect_req);
        return r;
    }

    UVKCP_LOG("TCP handshake initiated");
    return 0;
}

// Listen for incoming connections
int uvkcp_listen(uvkcp_t *stream, int backlog, uvkcp_connection_cb cb) {
    kcp_context_t *ctx = (kcp_context_t *)stream->kcp_ctx;
    if (!ctx) {
        return UV_EINVAL;
    }

    if (ctx->is_connected) {
        UVKCP_LOG_ERROR("KCP handle already connected, cannot listen");
        return UV_EISCONN;
    }

    // Check if TCP server is already bound
    if (ctx->tcp_server.loop == NULL) {
        UVKCP_LOG_ERROR("KCP handle not bound to TCP socket, call uvkcp_bind first");
        return UV_EINVAL;
    }

    stream->connection_cb = cb;
    ctx->connection_cb = cb;
    ctx->backlog = backlog;
    ctx->is_listening = 1;

    // Start listening on the already-bound TCP server
    UVKCP_LOG("Starting TCP server for KCP handshake");

    // Start listening on TCP server
    int r = uv_listen((uv_stream_t *)&ctx->tcp_server, backlog, tcp_connection_cb);
    if (r != 0) {
        UVKCP_LOG_ERROR("Failed to listen on TCP server: %d", r);
        return r;
    }

    UVKCP_LOG("KCP server listening on TCP handshake socket, backlog=%d", backlog);
    return 0;
}

// Close KCP handle
int uvkcp_close(uvkcp_t *handle, uv_close_cb close_cb) {
    kcp_context_t *ctx = (kcp_context_t *)handle->kcp_ctx;
    if (!ctx) {
        return UV_EINVAL;
    }

    // Stop timer
    if (ctx->timer_active) {
        uv_timer_stop(&ctx->timer_handle);
        ctx->timer_active = 0;
    }

    // Cleanup KCP
    if (ctx->kcp) {
        // Remove conversation ID from registry if this is a client connection
        if (ctx->is_connected && !ctx->is_listening && ctx->server_ctx) {
            // Get conversation ID from KCP instance
            uint32_t conv_id = ctx->kcp->conv;
            remove_conv_from_registry(ctx->server_ctx, conv_id);
        }
        ikcp_release(ctx->kcp);
        ctx->kcp = NULL;
    }

    // Close TCP connections for handshake
    uv_close((uv_handle_t *)&ctx->tcp_server, NULL);
    uv_close((uv_handle_t *)&ctx->tcp_client, NULL);

    // Close UDP socket
    if (ctx->udp_fd != -1) {
        close(ctx->udp_fd);
        ctx->udp_fd = -1;
    }

    // Clean up conversation registry for server contexts
    if (ctx->is_listening) {
        cleanup_conv_registry(ctx);
    }

    // Free context
    free(ctx);
    handle->kcp_ctx = NULL;

    // Call stream destroy
    kcp__stream_destroy(handle);

    return 0;
}

// Set KCP nodelay option
int uvkcp_nodelay(uvkcp_t *handle, int enable, int interval, int resend, int nc) {
    kcp_context_t *ctx = (kcp_context_t *)handle->kcp_ctx;
    if (!ctx || !ctx->kcp) {
        return UV_EINVAL;
    }

    ikcp_nodelay(ctx->kcp, enable, interval, resend, nc);
    return 0;
}

// Set KCP window size
int uvkcp_wndsize(uvkcp_t *handle, int sndwnd, int rcvwnd) {
    kcp_context_t *ctx = (kcp_context_t *)handle->kcp_ctx;
    if (!ctx || !ctx->kcp) {
        return UV_EINVAL;
    }

    ikcp_wndsize(ctx->kcp, sndwnd, rcvwnd);
    return 0;
}

// Set KCP MTU
int uvkcp_setmtu(uvkcp_t *handle, int mtu) {
    kcp_context_t *ctx = (kcp_context_t *)handle->kcp_ctx;
    if (!ctx || !ctx->kcp) {
        return UV_EINVAL;
    }

    ikcp_setmtu(ctx->kcp, mtu);
    return 0;
}

// Translate KCP error to libuv error
int uvkcp_translate_kcp_error(void) {
    // KCP doesn't have detailed error codes, return generic error
    return UV_EIO;
}

// Stub implementations for missing functions
int uvkcp_keepalive(uvkcp_t *handle, int enable, unsigned int delay) {
    // KCP doesn't support keepalive in the same way as TCP
    return 0;
}

int uvkcp_getsockname(const uvkcp_t* handle, struct sockaddr* name, int* namelen) {
    kcp_context_t *ctx = (kcp_context_t *)handle->kcp_ctx;
    if (!ctx || ctx->udp_fd == -1) {
        return UV_EINVAL;
    }

    socklen_t len = *namelen;
    if (getsockname(ctx->udp_fd, name, &len) < 0) {
        return uv_translate_sys_error(errno);
    }
    *namelen = len;
    return 0;
}

int uvkcp_getpeername(const uvkcp_t *handle, struct sockaddr *name, int *namelen) {
    kcp_context_t *ctx = (kcp_context_t *)handle->kcp_ctx;
    if (!ctx || !ctx->is_connected) {
        return UV_ENOTCONN;
    }

    socklen_t len = *namelen;
    if (len > ctx->peer_addr_len) {
        len = ctx->peer_addr_len;
    }
    memcpy(name, &ctx->peer_addr, len);
    *namelen = len;
    return 0;
}



int uvkcp_getperf(uvkcp_t *handle, uvkcp_netperf_t *perf, int clear) {
    kcp_context_t *ctx = (kcp_context_t *)handle->kcp_ctx;
    if (!ctx || !ctx->kcp) {
        return UV_EINVAL;
    }

    // Get basic KCP statistics
    memset(perf, 0, sizeof(uvkcp_netperf_t));

    // Fill in actual statistics from context
    perf->msTimeStamp = uv_now(ctx->loop);
    perf->pktSentTotal = ctx->pktSentTotal;
    perf->pktRecvTotal = ctx->pktRecvTotal;
    perf->pktSndLossTotal = ctx->pktSndLossTotal;
    perf->pktRcvLossTotal = ctx->pktRcvLossTotal;
    perf->pktRetransTotal = ctx->pktRetransTotal;
    perf->pktSentACKTotal = 0; // KCP handles ACKs internally
    perf->pktRecvACKTotal = 0; // KCP handles ACKs internally
    perf->pktSentNAKTotal = 0; // KCP doesn't use NAK
    perf->pktRecvNAKTotal = 0; // KCP doesn't use NAK
    perf->usSndDurationTotal = 0; // Not tracked currently

    // Calculate rates (simplified)
    IUINT32 current = uv_now(ctx->loop);
    IUINT32 timeDiff = current - ctx->lastUpdateTime;
    if (timeDiff > 0) {
        double timeSec = timeDiff / 1000.0;
        if (timeSec > 0) {
            perf->mbpsSendRate = (ctx->bytesSentTotal * 8.0) / (timeSec * 1000000.0);
            perf->mbpsRecvRate = (ctx->bytesRecvTotal * 8.0) / (timeSec * 1000000.0);
        }
    }

    // Update last update time
    ctx->lastUpdateTime = current;

    // Clear statistics if requested
    if (clear) {
        ctx->pktSentTotal = 0;
        ctx->pktRecvTotal = 0;
        ctx->pktSndLossTotal = 0;
        ctx->pktRcvLossTotal = 0;
        ctx->pktRetransTotal = 0;
        ctx->bytesSentTotal = 0;
        ctx->bytesRecvTotal = 0;
    }

    return 0;
}
// Server/Client style KCP implementation with TCP handshake

// Simple conversation registry entry
struct conv_registry_entry_s {
    uint32_t conv_id;
    uvkcp_t *handle;
    struct conv_registry_entry_s *next;
};
typedef struct conv_registry_entry_s conv_registry_entry_t;

// Simple hash table for conversation ID registry
#define CONV_REGISTRY_SIZE 256

// Initialize conversation registry
static void init_conv_registry(kcp_context_t *ctx) {
    if (ctx->conv_registry == NULL) {
        ctx->conv_registry = calloc(CONV_REGISTRY_SIZE, sizeof(conv_registry_entry_t*));
    }
}

// Hash function for conversation ID
static unsigned int conv_hash(uint32_t conv_id) {
    return conv_id % CONV_REGISTRY_SIZE;
}

// Add conversation ID to registry
static int add_conv_to_registry(kcp_context_t *ctx, uint32_t conv_id, uvkcp_t *handle) {
    if (!ctx->conv_registry) {
        init_conv_registry(ctx);
    }

    unsigned int hash = conv_hash(conv_id);
    conv_registry_entry_t **bucket = (conv_registry_entry_t**)ctx->conv_registry + hash;

    // Check for existing entry
    conv_registry_entry_t *entry = *bucket;
    while (entry) {
        if (entry->conv_id == conv_id) {
            UVKCP_LOG_ERROR("Conversation ID %u already exists in registry", conv_id);
            return -1; // Conflict detected
        }
        entry = entry->next;
    }

    // Add new entry
    entry = malloc(sizeof(conv_registry_entry_t));
    if (!entry) {
        UVKCP_LOG_ERROR("Failed to allocate conversation registry entry for conv %u", conv_id);
        return -1;
    }
    entry->conv_id = conv_id;
    entry->handle = handle;
    entry->next = *bucket;
    *bucket = entry;

    UVKCP_LOG("Registered conversation ID %u in registry", conv_id);
    return 0;
}

// Remove conversation ID from registry
static int remove_conv_from_registry(kcp_context_t *ctx, uint32_t conv_id) {
    if (!ctx->conv_registry) {
        UVKCP_LOG_ERROR("Cannot remove conversation ID %u - registry not initialized", conv_id);
        return -1;
    }

    unsigned int hash = conv_hash(conv_id);
    conv_registry_entry_t **bucket = (conv_registry_entry_t**)ctx->conv_registry + hash;
    conv_registry_entry_t *prev = NULL;
    conv_registry_entry_t *entry = *bucket;

    while (entry) {
        if (entry->conv_id == conv_id) {
            if (prev) {
                prev->next = entry->next;
            } else {
                *bucket = entry->next;
            }
            free(entry);
            UVKCP_LOG("Removed conversation ID %u from registry", conv_id);
            return 0;
        }
        prev = entry;
        entry = entry->next;
    }

    UVKCP_LOG_ERROR("Conversation ID %u not found in registry", conv_id);
    return -1; // Not found
}

// Clean up entire conversation registry
static void cleanup_conv_registry(kcp_context_t *ctx) {
    if (!ctx->conv_registry) {
        UVKCP_LOG("No conversation registry to clean up");
        return;
    }

    int total_entries = 0;
    int i;
    for (i = 0; i < CONV_REGISTRY_SIZE; i++) {
        conv_registry_entry_t **bucket = (conv_registry_entry_t**)ctx->conv_registry + i;
        conv_registry_entry_t *entry = *bucket;
        while (entry) {
            conv_registry_entry_t *next = entry->next;
            free(entry);
            entry = next;
            total_entries++;
        }
        *bucket = NULL;
    }

    free(ctx->conv_registry);
    ctx->conv_registry = NULL;

    UVKCP_LOG("Cleaned up conversation registry with %d entries", total_entries);
}

// Check if conversation ID exists in registry
static int conv_exists_in_registry(kcp_context_t *ctx, uint32_t conv_id) {
    if (!ctx->conv_registry) {
        return 0;
    }

    unsigned int hash = conv_hash(conv_id);
    conv_registry_entry_t *bucket = *((conv_registry_entry_t**)ctx->conv_registry + hash);

    while (bucket) {
        if (bucket->conv_id == conv_id) {
            return 1;
        }
        bucket = bucket->next;
    }

    return 0;
}

// Helper function to generate random conversation ID with conflict detection
static uint32_t generate_conv_id(kcp_context_t *ctx) {
    // Start with a random base to avoid predictable conversation IDs
    static uint32_t base_conv = 0;
    if (base_conv == 0) {
        base_conv = (uint32_t)rand() | 0x10000000; // Ensure non-zero and high bit set
        UVKCP_LOG("Initialized conversation ID base: %u", base_conv);
    }

    // Generate conversation ID with conflict detection
    uint32_t conv;
    int attempts = 100; // Prevent infinite loop
    int conflicts = 0;

    do {
        conv = base_conv + ctx->next_conv;
        ctx->next_conv++;

        // Wrap around if needed (avoid 0 which is reserved for handshake requests)
        if (ctx->next_conv > 0x0FFFFFFF) {
            ctx->next_conv = 1;
            base_conv = (uint32_t)rand() | 0x10000000;
            UVKCP_LOG("Wrapped conversation ID counter, new base: %u", base_conv);
        }

        if (conv_exists_in_registry(ctx, conv)) {
            conflicts++;
            UVKCP_LOG("Conversation ID conflict detected: %u (attempt %d)", conv, attempts);
        }

        attempts--;
    } while (conv_exists_in_registry(ctx, conv) && attempts > 0);

    if (attempts <= 0) {
        // Fallback: use timestamp-based ID
        conv = (uint32_t)uv_now(ctx->loop) ^ (uint32_t)rand();
        UVKCP_LOG("Used fallback conversation ID: %u (after %d conflicts)", conv, conflicts);
    } else if (conflicts > 0) {
        UVKCP_LOG("Generated conversation ID: %u (resolved %d conflicts)", conv, conflicts);
    } else {
        UVKCP_LOG("Generated conversation ID: %u", conv);
    }

    return conv;
}

// Helper function to get current timestamp
static uint32_t get_timestamp(void) {
    return (uint32_t)(uv_now(uv_default_loop()) / 1000);
}

// Helper function to generate random nonce
static uint32_t generate_nonce(void) {
    return (uint32_t)rand();
}

// Simple alloc function for TCP handshake
static void echo_alloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    buf->base = malloc(suggested_size);
    buf->len = suggested_size;
}

// TCP connection callback for server
static void tcp_connection_cb(uv_stream_t *server, int status) {
    if (status != 0) {
        UVKCP_LOG_ERROR("TCP connection error: %d", status);
        return;
    }

    uvkcp_t *kcp_server = (uvkcp_t *)server->data;
    kcp_context_t *server_ctx = (kcp_context_t *)kcp_server->kcp_ctx;

    uv_tcp_t *tcp_client = malloc(sizeof(uv_tcp_t));
    if (!tcp_client) {
        UVKCP_LOG_ERROR("Failed to allocate TCP client");
        return;
    }

    if (uv_tcp_init(server_ctx->loop, tcp_client) < 0) {
        UVKCP_LOG_ERROR("Failed to initialize TCP client");
        free(tcp_client);
        return;
    }

    tcp_client->data = kcp_server;

    if (uv_accept(server, (uv_stream_t *)tcp_client) == 0) {
        UVKCP_LOG("Accepted TCP connection for KCP handshake");

        // Start reading handshake request
        uv_read_start((uv_stream_t *)tcp_client,
                     (uv_alloc_cb)echo_alloc,
                     tcp_handshake_read_cb);
    } else {
        UVKCP_LOG_ERROR("Failed to accept TCP connection");
        uv_close((uv_handle_t *)tcp_client, NULL);
    }
}

// TCP read callback for handshake
static void tcp_handshake_read_cb(uv_stream_t *tcp_client, ssize_t nread, const uv_buf_t *buf) {
    if (nread < 0) {
        UVKCP_LOG_ERROR("TCP handshake read error: %zd", nread);
        free(buf->base);
        uv_close((uv_handle_t *)tcp_client, NULL);
        return;
    }

    if (nread == sizeof(kcp_handshake_t)) {
        kcp_handshake_t *handshake = (kcp_handshake_t *)buf->base;
        uvkcp_t *kcp_server = (uvkcp_t *)tcp_client->data;
        kcp_context_t *server_ctx = (kcp_context_t *)kcp_server->kcp_ctx;

        // Process handshake request
        uint32_t conv = ntohl(handshake->conv);
        UVKCP_LOG("Received KCP handshake request: conv=%u", conv);

        // Create response handshake
        kcp_handshake_t response;
        uint32_t assigned_conv = generate_conv_id(server_ctx);
        response.conv = htonl(assigned_conv);

        // Get server's TCP socket address for proper peer communication
        // UDP socket will be created later for each client connection
        struct sockaddr_storage server_addr;
        socklen_t server_addr_len = sizeof(server_addr);
        if (getsockname(server_ctx->tcp_server.io_watcher.fd, (struct sockaddr*)&server_addr, &server_addr_len) == 0) {
            if (server_addr.ss_family == AF_INET) {
                struct sockaddr_in *addr_in = (struct sockaddr_in*)&server_addr;
                response.udp_port = addr_in->sin_port;
            } else if (server_addr.ss_family == AF_INET6) {
                struct sockaddr_in6 *addr_in6 = (struct sockaddr_in6*)&server_addr;
                response.udp_port = addr_in6->sin6_port;
            } else {
                response.udp_port = 0;
            }
        } else {
            response.udp_port = 0;
        }

        response.addr_family = handshake->addr_family;
        memcpy(response.peer_addr, handshake->peer_addr, sizeof(response.peer_addr));
        response.timestamp = htonl(get_timestamp());
        response.nonce = htonl(generate_nonce());
        response.flags = 0;

        // Send response
        uv_buf_t response_buf = uv_buf_init((char *)&response, sizeof(response));
        uv_write_t *write_req = malloc(sizeof(uv_write_t));
        write_req->data = tcp_client;

        uv_write(write_req, tcp_client, &response_buf, 1, tcp_handshake_write_cb);

        UVKCP_LOG("Sent KCP handshake response: conv=%u", ntohl(response.conv));

        // For server mode, we need to create a KCP object and call the connection callback
        if (kcp_server && kcp_server->connection_cb) {
            // Create a new KCP handle for the accepted connection
            uvkcp_t *client = malloc(sizeof(uvkcp_t));
            if (client) {
                if (uvkcp_init(kcp_server->aloop, client) == 0) {
                    kcp_context_t *ctx = (kcp_context_t *)client->kcp_ctx;
                    if (ctx) {
                        // Create UDP socket for KCP communication
                        int domain = (handshake->addr_family == AF_INET) ? AF_INET : AF_INET6;
                        int sock = socket(domain, SOCK_DGRAM, 0);
                        if (sock >= 0) {
                            ctx->udp_fd = sock;

                            // Create KCP instance with assigned conversation ID
                            uint32_t conv_id = ntohl(response.conv);
                            ctx->kcp = ikcp_create(conv_id, ctx);
                            if (ctx->kcp) {
                                // Set server context reference for registry cleanup
                                ctx->server_ctx = server_ctx;

                                // Register conversation ID in registry
                                if (add_conv_to_registry(server_ctx, conv_id, client) != 0) {
                                    UVKCP_LOG_ERROR("Failed to register conversation ID %u in registry", conv_id);
                                    // Continue anyway, but log the error
                                }

                                // Configure KCP
                                ikcp_setoutput(ctx->kcp, kcp_output);
                                ikcp_nodelay(ctx->kcp, 1, 10, 2, 1);
                                ikcp_wndsize(ctx->kcp, 128, 128);
                                ikcp_setmtu(ctx->kcp, 1400);

                                // Set peer address from handshake
                                if (handshake->addr_family == AF_INET) {
                                    struct sockaddr_in *peer = (struct sockaddr_in *)&ctx->peer_addr;
                                    peer->sin_family = AF_INET;
                                    peer->sin_port = handshake->udp_port;
                                    memcpy(&peer->sin_addr, handshake->peer_addr, 4);
                                    ctx->peer_addr_len = sizeof(struct sockaddr_in);
                                } else {
                                    struct sockaddr_in6 *peer = (struct sockaddr_in6 *)&ctx->peer_addr;
                                    peer->sin6_family = AF_INET6;
                                    peer->sin6_port = handshake->udp_port;
                                    memcpy(&peer->sin6_addr, handshake->peer_addr, 16);
                                    ctx->peer_addr_len = sizeof(struct sockaddr_in6);
                                }

                                // Mark as connected
                                ctx->is_connected = 1;

                                // Open the KCP stream
                                if (kcp__stream_open(client, sock, UVKCP_FLAG_READABLE | UVKCP_FLAG_WRITABLE) == 0) {
                                    UVKCP_LOG("KCP server created successfully with conv=%u", ntohl(response.conv));
                                    // Call connection callback with the new client
                                    kcp_server->connection_cb(client, 0);
                                } else {
                                    UVKCP_LOG_ERROR("Failed to open KCP stream");
                                    close(sock);
                                    ikcp_release(ctx->kcp);
                                    ctx->kcp = NULL;
                                    ctx->is_connected = 0; // Reset connection state on failure
                                    // Call connection callback with error
                                    kcp_server->connection_cb(NULL, UV_EIO);
                                }
                            } else {
                                UVKCP_LOG_ERROR("Failed to create KCP instance");
                                close(sock);
                                ctx->is_connected = 0; // Reset connection state on failure
                                // Call connection callback with error
                                kcp_server->connection_cb(NULL, UV_ENOMEM);
                            }
                        } else {
                            UVKCP_LOG_ERROR("Failed to create UDP socket");
                            ctx->is_connected = 0; // Reset connection state on failure
                            // Call connection callback with error
                            kcp_server->connection_cb(NULL, uv_translate_sys_error(errno));
                        }
                    } else {
                        UVKCP_LOG_ERROR("Failed to get KCP context");
                        ctx->is_connected = 0; // Reset connection state on failure
                        // Call connection callback with error
                        kcp_server->connection_cb(NULL, UV_ENOMEM);
                    }
                } else {
                    UVKCP_LOG_ERROR("Failed to initialize KCP handle");
                    free(client);
                    // Call connection callback with error
                    kcp_server->connection_cb(NULL, UV_ENOMEM);
                }
            } else {
                UVKCP_LOG_ERROR("Failed to allocate KCP handle");
                // Call connection callback with error
                kcp_server->connection_cb(NULL, UV_ENOMEM);
            }
        }
    } else {
        UVKCP_LOG_ERROR("Invalid handshake size: %zd", nread);
    }

    free(buf->base);
}

// TCP write callback for handshake
static void tcp_handshake_write_cb(uv_write_t *req, int status) {
    if (status != 0) {
        UVKCP_LOG_ERROR("TCP handshake write error: %d", status);
        // Close TCP connection on write error
        uv_close((uv_handle_t *)req->data, NULL);
    } else {
        UVKCP_LOG("KCP handshake response sent successfully");
        // Don't close TCP connection here - let client close it after reading the response
        // The client will close the connection in tcp_client_handshake_read_cb
    }
    free(req);
}


// Client implementation

// TCP connect callback for built-in handshake
static void tcp_connect_cb(uv_connect_t *req, int status) {
    kcp_context_t *ctx = (kcp_context_t *)req->data;

    if (status != 0) {
        UVKCP_LOG_ERROR("TCP connect error: %d", status);
        if (ctx->pending_connect_req && ctx->pending_connect_req->cb) {
            ctx->pending_connect_req->cb(ctx->pending_connect_req, status);
        }
        ctx->pending_connect_req = NULL;
        free(req);
        return;
    }

    UVKCP_LOG("TCP connected for KCP handshake");

    // Send handshake request
    kcp_handshake_t handshake;
    handshake.conv = 0;  // 0 means request for new conv

    // Create UDP socket for KCP communication first
    int domain = (ctx->server_addr.ss_family == AF_INET) ? AF_INET : AF_INET6;
    int sock = socket(domain, SOCK_DGRAM, 0);
    if (sock >= 0) {
        ctx->udp_fd = sock;

        // Get client's UDP socket address for proper peer communication
        struct sockaddr_storage client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        if (getsockname(ctx->udp_fd, (struct sockaddr*)&client_addr, &client_addr_len) == 0) {
            if (client_addr.ss_family == AF_INET) {
                struct sockaddr_in *addr_in = (struct sockaddr_in*)&client_addr;
                handshake.udp_port = addr_in->sin_port;
                handshake.addr_family = AF_INET;
                memcpy(handshake.peer_addr, &addr_in->sin_addr, 4);
            } else if (client_addr.ss_family == AF_INET6) {
                struct sockaddr_in6 *addr_in6 = (struct sockaddr_in6*)&client_addr;
                handshake.udp_port = addr_in6->sin6_port;
                handshake.addr_family = AF_INET6;
                memcpy(handshake.peer_addr, &addr_in6->sin6_addr, 16);
            } else {
                handshake.udp_port = 0;
                handshake.addr_family = AF_INET;
                memset(handshake.peer_addr, 0, sizeof(handshake.peer_addr));
            }
        } else {
            handshake.udp_port = 0;
            handshake.addr_family = AF_INET;
            memset(handshake.peer_addr, 0, sizeof(handshake.peer_addr));
        }
    } else {
        handshake.udp_port = 0;
        handshake.addr_family = AF_INET;
        memset(handshake.peer_addr, 0, sizeof(handshake.peer_addr));
    }

    handshake.timestamp = htonl(get_timestamp());
    handshake.nonce = htonl(generate_nonce());
    handshake.flags = 0;

    uv_buf_t handshake_buf = uv_buf_init((char *)&handshake, sizeof(handshake));
    uv_write_t *write_req = malloc(sizeof(uv_write_t));
    write_req->data = req->handle;

    uv_write(write_req, req->handle, &handshake_buf, 1, tcp_client_handshake_write_cb);

    // Start reading handshake response
    uv_read_start(req->handle, (uv_alloc_cb)echo_alloc, tcp_client_handshake_read_cb);

    free(req);
}


// TCP write callback for client handshake
static void tcp_client_handshake_write_cb(uv_write_t *req, int status) {
    if (status != 0) {
        UVKCP_LOG_ERROR("Client handshake write error: %d", status);
        uv_close((uv_handle_t *)req->data, NULL);
    } else {
        UVKCP_LOG("Client handshake request sent");
    }
    free(req);
}

// TCP read callback for client handshake
static void tcp_client_handshake_read_cb(uv_stream_t *tcp_client, ssize_t nread, const uv_buf_t *buf) {
    kcp_context_t *ctx = (kcp_context_t *)tcp_client->data;

    if (nread < 0) {
        UVKCP_LOG_ERROR("Client handshake read error: %zd", nread);
        if (ctx->pending_connect_req && ctx->pending_connect_req->cb) {
            ctx->pending_connect_req->cb(ctx->pending_connect_req, nread);
        }
        ctx->pending_connect_req = NULL;
        free(buf->base);
        uv_close((uv_handle_t *)tcp_client, NULL);
        return;
    }

    if (nread == sizeof(kcp_handshake_t)) {
        kcp_handshake_t *handshake = (kcp_handshake_t *)buf->base;
        uint32_t conv = ntohl(handshake->conv);

        UVKCP_LOG("Received KCP handshake response: conv=%u", conv);

        // Create KCP object with exchanged conversation ID
        uvkcp_t *client = ctx->pending_connect_req->handle;

        if (ctx) {
            // Use the UDP socket already created in TCP connect callback
            if (ctx->udp_fd >= 0) {
                // Create KCP instance with exchanged conversation ID
                ctx->kcp = ikcp_create(conv, ctx);
                if (ctx->kcp) {
                    // Configure KCP
                    ikcp_setoutput(ctx->kcp, kcp_output);
                    ikcp_nodelay(ctx->kcp, 1, 10, 2, 1);
                    ikcp_wndsize(ctx->kcp, 128, 128);
                    ikcp_setmtu(ctx->kcp, 1400);

                    // Set peer address from handshake
                    if (handshake->addr_family == AF_INET) {
                        struct sockaddr_in *peer = (struct sockaddr_in *)&ctx->peer_addr;
                        peer->sin_family = AF_INET;
                        peer->sin_port = handshake->udp_port;
                        memcpy(&peer->sin_addr, handshake->peer_addr, 4);
                        ctx->peer_addr_len = sizeof(struct sockaddr_in);
                    } else {
                        struct sockaddr_in6 *peer = (struct sockaddr_in6 *)&ctx->peer_addr;
                        peer->sin6_family = AF_INET6;
                        peer->sin6_port = handshake->udp_port;
                        memcpy(&peer->sin6_addr, handshake->peer_addr, 16);
                        ctx->peer_addr_len = sizeof(struct sockaddr_in6);
                    }

                    // Mark as connected
                    ctx->is_connected = 1;

                    // Open the KCP stream
                    if (kcp__stream_open(client, ctx->udp_fd, UVKCP_FLAG_READABLE | UVKCP_FLAG_WRITABLE) == 0) {
                        UVKCP_LOG("KCP client created successfully with conv=%u", conv);
                    } else {
                        UVKCP_LOG_ERROR("Failed to open KCP stream");
                        close(ctx->udp_fd);
                        ikcp_release(ctx->kcp);
                        ctx->kcp = NULL;
                        ctx->is_connected = 0; // Reset connection state on failure
                        // Connection will fail, but callback will be called below
                    }
                } else {
                    UVKCP_LOG_ERROR("Failed to create KCP instance");
                    close(ctx->udp_fd);
                    ctx->is_connected = 0; // Reset connection state on failure
                    // Connection will fail, but callback will be called below
                }
            } else {
                UVKCP_LOG_ERROR("Failed to create UDP socket");
                // Connection will fail, but callback will be called below
            }
        }

        // Close TCP connection
        uv_close((uv_handle_t *)tcp_client, NULL);

        // Call connection callback
        if (ctx->pending_connect_req && ctx->pending_connect_req->cb) {
            // Check if connection was successfully established
            if (ctx->is_connected && ctx->kcp != NULL) {
                ctx->pending_connect_req->cb(ctx->pending_connect_req, 0);
            } else {
                // Connection failed during setup
                ctx->pending_connect_req->cb(ctx->pending_connect_req, UV_EIO);
            }
        }

        ctx->pending_connect_req = NULL;
    } else {
        UVKCP_LOG_ERROR("Invalid handshake response size: %zd", nread);
        if (ctx->pending_connect_req && ctx->pending_connect_req->cb) {
            ctx->pending_connect_req->cb(ctx->pending_connect_req, UV_EINVAL);
        }
        uv_close((uv_handle_t *)tcp_client, NULL);
        ctx->pending_connect_req = NULL;
    }

    free(buf->base);
}

