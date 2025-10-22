
#include "uvkcp.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#define TEST_PORT (51686)

// Helper function to display KCP connection information
static void display_kcp_connection_info(uvkcp_t* handle, const char* role) {
    // Get KCP conversation ID
    kcp_context_t *ctx = (kcp_context_t *)handle->kcp_ctx;
    if (!ctx || !ctx->kcp) {
        printf("[%s] KCP context not available\n", role);
        return;
    }

    uint32_t conv_id = ctx->kcp->conv;
    printf("[%s] KCP Conversation ID: %u\n", role, conv_id);

    // Get peer address (remote endpoint)
    struct sockaddr_storage peer_addr;
    int addr_len = sizeof(peer_addr);
    if (uvkcp_getpeername(handle, (struct sockaddr*)&peer_addr, &addr_len) == 0) {
        char ip_str[INET6_ADDRSTRLEN];
        uint16_t port;

        if (peer_addr.ss_family == AF_INET) {
            struct sockaddr_in *addr_in = (struct sockaddr_in*)&peer_addr;
            inet_ntop(AF_INET, &addr_in->sin_addr, ip_str, sizeof(ip_str));
            port = ntohs(addr_in->sin_port);
        } else {
            struct sockaddr_in6 *addr_in6 = (struct sockaddr_in6*)&peer_addr;
            inet_ntop(AF_INET6, &addr_in6->sin6_addr, ip_str, sizeof(ip_str));
            port = ntohs(addr_in6->sin6_port);
        }
        printf("[%s] Connected to: %s:%d\n", role, ip_str, port);
    } else {
        printf("[%s] Peer address not available\n", role);
    }

    // Get local address
    struct sockaddr_storage local_addr;
    addr_len = sizeof(local_addr);
    if (uvkcp_getsockname(handle, (struct sockaddr*)&local_addr, &addr_len) == 0) {
        char ip_str[INET6_ADDRSTRLEN];
        uint16_t port;

        if (local_addr.ss_family == AF_INET) {
            struct sockaddr_in *addr_in = (struct sockaddr_in*)&local_addr;
            inet_ntop(AF_INET, &addr_in->sin_addr, ip_str, sizeof(ip_str));
            port = ntohs(addr_in->sin_port);
        } else {
            struct sockaddr_in6 *addr_in6 = (struct sockaddr_in6*)&local_addr;
            inet_ntop(AF_INET6, &addr_in6->sin6_addr, ip_str, sizeof(ip_str));
            port = ntohs(addr_in6->sin6_port);
        }
        printf("[%s] Listening on: %s:%d\n", role, ip_str, port);
    } else {
        printf("[%s] Local address not available\n", role);
    }
}

typedef struct {
  uvkcp_write_t req;
  uv_buf_t buf;
} write_req_t;

static uv_loop_t* loop;

static uvkcp_t kcpServer;
static uvkcp_t *server;
static uvkcp_t kcp6Server;
static uvkcp_t *server6;

static void after_write(uvkcp_write_t* req, int status);
static void after_read(uvkcp_t*, ssize_t nread, const uv_buf_t* buf);
static void on_close(uv_handle_t* peer);
static void on_connection(uvkcp_t*, int status);


static void after_write(uvkcp_write_t* req, int status) {
  write_req_t* wr;

  /* Free the read/write buffer and the request */
  wr = (write_req_t*) req;
  free(wr->buf.base);
  free(wr);

  if (status == 0)
    return;

  fprintf(stderr,
          "uvkcp_write error: %s - %s\n",
          uv_err_name(status),
          uv_strerror(status));
}


static void after_shutdown(uvkcp_shutdown_t* req, int status) {
  uvkcp_close( req->handle, on_close);
  free(req);
}


