//////////////////////////////////////////////////////
// KCP interfaces definition
// Copyright 2020, tom zhou<appnet.link@gmail.com>
//////////////////////////////////////////////////////

#ifndef __UVKCP_H__
#define __UVKCP_H__
#ifdef __cplusplus
extern "C" {
#endif

#include "uv.h"
#include "queue.h"
#include "kcp/ikcp.h"
#include <stdio.h>

// Debug logging
///#define UVKCP_DEBUG 1

#ifdef UVKCP_DEBUG
#define UVKCP_LOG(fmt, ...) printf("[UVKCP] %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#define UVKCP_LOG_FUNC(fmt, ...) printf("[UVKCP] %s:%d %s: " fmt "\n", __FILE__, __LINE__, __func__, ##__VA_ARGS__)
#define UVKCP_LOG_ERROR(fmt, ...) printf("[UVKCP ERROR] %s:%d %s: " fmt "\n", __FILE__, __LINE__, __func__, ##__VA_ARGS__)
#else
#define UVKCP_LOG(fmt, ...)
#define UVKCP_LOG_FUNC(fmt, ...)
#define UVKCP_LOG_ERROR(fmt, ...)
#endif

// request type
struct uvkcp_connect_s;
struct uvkcp_shutdown_s;
struct uvkcp_write_s;
struct uvkcp_req_s;
struct uvkcp_s;

// callback
typedef void (* uvkcp_connect_cb)(struct uvkcp_connect_s *req, int status);
typedef void (* uvkcp_shutdown_cb)(struct uvkcp_shutdown_s *req, int status);
typedef void (* uvkcp_write_cb)(struct uvkcp_write_s *req, int status);
typedef void (* uvkcp_read_cb)(struct uvkcp_s *stream, ssize_t nread, const uv_buf_t *buf);
typedef void (* uvkcp_connection_cb)(struct uvkcp_s *server, int status);

// Forward declarations
struct kcp_context_s;
typedef struct kcp_context_s kcp_context_t;
struct uvkcp_s;
typedef struct uvkcp_s uvkcp_t;

// Performance monitoring structure
typedef struct uvkcp_netperf_s {
    int64_t msTimeStamp;          // Timestamp of the performance data
    int64_t pktSentTotal;         // Total packets sent
    int64_t pktRecvTotal;         // Total packets received
    int pktSndLossTotal;          // Total send packet loss
    int pktRcvLossTotal;          // Total receive packet loss
    int pktRetransTotal;          // Total packet retransmissions
    int pktSentACKTotal;          // Total ACK packets sent
    int pktRecvACKTotal;          // Total ACK packets received
    int pktSentNAKTotal;          // Total NAK packets sent
    int pktRecvNAKTotal;          // Total NAK packets received
    int64_t usSndDurationTotal;   // Total send duration in microseconds
    double mbpsSendRate;          // Send rate in Mbps
    double mbpsRecvRate;          // Receive rate in Mbps
    double mbpsBandwidth;         // Estimated bandwidth in Mbps
    int pktFlowWindow;            // Flow window size
    int pktCongestionWindow;      // Congestion window size
    int pktFlightSize;            // Packets in flight
    int msRTT;                    // Round-trip time in milliseconds
    int byteAvailSndBuf;          // Available send buffer bytes
    int byteAvailRcvBuf;          // Available receive buffer bytes
} uvkcp_netperf_t;

// Performance monitoring callbacks
typedef void (*uvkcp_perf_cb)(uvkcp_t *handle, const uvkcp_netperf_t *perf);

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
    uint32_t update_interval;  // Interval for KCP updates in milliseconds

    // Server-specific fields
    int backlog;
    uvkcp_connection_cb connection_cb;
    struct sockaddr_storage server_addr;
    socklen_t server_addr_len;

    // TCP handshake support (mandatory)
    uv_tcp_t tcp_server;
    uv_tcp_t tcp_client;
    uint32_t next_conv;
    uint32_t base_conv;  // Instance-specific base for conversation ID generation
    struct uvkcp_connect_s *pending_connect_req;

    // Server reference for client connections (for registry cleanup)
    kcp_context_t *server_ctx;

    // Conversation registry for shared socket multiplexing
    void *conv_registry;  // Hash table of conversation_id -> uvkcp_t*

    // Performance tracking
    int64_t pktSentTotal;
    int64_t pktRecvTotal;
    int pktSndLossTotal;
    int pktRcvLossTotal;
    int pktRetransTotal;
    int64_t bytesSentTotal;
    int64_t bytesRecvTotal;
    IUINT32 lastUpdateTime;

    // Performance optimization flags
    int high_performance_mode;  // Enable aggressive timer and buffer optimizations

    // Performance monitoring
    uvkcp_perf_cb perf_cb;      // Performance callback function
    uv_timer_t perf_timer;      // Timer for periodic performance callbacks
    int perf_interval;          // Performance callback interval in ms
};

