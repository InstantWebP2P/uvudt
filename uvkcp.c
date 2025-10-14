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

//#define KCP_DEBUG 1

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

    return 0;
}

// Initialize KCP handle
int uvkcp_init(uv_loop_t *loop, uvkcp_t *handle) {
    static int _initialized = 0;

    if (!_initialized) {
        // KCP library initialization if needed
        _initialized = 1;
    }

    // Initialize stream
    kcp__stream_init(loop, handle);

    // Allocate KCP context
    kcp_context_t *ctx = (kcp_context_t *)malloc(sizeof(kcp_context_t));
    if (!ctx) {
        return UV_ENOMEM;
    }

    memset(ctx, 0, sizeof(kcp_context_t));
    ctx->loop = loop;
    ctx->udp_fd = -1;
    ctx->is_connected = 0;
    ctx->is_listening = 0;

    // Store context in handle
    handle->poll.data = ctx;

    return 0;
}

// Open KCP handle with existing UDP socket
int uvkcp_open(uvkcp_t *handle, uv_os_sock_t sock) {
    kcp_context_t *ctx = (kcp_context_t *)handle->poll.data;
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
    ikcp_nodelay(ctx->kcp, 1, 10, 2, 1);  // Enable nodelay
    ikcp_wndsize(ctx->kcp, 128, 128);     // Set window size

    return kcp__stream_open(handle, sock, 0);
}

// Bind KCP to address
int uvkcp_bind(uvkcp_t *handle, const struct sockaddr *addr, int reuseaddr, int reuseable) {
    kcp_context_t *ctx = (kcp_context_t *)handle->poll.data;
    if (!ctx) {
        return UV_EINVAL;
    }

    // Create UDP socket
    int domain = addr->sa_family;
    int sock = socket(domain, SOCK_DGRAM, 0);
    if (sock < 0) {
        return uv_translate_sys_error(errno);
    }

    // Set socket options
    if (reuseaddr) {
        int optval = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    }

    // Bind socket
    socklen_t addrlen = (domain == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
    if (bind(sock, addr, addrlen) < 0) {
        close(sock);
        return uv_translate_sys_error(errno);
    }

    ctx->udp_fd = sock;

    // Create KCP instance
    ctx->kcp = ikcp_create(0, ctx);
    if (!ctx->kcp) {
        close(sock);
        return UV_ENOMEM;
    }

    ikcp_setoutput(ctx->kcp, kcp_output);
    ikcp_nodelay(ctx->kcp, 1, 10, 2, 1);
    ikcp_wndsize(ctx->kcp, 128, 128);

    return kcp__stream_open(handle, sock, 0);
}

// Connect to remote address
int uvkcp_connect(uvkcp_connect_t *req, uvkcp_t *handle, const struct sockaddr *addr, uvkcp_connect_cb cb) {
    kcp_context_t *ctx = (kcp_context_t *)handle->poll.data;
    if (!ctx) {
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

    // Call connection callback immediately (KCP is connectionless)
    if (cb) {
        cb(req, 0);
    }

    return 0;
}

// Listen for incoming connections
int uvkcp_listen(uvkcp_t *stream, int backlog, uvkcp_connection_cb cb) {
    kcp_context_t *ctx = (kcp_context_t *)stream->poll.data;
    if (!ctx) {
        return UV_EINVAL;
    }

    stream->connection_cb = cb;
    ctx->is_listening = 1;

    return 0;
}

// Close KCP handle
int uvkcp_close(uvkcp_t *handle, uv_close_cb close_cb) {
    kcp_context_t *ctx = (kcp_context_t *)handle->poll.data;
    if (!ctx) {
        return UV_EINVAL;
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
    handle->poll.data = NULL;

    // Call stream destroy
    kcp__stream_destroy(handle);

    return 0;
}

// Set KCP nodelay option
int uvkcp_nodelay(uvkcp_t *handle, int enable) {
    kcp_context_t *ctx = (kcp_context_t *)handle->poll.data;
    if (!ctx || !ctx->kcp) {
        return UV_EINVAL;
    }

    ikcp_nodelay(ctx->kcp, enable, 10, 2, 1);
    return 0;
}

// Set KCP window size
int uvkcp_wndsize(uvkcp_t *handle, int sndwnd, int rcvwnd) {
    kcp_context_t *ctx = (kcp_context_t *)handle->poll.data;
    if (!ctx || !ctx->kcp) {
        return UV_EINVAL;
    }

    ikcp_wndsize(ctx->kcp, sndwnd, rcvwnd);
    return 0;
}

// Translate KCP error to libuv error
int uvkcp_translate_kcp_error(void) {
    // KCP doesn't have detailed error codes, return generic error
    return UV_EIO;
}