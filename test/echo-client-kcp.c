#include "uvkcp.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h> /* strlen */
#include <assert.h>

#define TEST_PORT (51686)

#define SERVER_MAX_NUM 1
#define CLIENT_MAX_NUM 1

/* Run the benchmark for this many ms */
#define TIME 6000

typedef struct {
  int domain;
  int pongs;
  int state;
  uvkcp_t kcp;
  uvkcp_connect_t connect_req;
  uvkcp_shutdown_t shutdown_req;
} pinger_t;

typedef struct buf_s {
  uv_buf_t uv_buf_t;
  struct buf_s* next;
} buf_t;


static char PING[] = "PING\n";

static uv_loop_t* loop;

static buf_t* buf_freelist = NULL;
static int64_t start_time;


static void buf_alloc(uv_handle_t* udt, size_t size, uv_buf_t *buf) {
  buf_t* ab;

  ab = buf_freelist;

  if (ab != NULL) {
    buf_freelist = ab->next;
    *buf = ab->uv_buf_t;
    return;
  }

  ab = (buf_t*) malloc(size + sizeof *ab);
  ab->uv_buf_t.len = size;
  ab->uv_buf_t.base = ((char*) ab) + sizeof *ab;

  *buf = ab->uv_buf_t;
}


static void buf_free(const uv_buf_t * buf) {
  buf_t* ab = (buf_t*) (buf->base - sizeof *ab);

  ab->next = buf_freelist;
  buf_freelist = ab;
}


static void pinger_close_cb(uv_handle_t* handle) {
  pinger_t* pinger;

  pinger = (pinger_t*)handle->data;
  printf("ping_pongs: %d roundtrips/s in ip%d\n", (1000 * pinger->pongs) / TIME, pinger->domain);

  free(pinger);
}


static void pinger_write_cb(uvkcp_write_t* req, int status) {
  assert(status == 0);

  free(req);
}


static void pinger_write_ping(pinger_t* pinger) {
  uvkcp_write_t* req;
  uv_buf_t buf;

  buf.base = PING;
  buf.len = strlen(PING);

  req = malloc(sizeof *req);
  if (uvkcp_write(req, &pinger->kcp, &buf, 1, pinger_write_cb)) {
    printf("uvkcp_write failed");
  }
}


static void pinger_shutdown_cb(uvkcp_shutdown_t* req, int status) {
  uvkcp_close(req->handle, pinger_close_cb);

  assert(status == 0);
}


static void pinger_read_cb(uvkcp_t* kcp, ssize_t nread, const uv_buf_t * buf) {
  ssize_t i;
  pinger_t* pinger;

  pinger = (pinger_t*)((uv_handle_t*)kcp)->data;

  printf("[CLIENT] Read callback: nread=%zd, buf_base=%p, buf_len=%zu\n", nread, buf->base, buf->len);

  if (nread < 0) {
    if (buf->base) {
      buf_free(buf);
    }

    printf("[CLIENT] Read error: %zd (%s)\n", nread, uv_err_name(nread));
    assert(nread == UV_EOF);

    return;
  }

  // Log the actual data received
  printf("[CLIENT] Received %zd bytes of data: ", nread);
  for (i = 0; i < nread && i < 32; i++) {
    printf("%02x ", (unsigned char)buf->base[i]);
  }
  if (nread > 32) printf("...");
  printf("\n");

  /* Now we count the pings */
  for (i = 0; i < nread; i++) {
    printf("[CLIENT] Checking byte %zd: expected='%c'(0x%02x), actual='%c'(0x%02x)\n",
           i, PING[pinger->state], (unsigned char)PING[pinger->state],
           buf->base[i], (unsigned char)buf->base[i]);
    assert(buf->base[i] == PING[pinger->state]);
    pinger->state = (pinger->state + 1) % (sizeof(PING) - 1);
    if (pinger->state == 0) {
      pinger->pongs++;
      printf("[CLIENT] Received PONG #%d\n", pinger->pongs);
      if (uv_now(loop) - start_time > TIME) {
        printf("[CLIENT] Time expired, shutting down\n");
        uvkcp_shutdown(&pinger->shutdown_req, kcp, pinger_shutdown_cb);
        break;
      } else {
        printf("[CLIENT] Writing next PING\n");
        pinger_write_ping(pinger);
      }
    }
  }

  buf_free(buf);
}


