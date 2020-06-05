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
    // inherit from uv_async_t
    uv_async_t poll;
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
};
typedef struct uvkcp_s uvkcp_t;

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

UV_EXTERN int uvkcp_nodelay(uvkcp_t *handle, int enable);
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

UV_EXTERN int uvkcp_punchhole(uvkcp_t *handle, 
                              const struct sockaddr *addr, 
                              int from, 
                              int to);

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

// set KCP socket qos/priority
UV_EXTERN int uvkcp_setqos(uvkcp_t *handle, int qos);

// set KCP socket maxim bandwidth bytes/second
UV_EXTERN int uvkcp_setmbw(uvkcp_t *handle, int64_t mbw);

// set KCP socket maxim buffer size
UV_EXTERN int uvkcp_setmbs(uvkcp_t *handle, 
                           int mfc, 
                           int mkcp, 
                           int mudp);

// set KCP socket security mode
UV_EXTERN int uvkcp_setsec(uvkcp_t *handle, 
                           int mode, 
                           unsigned char keybuf[], 
                           int keylen);

// binding kcp socket on existing udp socket/fd
UV_EXTERN int uvkcp_bindfd(uvkcp_t *handle, 
                           uv_os_sock_t udpfd, 
                           int reuseaddr, 
                           int reuseable);

// retrieve udp socket/fd associated with kcp socket
UV_EXTERN int uvkcp_udpfd(uvkcp_t *handle, uv_os_sock_t *udpfd);

// set if REUSE existing ADDRESS created by previous kcp socket
UV_EXTERN int uvkcp_reuseaddr(uvkcp_t *handle, int yes);

// set if ADDRESS reusable for another kcp socket
UV_EXTERN int uvkcp_reuseable(uvkcp_t *handle, int yes);

// KCP network performance track
typedef struct uvkcp_netperf_s
{
    // global measurements
    int64_t msTimeStamp;        // time since the KCP entity is started, in milliseconds
    int64_t pktSentTotal;       // total number of sent data packets, including retransmissions
    int64_t pktRecvTotal;       // total number of received packets
    int pktSndLossTotal;        // total number of lost packets (sender side)
    int pktRcvLossTotal;        // total number of lost packets (receiver side)
    int pktRetransTotal;        // total number of retransmitted packets
    int pktSentACKTotal;        // total number of sent ACK packets
    int pktRecvACKTotal;        // total number of received ACK packets
    int pktSentNAKTotal;        // total number of sent NAK packets
    int pktRecvNAKTotal;        // total number of received NAK packets
    int64_t usSndDurationTotal; // total time duration when KCP is sending data (idle time exclusive)

    // local measurements
    int64_t pktSent;            // number of sent data packets, including retransmissions
    int64_t pktRecv;            // number of received packets
    int pktSndLoss;             // number of lost packets (sender side)
    int pktRcvLoss;             // number of lost packets (receiver side)
    int pktRetrans;             // number of retransmitted packets
    int pktSentACK;             // number of sent ACK packets
    int pktRecvACK;             // number of received ACK packets
    int pktSentNAK;             // number of sent NAK packets
    int pktRecvNAK;             // number of received NAK packets
    double mbpsSendRate;        // sending rate in Mb/s
    double mbpsRecvRate;        // receiving rate in Mb/s
    int64_t usSndDuration;      // busy sending time (i.e., idle time exclusive)

    // instant measurements
    double usPktSndPeriod;      // packet sending period, in microseconds
    int pktFlowWindow;          // flow window size, in number of packets
    int pktCongestionWindow;    // congestion window size, in number of packets
    int pktFlightSize;          // number of packets on flight
    double msRTT;               // RTT, in milliseconds
    double mbpsBandwidth;       // estimated bandwidth, in Mb/s
    int byteAvailSndBuf;        // available KCP sender buffer size
    int byteAvailRcvBuf;        // available KCP receiver buffer size
} uvkcp_netperf_t;

UV_EXTERN int uvkcp_getperf(uvkcp_t *handle, uvkcp_netperf_t *perf, int clear);

// internal api
UV_EXTERN int  uvkcp_translate_kcp_error(void);
UV_EXTERN int  kcp__nonblock(int kcpfd, int set);
UV_EXTERN int  kcp__accept(int kcpfd);
UV_EXTERN void kcp__stream_init(uv_loop_t* loop, uvkcp_t* stream);
UV_EXTERN int  kcp__stream_open(uvkcp_t* kcp, uv_os_sock_t fd, int flags);

#ifdef __cplusplus
}
#endif
#endif // __UVKCP_H__