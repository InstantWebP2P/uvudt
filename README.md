# uvkcp

KCP Transport with libUV - High-performance UDP-based reliable transport protocol

## Overview

uvkcp is a libuv-based implementation of the KCP (A Fast and Reliable ARQ Protocol) transport protocol. It provides reliable, high-performance UDP-based communication with TCP-like reliability and stream-oriented semantics.

### Key Features

- **Reliable UDP Transport**: KCP protocol over UDP with automatic retransmission and congestion control
- **libUV Integration**: Full integration with libuv event loop for asynchronous I/O
- **TCP Handshake Protocol**: Uses TCP for initial connection establishment and UDP for data transfer
- **Performance Monitoring**: Comprehensive performance statistics and monitoring callbacks
- **Adaptive Timers**: Dynamic timer intervals based on network conditions using `ikcp_check()`
- **High-Performance Mode**: Aggressive optimization settings for low-latency scenarios
- **IPv4/IPv6 Support**: Full support for both IPv4 and IPv6 addressing
- **Stream-Oriented API**: Compatible with libuv stream semantics

## Architecture

uvkcp implements a hybrid approach:

1. **TCP Handshake**: Initial connection establishment via TCP for reliable handshake
2. **UDP Data Transfer**: High-performance data transfer using KCP over UDP
3. **libUV Integration**: All operations integrated with libuv event loop

## API Reference

### Core Initialization

```c
int uvkcp_init(uv_loop_t *loop, uvkcp_t *handle);
```
Initialize a KCP handle with the specified libuv event loop.

### Connection Management

```c
int uvkcp_bind(uvkcp_t *handle, const struct sockaddr *addr, int reuseaddr, int reuseable);
int uvkcp_listen(uvkcp_t *stream, int backlog, uvkcp_connection_cb cb);
int uvkcp_accept(uvkcp_t *server, uvkcp_t *client);
int uvkcp_connect(uvkcp_connect_t *req, uvkcp_t *handle, const struct sockaddr *addr, uvkcp_connect_cb cb);
```

### I/O Operations

```c
int uvkcp_read_start(uvkcp_t *stream, uv_alloc_cb alloc_cb, uvkcp_read_cb read_cb);
int uvkcp_read_stop(uvkcp_t *stream);
int uvkcp_write(uvkcp_write_t *req, uvkcp_t *handle, const uv_buf_t bufs[], unsigned int nbufs, uvkcp_write_cb cb);
int uvkcp_try_write(uvkcp_t *handle, const uv_buf_t bufs[], unsigned int nbufs);
```

### KCP Protocol Configuration

```c
int uvkcp_nodelay(uvkcp_t *handle, int enable, int interval, int resend, int nc);
int uvkcp_wndsize(uvkcp_t *handle, int sndwnd, int rcvwnd);
int uvkcp_setmtu(uvkcp_t *handle, int mtu);
```

### Performance Optimization

```c
int uvkcp_set_high_performance(uvkcp_t *handle, int enable);
int uvkcp_set_perf_callback(uvkcp_t *handle, uvkcp_perf_cb cb, int interval_ms);
int uvkcp_getperf(uvkcp_t *handle, uvkcp_netperf_t *perf, int clear);
```

### Utility Functions

```c
int uvkcp_getsockname(const uvkcp_t *handle, struct sockaddr *name, int *namelen);
int uvkcp_getpeername(const uvkcp_t *handle, struct sockaddr *name, int *namelen);
int uvkcp_close(uvkcp_t *handle, uv_close_cb close_cb);
int uvkcp_is_readable(uvkcp_t *handle);
int uvkcp_is_writable(uvkcp_t *handle);
```

## Performance Monitoring

### Performance Structure

```c
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
```

### Performance Callback

```c
typedef void (*uvkcp_perf_cb)(uvkcp_t *handle, const uvkcp_netperf_t *perf);
```

## Usage Examples

### Server Example

```c
#include "uvkcp.h"

void on_connection(uvkcp_t *server, int status) {
    if (status != 0) {
        fprintf(stderr, "Connection error: %s\n", uv_strerror(status));
        return;
    }

    uvkcp_t *client = malloc(sizeof(uvkcp_t));
    if (uvkcp_init(server->aloop, client) != 0) {
        fprintf(stderr, "Failed to initialize client\n");
        return;
    }

    if (uvkcp_accept(server, client) != 0) {
        fprintf(stderr, "Failed to accept connection\n");
        uvkcp_close(client, NULL);
        return;
    }

    // Start reading from client
    uvkcp_read_start(client, alloc_buffer, on_read);
}

void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    buf->base = malloc(suggested_size);
    buf->len = suggested_size;
}

void on_read(uvkcp_t *stream, ssize_t nread, const uv_buf_t *buf) {
    if (nread > 0) {
        // Process received data
        printf("Received %zd bytes\n", nread);
    } else if (nread < 0) {
        // Handle error or EOF
        uvkcp_close(stream, NULL);
    }

    free(buf->base);
}

int main() {
    uv_loop_t *loop = uv_default_loop();
    uvkcp_t server;

    if (uvkcp_init(loop, &server) != 0) {
        fprintf(stderr, "Failed to initialize KCP server\n");
        return 1;
    }

    struct sockaddr_in addr;
    uv_ip4_addr("0.0.0.0", 8080, &addr);

    if (uvkcp_bind(&server, (struct sockaddr*)&addr, 1, 1) != 0) {
        fprintf(stderr, "Failed to bind server\n");
        return 1;
    }

    if (uvkcp_listen(&server, 128, on_connection) != 0) {
        fprintf(stderr, "Failed to listen\n");
        return 1;
    }

    return uv_run(loop, UV_RUN_DEFAULT);
}
```

