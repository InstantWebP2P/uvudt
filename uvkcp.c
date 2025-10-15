//////////////////////////////////////////////////////
// KCP interfaces implementation
// Copyright 2020, tom zhou<appnet.link@gmail.com>
//////////////////////////////////////////////////////

#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include "kcp/ikcp.h"
#include "uvkcp.h"

#define KCP_DEBUG 1

void kcp__stream_io(uv_poll_t *handle, int status, int events);

// KCP context structure
struct kcp_context_s {
    ikcpcb *kcp;
    uv_os_sock_t udp_fd;
    struct sockaddr_storage peer_addr;
    socklen_t peer_addr_len;
    uv_loop_t *loop;
    uv_poll_t poll_handle;
    uv_timer_t timer_handle;
    int is_connected;
    int is_listening;
    int timer_active;

    // Performance tracking
    int64_t pktSentTotal;
    int64_t pktRecvTotal;
    int pktSndLossTotal;
    int pktRcvLossTotal;
    int pktRetransTotal;
    int64_t bytesSentTotal;
    int64_t bytesRecvTotal;
    IUINT32 lastUpdateTime;
};

typedef struct kcp_context_s kcp_context_t;

// KCP output function - sends data over UDP
static int kcp_output(const char *buf, int len, ikcpcb *kcp, void *user) {
    kcp_context_t *ctx = (kcp_context_t *)user;

    if (ctx->udp_fd == -1) {
        return -1;
    }

    ssize_t sent = sendto(ctx->udp_fd, buf, len, 0,
                         (struct sockaddr *)&ctx->peer_addr,
                         ctx->peer_addr_len);

    if (sent < 0) {
        return -1;
    }

    // Track statistics
    ctx->pktSentTotal++;
    ctx->bytesSentTotal += sent;

    return (int)sent;
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

    // Initialize statistics
    ctx->pktSentTotal = 0;
    ctx->pktRecvTotal = 0;
    ctx->pktSndLossTotal = 0;
    ctx->pktRcvLossTotal = 0;
    ctx->pktRetransTotal = 0;
    ctx->bytesSentTotal = 0;
    ctx->bytesRecvTotal = 0;
    ctx->lastUpdateTime = 0;

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

    // Create KCP instance with default conv (0 for now)
    ctx->kcp = ikcp_create(0, ctx);
    if (!ctx->kcp) {
        return UV_ENOMEM;
    }

    // Set output callback
    ikcp_setoutput(ctx->kcp, kcp_output);

    // Configure KCP for better performance
    // nodelay: 1=enable nodelay, interval=10ms, resend=2, congestion control=1
    ikcp_nodelay(ctx->kcp, 1, 10, 2, 1);
    // Set window sizes: send window=128, receive window=128
    ikcp_wndsize(ctx->kcp, 128, 128);
    // Set maximum transmission unit
    ikcp_setmtu(ctx->kcp, 1400);

    return kcp__stream_open(handle, sock, UVKCP_FLAG_READABLE | UVKCP_FLAG_WRITABLE);
}

// Bind KCP to address
int uvkcp_bind(uvkcp_t *handle, const struct sockaddr *addr, int reuseaddr, int reuseable) {
    UVKCP_LOG_FUNC("Binding KCP handle");

    kcp_context_t *ctx = (kcp_context_t *)handle->kcp_ctx;
    if (!ctx) {
        UVKCP_LOG_ERROR("Invalid KCP context");
        return UV_EINVAL;
    }

    // Create UDP socket
    int domain = addr->sa_family;
    int sock = socket(domain, SOCK_DGRAM, 0);
    if (sock < 0) {
        UVKCP_LOG_ERROR("Failed to create UDP socket: %s", strerror(errno));
        return uv_translate_sys_error(errno);
    }

    // Set socket options
    if (reuseaddr) {
        int optval = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
        UVKCP_LOG("Set SO_REUSEADDR on socket");
    }

    // Bind socket
    socklen_t addrlen = (domain == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
    if (bind(sock, addr, addrlen) < 0) {
        UVKCP_LOG_ERROR("Failed to bind socket: %s", strerror(errno));
        close(sock);
        return uv_translate_sys_error(errno);
    }

    ctx->udp_fd = sock;
    UVKCP_LOG("Bound to UDP socket fd=%d", sock);

    // Create KCP instance
    ctx->kcp = ikcp_create(0, ctx);
    if (!ctx->kcp) {
        UVKCP_LOG_ERROR("Failed to create KCP instance");
        close(sock);
        return UV_ENOMEM;
    }

    ikcp_setoutput(ctx->kcp, kcp_output);
    ikcp_nodelay(ctx->kcp, 1, 10, 2, 1);
    ikcp_wndsize(ctx->kcp, 128, 128);
    ikcp_setmtu(ctx->kcp, 1400);

    UVKCP_LOG("KCP instance created and configured");
    return kcp__stream_open(handle, sock, UVKCP_FLAG_READABLE | UVKCP_FLAG_WRITABLE);
}

// Connect to remote address
int uvkcp_connect(uvkcp_connect_t *req, uvkcp_t *handle, const struct sockaddr *addr, uvkcp_connect_cb cb) {
    UVKCP_LOG_FUNC("Connecting KCP handle");

    kcp_context_t *ctx = (kcp_context_t *)handle->kcp_ctx;
    if (!ctx) {
        UVKCP_LOG_ERROR("Invalid KCP context");
        return UV_EINVAL;
    }

    // Store peer address
    memcpy(&ctx->peer_addr, addr, sizeof(struct sockaddr_storage));
    ctx->peer_addr_len = (addr->sa_family == AF_INET) ?
                         sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);

    // Setup connection request
    req->handle = handle;
    req->cb = cb;
    handle->connect_req = req;

    // Mark as connected
    ctx->is_connected = 1;

    // For KCP (connectionless protocol over UDP), we immediately call the connection callback
    // since there's no actual connection establishment like TCP
    if (cb) {
        UVKCP_LOG("Calling connection callback immediately");
        cb(req, 0);  // Success
    }

    UVKCP_LOG("KCP connection established");
    return 0;
}