static void pinger_connect_cb(uvkcp_connect_t* req, int status) {
  pinger_t *pinger = (pinger_t*)((uv_handle_t*)req->handle)->data;

  printf("[CLIENT] Connect callback: status=%d (%s)\n", status, uv_err_name(status));

  if (status != 0) {
    printf("[CLIENT] Connection failed, not proceeding with PING\n");
    return;
  }

  printf("[CLIENT] Connection successful, writing initial PING\n");
  pinger_write_ping(pinger);

  int read_start_result = uvkcp_read_start(req->handle, buf_alloc, pinger_read_cb);
  if (read_start_result) {
    printf("[CLIENT] uvkcp_read_start failed with error: %d\n", read_start_result);
  } else {
    printf("[CLIENT] Started reading successfully\n");
  }
}


static void pinger_new(int port) {
  int r;
  struct sockaddr_in client_addr; 
  uv_ip4_addr("0.0.0.0", 0, &client_addr);

  struct sockaddr_in server_addr; 
  uv_ip4_addr("127.0.0.1", port, &server_addr);
  pinger_t *pinger;

  pinger = (pinger_t*)malloc(sizeof(*pinger));
  pinger->domain= 4;
  pinger->state = 0;
  pinger->pongs = 0;

  /* Try to connect to the server and do NUM_PINGS ping-pongs. */
  r = uvkcp_init(loop, &pinger->kcp);
  assert(!r);

  // Set application data on the handle
  ((uv_handle_t*)&pinger->kcp)->data = pinger;

  // Client doesn't need to bind - just connect
  r = uvkcp_connect(&pinger->connect_req, &pinger->kcp, (const struct sockaddr*)&server_addr, pinger_connect_cb);
  assert(!r);
}

static void pinger_new6(int port)
{
    int r;
    struct sockaddr_in6 client_addr;
    uv_ip6_addr("::", 0, &client_addr);

    struct sockaddr_in6 server_addr;
    uv_ip6_addr("::1", port, &server_addr);
    pinger_t *pinger;

    pinger = (pinger_t *)malloc(sizeof(*pinger));
    pinger->domain= 6;
    pinger->state = 0;
    pinger->pongs = 0;

    /* Try to connect to the server and do NUM_PINGS ping-pongs. */
    r = uvkcp_init(loop, &pinger->kcp);
    assert(!r);

    // Set application data on the handle
    ((uv_handle_t*)&pinger->kcp)->data = pinger;

    // Client doesn't need to bind - just connect
    r = uvkcp_connect(&pinger->connect_req, &pinger->kcp, (const struct sockaddr*)&server_addr, pinger_connect_cb);
    assert(!r);
}

int main(int argc, char * argv [])
{
    int srvNum=1;
	int clnNum=1;
	int port = TEST_PORT;
	int i=0;
	int j=0;
	loop = uv_default_loop();

	start_time = uv_now(loop);

	if (argc == 4) {
	    port    = atoi(argv[1]);
	    srvNum  = atoi(argv[2]);
		clnNum  = atoi(argv[3]);
	} else if (argc == 3) {
	    port   = atoi(argv[1]);
	    srvNum = atoi(argv[2]);
	} else if (argc == 2) {
	    port = atoi(argv[1]);
	}
	srvNum = (srvNum < SERVER_MAX_NUM)? srvNum : SERVER_MAX_NUM;
	clnNum = (clnNum < CLIENT_MAX_NUM)? clnNum : CLIENT_MAX_NUM;
	
	for (i = 0; i < srvNum; i++)
		for (j = 0; j < clnNum; j++) {
            pinger_new(port+i);
            pinger_new6(port+i);
        }

    uv_run(loop, UV_RUN_DEFAULT);

    return 0;
}