static void after_read(uvkcp_t* handle,
                       ssize_t nread,
                       const uv_buf_t* buf) {
  write_req_t *wr;
  uvkcp_shutdown_t* sreq;

  if (nread < 0) {
    /* Error or EOF */
    fprintf(stderr, "[SERVER] Read error: %zd (%s)\n", nread, uv_err_name(nread));
    assert(nread == UV_EOF);

    free(buf->base);
    sreq = malloc(sizeof* sreq);
    assert(0 == uvkcp_shutdown(sreq, handle, after_shutdown));
    return;
  }

  if (nread == 0) {
    /* Everything OK, but nothing read. */
    free(buf->base);
    return;
  }

  wr = (write_req_t*) malloc(sizeof *wr);
  assert(wr != NULL);
  wr->buf = uv_buf_init(buf->base, nread);

  int write_result = uvkcp_write(&wr->req, handle, &wr->buf, 1, after_write);
  if (write_result) {
    fprintf(stderr, "[SERVER] uvkcp_write failed with error: %d\n", write_result);
  }
}


static void on_close(uv_handle_t* peer) {
  free(peer);
}


static void echo_alloc(uv_handle_t* handle,
                       size_t suggested_size,
                       uv_buf_t* buf) {
  buf->base = malloc(suggested_size);
  buf->len = suggested_size;
}


static void on_connection(uvkcp_t* client, int status) {
  int r;

  if (status != 0) {
    fprintf(stderr, "[SERVER] Connect error %s\n", uv_err_name(status));
    return;
  }

  // Display connection information for the new client
  printf("\n[SERVER] New client connection established:\n");
  display_kcp_connection_info(client, "SERVER-CLIENT");

  // With TCP handshake, the client stream is already fully connected
  // and ready for reading/writing. No need for uvkcp_accept.
  // The client parameter is the fully connected KCP stream.

  r = uvkcp_read_start(client, echo_alloc, after_read);

  if (r != 0) {
    fprintf(stderr, "[SERVER] uvkcp_read_start failed: %d\n", r);
    return;
  }
}


static int kcp4_echo_start(int port) {
  struct sockaddr_in addr;
  int r;

  assert(0 == uv_ip4_addr("0.0.0.0", port, &addr));

  memset(&kcpServer, 0, sizeof(kcpServer));
  server = &kcpServer;

  r = uvkcp_init(loop, &kcpServer);
  if (r) {
    /* TODO: Error codes */
    fprintf(stderr, "Socket creation error\n");
    return 1;
  }

  r = uvkcp_bind(&kcpServer, (const struct sockaddr*) &addr, 1, 1);
  if (r) {
    /* TODO: Error codes */
    fprintf(stderr, "Bind error\n");
    return 1;
  }

  r = uvkcp_listen((uvkcp_t*)&kcpServer, SOMAXCONN, on_connection);
  if (r) {
    /* TODO: Error codes */
    fprintf(stderr, "Listen error %s\n", uv_err_name(r));
    return 1;
  }

  return 0;
}


static int kcp6_echo_start(int port) {
  struct sockaddr_in6 addr6;
  int r;

  assert(0 == uv_ip6_addr("::1", port, &addr6));

  memset(&kcp6Server, 0, sizeof(kcp6Server));
  server6 = &kcp6Server;

  r = uvkcp_init(loop, &kcp6Server);
  if (r) {
    /* TODO: Error codes */
    fprintf(stderr, "Socket creation error\n");
    return 1;
  }

  /* IPv6 is optional as not all platforms support it */
  r = uvkcp_bind(&kcp6Server, (const struct sockaddr*) &addr6, 1, 1);
  if (r) {
    /* show message but return OK */
    fprintf(stderr, "IPv6 not supported\n");
    return 0;
  }

  r = uvkcp_listen((uvkcp_t*)&kcp6Server, SOMAXCONN, on_connection);
  if (r) {
    /* TODO: Error codes */
    fprintf(stderr, "Listen error\n");
    return 1;
  }

  return 0;
}


int main(int argc, char *argv[]) {
  loop = uv_default_loop();

  int port = TEST_PORT;
  if (argc > 1) {
    port = atoi(argv[1]);
  }

  printf("[SERVER] Starting KCP echo server on port %d\n", port);

  if (kcp4_echo_start(port))
    return 1;

  if (kcp6_echo_start(port))
    return 1;

  // Display server listening information
  printf("\n[SERVER] Server listening information:\n");
  if (server) {
    display_kcp_connection_info(server, "SERVER");
  }
  if (server6) {
    display_kcp_connection_info(server6, "SERVER-IPv6");
  }
  printf("\n[SERVER] Server ready for connections...\n\n");

  uv_run(loop, UV_RUN_DEFAULT);
  return 0;
}