// Listen for incoming connections
int uvkcp_listen(uvkcp_t *stream, int backlog, uvkcp_connection_cb cb) {
    kcp_context_t *ctx = (kcp_context_t *)stream->kcp_ctx;
    if (!ctx) {
        return UV_EINVAL;
    }

    stream->connection_cb = cb;
    ctx->is_listening = 1;

    // For KCP (connectionless protocol over UDP), we immediately call the connection callback
    // since there's no actual connection establishment like TCP
    if (cb) {
        UVKCP_LOG("Calling connection callback immediately");
        cb(stream, 0);  // Success
    }

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
        ikcp_release(ctx->kcp);
        ctx->kcp = NULL;
    }

    // Close UDP socket
    if (ctx->udp_fd != -1) {
        close(ctx->udp_fd);
        ctx->udp_fd = -1;
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

int uvkcp_punchhole(uvkcp_t *handle, const struct sockaddr *addr, int from, int to) {
    // KCP doesn't support punchhole
    return UV_ENOSYS;
}

int uvkcp_setqos(uvkcp_t *handle, int qos) {
    // KCP doesn't support QoS
    return 0;
}

int uvkcp_setmbw(uvkcp_t *handle, int64_t mbw) {
    // KCP doesn't support bandwidth limiting
    return 0;
}

int uvkcp_setmbs(uvkcp_t *handle, int mfc, int mkcp, int mudp) {
    // KCP doesn't support buffer size configuration
    return 0;
}

int uvkcp_setsec(uvkcp_t *handle, int mode, unsigned char keybuf[], int keylen) {
    // KCP doesn't support security modes
    return UV_ENOSYS;
}

int uvkcp_bindfd(uvkcp_t *handle, uv_os_sock_t udpfd, int reuseaddr, int reuseable) {
    kcp_context_t *ctx = (kcp_context_t *)handle->kcp_ctx;
    if (!ctx) {
        return UV_EINVAL;
    }

    ctx->udp_fd = udpfd;

    // Create KCP instance
    ctx->kcp = ikcp_create(0, ctx);
    if (!ctx->kcp) {
        return UV_ENOMEM;
    }

    ikcp_setoutput(ctx->kcp, kcp_output);
    ikcp_nodelay(ctx->kcp, 1, 10, 2, 1);
    ikcp_wndsize(ctx->kcp, 128, 128);
    ikcp_setmtu(ctx->kcp, 1400);

    return kcp__stream_open(handle, udpfd, UVKCP_FLAG_READABLE | UVKCP_FLAG_WRITABLE);
}

int uvkcp_udpfd(uvkcp_t *handle, uv_os_sock_t *udpfd) {
    kcp_context_t *ctx = (kcp_context_t *)handle->kcp_ctx;
    if (!ctx || ctx->udp_fd == -1) {
        return UV_EINVAL;
    }

    *udpfd = ctx->udp_fd;
    return 0;
}

int uvkcp_reuseaddr(uvkcp_t *handle, int yes) {
    // Already handled in bind
    return 0;
}

int uvkcp_reuseable(uvkcp_t *handle, int yes) {
    // Already handled in bind
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