// KCP handshake protocol structures
struct kcp_handshake_s {
    uint32_t conv;           // KCP conversation ID (0 = request new)
    uint16_t udp_port;       // UDP port for KCP communication
    uint8_t  addr_family;    // Address family (AF_INET or AF_INET6)
    uint8_t  peer_addr[16];  // Peer IP address for UDP communication
    uint32_t timestamp;      // Timestamp for handshake
    uint32_t nonce;          // Random nonce for security
    uint32_t flags;          // Handshake flags
};

typedef struct kcp_handshake_s kcp_handshake_t;

// KCP server context for TCP handshake
struct kcp_server_ctx_s {
    uv_tcp_t tcp_server;     // TCP server for handshake
    uv_loop_t *loop;
    uvkcp_connection_cb connection_cb;
    int backlog;
    uint32_t next_conv;      // Next conversation ID to assign
};

typedef struct kcp_server_ctx_s kcp_server_ctx_t;

// KCP client context for TCP handshake
struct kcp_client_ctx_s {
    uv_tcp_t tcp_client;     // TCP client for handshake
    uv_loop_t *loop;
    uvkcp_connect_cb connect_cb;
    struct uvkcp_connect_s *connect_req;
    uint32_t conv;           // Assigned conversation ID
};

typedef struct kcp_client_ctx_s kcp_client_ctx_t;

// state flags
enum uvkcp_flags_e
{
    UVKCP_FLAG_READABLE  = 0x0001,
    UVKCP_FLAG_WRITABLE  = 0x0002,
    UVKCP_FLAG_SHUT      = 0x0004,
    UVKCP_FLAG_SHUTTING  = 0x0008,
    UVKCP_FLAG_CLOSING   = 0x0010,
    UVKCP_FLAG_CLOSED    = 0x0020,

    UVKCP_FLAG_REUSEADDR = 0x0100,
    UVKCP_FLAG_REUSEABLE = 0x0200,
    UVKCP_FLAG_RENDEZ    = 0x0400,

    UVKCP_FLAG_IPV6ONLY  = 0x1000,
};

// uvkcp_t
struct uvkcp_s
{
    // inherit from uv_poll_t
    uv_poll_t poll;
    uv_loop_t *aloop;
    int flags;

    // uv_stream_t compatibile fields
    size_t write_queue_size;
    uv_alloc_cb alloc_cb;
    uvkcp_read_cb read_cb;

    struct uvkcp_connect_s *connect_req;
    struct uvkcp_shutdown_s *shutdown_req;
    void *write_queue[2];
    void *write_completed_queue[2];
    uvkcp_connection_cb connection_cb;
    int delayed_error;
    uv_os_sock_t fd;
    uv_os_sock_t accepted_fd;

    // KCP specific fields
    int kcpfd;
    int accepted_kcpfd;
    uv_os_sock_t udpfd;
    uv_os_sock_t accepted_udpfd;

    // Private KCP context
    void *kcp_ctx;
};

// request type
enum uvkcp_req_e
{
    UVKCP_REQ_CONNECT  = 1,
    UVKCP_REQ_SHUTDOWN = 2,
    UVKCP_REQ_WRITE    = 3,
};
struct uvkcp_req_s 
{
    int type;
    int error;
    uvkcp_t *handle;
    void *data;
};
typedef struct uvkcp_req_s uvkcp_req_t;

// uvkcp_connect_t
struct uvkcp_connect_s
{
    int type;
    int error;
    uvkcp_t *handle;
    void *data;

    void *queue[2];
    uvkcp_connect_cb cb;
};
typedef struct uvkcp_connect_s uvkcp_connect_t;

// uvkcp_shutdown_t
struct uvkcp_shutdown_s {
    int type;
    int error;
    uvkcp_t *handle;
    void *data;

    uvkcp_shutdown_cb cb;
};
typedef struct uvkcp_shutdown_s uvkcp_shutdown_t;