### Client Example

```c
#include "uvkcp.h"

void on_connect(uvkcp_connect_t *req, int status) {
    if (status != 0) {
        fprintf(stderr, "Connect error: %s\n", uv_strerror(status));
        return;
    }

    uvkcp_t *client = req->handle;

    // Configure KCP for high performance
    uvkcp_set_high_performance(client, 1);

    // Start reading
    uvkcp_read_start(client, alloc_buffer, on_read);

    // Send data
    uv_buf_t buf = uv_buf_init("Hello, Server!", 14);
    uvkcp_write_t write_req;
    uvkcp_write(&write_req, client, &buf, 1, on_write);
}

void on_write(uvkcp_write_t *req, int status) {
    if (status != 0) {
        fprintf(stderr, "Write error: %s\n", uv_strerror(status));
    }
}

void on_read(uvkcp_t *stream, ssize_t nread, const uv_buf_t *buf) {
    if (nread > 0) {
        printf("Received: %.*s\n", (int)nread, buf->base);
    }
    free(buf->base);
}

int main() {
    uv_loop_t *loop = uv_default_loop();
    uvkcp_t client;

    if (uvkcp_init(loop, &client) != 0) {
        fprintf(stderr, "Failed to initialize KCP client\n");
        return 1;
    }

    struct sockaddr_in addr;
    uv_ip4_addr("127.0.0.1", 8080, &addr);

    uvkcp_connect_t connect_req;
    uvkcp_connect(&connect_req, &client, (struct sockaddr*)&addr, on_connect);

    return uv_run(loop, UV_RUN_DEFAULT);
}
```

### Performance Monitoring Example

```c
void perf_callback(uvkcp_t *handle, const uvkcp_netperf_t *perf) {
    printf("Performance Stats:\n");
    printf("  Send Rate: %.2f Mbps\n", perf->mbpsSendRate);
    printf("  Recv Rate: %.2f Mbps\n", perf->mbpsRecvRate);
    printf("  RTT: %d ms\n", perf->msRTT);
    printf("  Packet Loss: %d/%d\n", perf->pktSndLossTotal, perf->pktRcvLossTotal);
    printf("  Retransmissions: %d\n", perf->pktRetransTotal);
}

// Enable performance monitoring
uvkcp_set_perf_callback(handle, perf_callback, 1000); // 1 second interval
```

## Build Integration

uvkcp is designed to be integrated into libuvpp builds. The source files are automatically included in the main libuv library build:

- `uvkcp.c` - Main KCP implementation
- `uvkcp.h` - Public API headers
- `kcpstream.c` - KCP stream interface
- `kcp/` - KCP protocol library

### Build Verification

After building, verify KCP integration:

```bash
nm build/libuv.a | grep -i kcp
```

## Protocol Details

### Handshake Protocol

1. **TCP Connection**: Client establishes TCP connection to server
2. **Handshake Exchange**: Client sends KCP handshake request, server responds with assigned conversation ID
3. **UDP Socket Creation**: Both sides create UDP sockets for data transfer
4. **KCP Session**: KCP protocol session starts using the assigned conversation ID

### Adaptive Timer System

uvkcp uses an adaptive timer system that:

- Uses `ikcp_check()` to determine optimal update intervals
- Dynamically adjusts timer delays based on network conditions
- Provides minimum 1ms and maximum 50ms intervals for responsive performance
- Supports high-performance mode with even tighter bounds (10ms max)

### High-Performance Mode

When enabled, high-performance mode provides:

- Aggressive KCP nodelay settings: `(1, 1, 1, 1)`
- Larger window sizes: 1024 packets
- Faster timer intervals: maximum 10ms delay
- Optimized for low-latency, high-throughput scenarios

## Debugging

Enable debug logging by defining `UVKCP_DEBUG`:

```c
#define UVKCP_DEBUG 1
#include "uvkcp.h"
```

This provides detailed logging for connection establishment, data transfer, and performance metrics.

## License

(The MIT License)

Copyright (c) 2020 Tom Zhou(appnet.link@gmail.com)