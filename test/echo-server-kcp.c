
#include "uvkcp.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#define TEST_PORT (51686)

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

  printf("[SERVER] Read callback: nread=%zd, buf_base=%p, buf_len=%zu\n", nread, buf->base, buf->len);

  if (nread < 0) {
    /* Error or EOF */
    printf("[SERVER] Read error: %zd (%s)\n", nread, uv_err_name(nread));
    assert(nread == UV_EOF);

    free(buf->base);
    sreq = malloc(sizeof* sreq);
    assert(0 == uvkcp_shutdown(sreq, handle, after_shutdown));
    return;
  }

  if (nread == 0) {
    /* Everything OK, but nothing read. */
    printf("[SERVER] Read 0 bytes\n");
    free(buf->base);
    return;
  }

  // Log the actual data received
  ssize_t j;
  printf("[SERVER] Received %zd bytes of data: ", nread);
  for (j = 0; j < nread && j < 32; j++) {
    printf("%02x ", (unsigned char)buf->base[j]);
  }
  if (nread > 32) printf("...");
  printf("\n");

  wr = (write_req_t*) malloc(sizeof *wr);
  assert(wr != NULL);
  wr->buf = uv_buf_init(buf->base, nread);

  printf("[SERVER] Echoing back %zd bytes\n", nread);
  int write_result = uvkcp_write(&wr->req, handle, &wr->buf, 1, after_write);
  if (write_result) {
    printf("[SERVER] uvkcp_write failed with error: %d\n", write_result);
  } else {
    printf("[SERVER] uvkcp_write succeeded\n");
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

  printf("[SERVER] Connection callback: status=%d\n", status);

  if (status != 0) {
    fprintf(stderr, "[SERVER] Connect error %s\n", uv_err_name(status));
    return;
  } else {
    printf("[SERVER] Connect success\n");
  }

  // With TCP handshake, the client stream is already fully connected
  // and ready for reading/writing. No need for uvkcp_accept.
  // The client parameter is the fully connected KCP stream.

  r = uvkcp_read_start(client, echo_alloc, after_read);

  printf("[SERVER] Started reading on accepted stream, result=%d\n", r);
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

  printf("[SERVER] Server started successfully, running event loop...\n");
  uv_run(loop, UV_RUN_DEFAULT);
  printf("[SERVER] Event loop finished\n");
  return 0;
}