// uvkcp_write_t
struct uvkcp_write_s {
    int type;
    int error;
    uvkcp_t *handle;
    void *data;

    void *queue[2];
    unsigned int write_index;
    uv_buf_t *bufs;
    unsigned int nbufs;
    uv_buf_t bufsml[4];
    uvkcp_write_cb cb;
};
typedef struct uvkcp_write_s uvkcp_write_t;

// Public API
UV_EXTERN int uvkcp_init(uv_loop_t *loop, uvkcp_t *handle);
UV_EXTERN int uvkcp_open(uvkcp_t *handle, uv_os_sock_t sock);

UV_EXTERN int uvkcp_nodelay(uvkcp_t *handle, int enable, int interval, int resend, int nc);
UV_EXTERN int uvkcp_wndsize(uvkcp_t *handle, int sndwnd, int rcvwnd);
UV_EXTERN int uvkcp_setmtu(uvkcp_t *handle, int mtu);
UV_EXTERN int uvkcp_keepalive(uvkcp_t *handle,
                              int enable,
                              unsigned int delay);
UV_EXTERN int uvkcp_simultaneous_accepts(uvkcp_t *handle, int enable);

UV_EXTERN int uvkcp_bind(uvkcp_t *handle, 
                         const struct sockaddr *addr, 
                         int reuseaddr, 
                         int reuseable);

UV_EXTERN int uvkcp_getsockname(const uvkcp_t* handle,
                                struct sockaddr* name,
                                int* namelen);

UV_EXTERN int uvkcp_getpeername(const uvkcp_t *handle, 
                                struct sockaddr *name, 
                                int *namelen);

UV_EXTERN int uvkcp_close(uvkcp_t *handle, uv_close_cb close_cb);

UV_EXTERN int uvkcp_connect(uvkcp_connect_t *req, 
                            uvkcp_t *handle, 
                            const struct sockaddr *addr, 
                            uvkcp_connect_cb cb);

UV_EXTERN int uvkcp_shutdown(uvkcp_shutdown_t *req,
                             uvkcp_t *handle,
                             uvkcp_shutdown_cb cb);

UV_EXTERN size_t uvkcp_get_write_queue_size(const uvkcp_t *stream);

UV_EXTERN int uvkcp_listen(uvkcp_t *stream,
                           int backlog,
                           uvkcp_connection_cb cb);

UV_EXTERN int uvkcp_accept(uvkcp_t *server, uvkcp_t *client);

UV_EXTERN int uvkcp_read_start(uvkcp_t *,
                               uv_alloc_cb alloc_cb,
                               uvkcp_read_cb read_cb);

UV_EXTERN int uvkcp_read_stop(uvkcp_t *);

UV_EXTERN int uvkcp_write(uvkcp_write_t *req,
                          uvkcp_t *handle,
                          const uv_buf_t bufs[],
                          unsigned int nbufs,
                          uvkcp_write_cb cb);

UV_EXTERN int uvkcp_write2(uvkcp_write_t* req,
                           uvkcp_t* handle,
                           const uv_buf_t bufs[],
                           unsigned int nbufs,
                           uv_stream_t* send_handle, // !!! not used, for compatibility with Node.js
                           uvkcp_write_cb cb);

UV_EXTERN int uvkcp_try_write(uvkcp_t *handle,
                              const uv_buf_t bufs[],
                              unsigned int nbufs);

UV_EXTERN int uvkcp_is_readable(uvkcp_t *handle);

UV_EXTERN int uvkcp_is_writable(uvkcp_t *handle);

UV_EXTERN int uvkcp_set_blocking(uvkcp_t *handle, int blocking);

// Performance optimization APIs
UV_EXTERN int uvkcp_set_high_performance(uvkcp_t *handle, int enable);




// internal api
UV_EXTERN int  uvkcp_translate_kcp_error(void);
UV_EXTERN int  kcp__nonblock(int kcpfd, int set);
UV_EXTERN int  kcp__accept(int kcpfd);
UV_EXTERN void kcp__stream_init(uv_loop_t* loop, uvkcp_t* stream);
UV_EXTERN int  kcp__stream_open(uvkcp_t* kcp, uv_os_sock_t fd, int flags);
UV_EXTERN void kcp__stream_destroy(uvkcp_t* stream);

#ifdef __cplusplus
}
#endif
#endif // __UVKCP_H__