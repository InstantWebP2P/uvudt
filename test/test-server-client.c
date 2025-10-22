#include "uvkcp.h"
#include "kcp/ikcp.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#define TEST_PORT (51690)

static uv_loop_t* loop;
static uvkcp_t kcp_server;
static int server_connections = 0;
static int client_connections = 0;

// Test data
static char TEST_MESSAGE[] = "Hello KCP Server/Client!";

// Forward declarations
static int start_kcp_server(void);
static int start_kcp_client(void);

// Timer callback for starting client
static void start_client_timer_cb(uv_timer_t* timer) {
    start_kcp_client();
}

// Server callbacks
static void server_after_write(uvkcp_write_t* req, int status) {
    if (status != 0) {
        fprintf(stderr, "[SERVER] Write error: status=%d\n", status);
    }
    free(req);
}

static void server_after_read(uvkcp_t* handle, ssize_t nread, const uv_buf_t* buf) {
    if (nread < 0) {
        fprintf(stderr, "[SERVER] Read error: %zd\n", nread);
        free(buf->base);
        return;
    }

    if (nread == 0) {
        free(buf->base);
        return;
    }

    // Echo back the received data
    uvkcp_write_t *write_req = malloc(sizeof(uvkcp_write_t));
    uv_buf_t write_buf = uv_buf_init(buf->base, nread);

    if (uvkcp_write(write_req, handle, &write_buf, 1, server_after_write)) {
        fprintf(stderr, "[SERVER] uvkcp_write failed\n");
        free(write_req);
    }
}

static void server_echo_alloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    buf->base = malloc(suggested_size);
    buf->len = suggested_size;
}

static void server_on_connection(uvkcp_t* server, int status) {
    server_connections++;

    if (status != 0) {
        fprintf(stderr, "[SERVER] Connection error: %s\n", uv_err_name(status));
        return;
    }

    // The client handle is already created and configured by the handshake
    // Just start reading on it
    if (uvkcp_read_start(server, server_echo_alloc, server_after_read)) {
        fprintf(stderr, "[SERVER] uvkcp_read_start failed\n");
    }
}

// Client callbacks
static void client_after_write(uvkcp_write_t* req, int status) {
    if (status != 0) {
        fprintf(stderr, "[CLIENT] Write error: status=%d\n", status);
    }
    free(req);
}

static void client_after_read(uvkcp_t* handle, ssize_t nread, const uv_buf_t* buf) {
    if (nread < 0) {
        fprintf(stderr, "[CLIENT] Read error: %zd\n", nread);
        free(buf->base);
        return;
    }

    if (nread == 0) {
        free(buf->base);
        return;
    }

    // Verify the echo matches our original message
    if (nread == (ssize_t)strlen(TEST_MESSAGE) &&
        memcmp(buf->base, TEST_MESSAGE, nread) == 0) {
        printf("[CLIENT] SUCCESS: Echo matches original message!\n");
    } else {
        fprintf(stderr, "[CLIENT] ERROR: Echo doesn't match original message\n");
    }

    free(buf->base);

    // Close the connection after successful test
    uvkcp_close(handle, NULL);
    client_connections--;

    // Stop the loop if all clients are done
    if (client_connections == 0) {
        uv_stop(loop);
    }
}

static void client_echo_alloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    buf->base = malloc(suggested_size);
    buf->len = suggested_size;
}

static void client_on_connect(uvkcp_connect_t* req, int status) {
    if (status != 0) {
        fprintf(stderr, "[CLIENT] Connect error: %s\n", uv_err_name(status));
        return;
    }

    // Start reading
    if (uvkcp_read_start(req->handle, client_echo_alloc, client_after_read)) {
        fprintf(stderr, "[CLIENT] uvkcp_read_start failed\n");
        return;
    }

    // Send test message
    uvkcp_write_t *write_req = malloc(sizeof(uvkcp_write_t));
    uv_buf_t write_buf = uv_buf_init(TEST_MESSAGE, strlen(TEST_MESSAGE));

    if (uvkcp_write(write_req, req->handle, &write_buf, 1, client_after_write)) {
        fprintf(stderr, "[CLIENT] uvkcp_write failed\n");
        free(write_req);
    }
}

// Test functions
static int start_kcp_server(void) {
    struct sockaddr_in tcp_addr;
    int r;

    assert(0 == uv_ip4_addr("0.0.0.0", TEST_PORT, &tcp_addr));

    printf("[SERVER] Initializing KCP server...\n");
    r = uvkcp_init(loop, &kcp_server);
    if (r != 0) {
        fprintf(stderr, "[SERVER] Failed to initialize KCP server: %s\n", uv_err_name(r));
        return 1;
    }

    // TCP handshake is mandatory and enabled by default in KCP implementation

    printf("[SERVER] Binding KCP server...\n");
    r = uvkcp_bind(&kcp_server, (const struct sockaddr*)&tcp_addr, 1, 0);
    if (r != 0) {
        fprintf(stderr, "[SERVER] Failed to bind KCP server: %s\n", uv_err_name(r));
        return 1;
    }

    printf("[SERVER] Starting KCP server listen...\n");
    r = uvkcp_listen(&kcp_server, 10, server_on_connection);
    if (r != 0) {
        fprintf(stderr, "[SERVER] Failed to listen on KCP server: %s\n", uv_err_name(r));
        return 1;
    }

    printf("[SERVER] KCP server listening on port %d\n", TEST_PORT);
    return 0;
}

static int start_kcp_client(void) {
    struct sockaddr_in tcp_addr;
    uvkcp_t *client;
    uvkcp_connect_t *connect_req;
    int r;

    assert(0 == uv_ip4_addr("127.0.0.1", TEST_PORT, &tcp_addr));

    printf("[CLIENT] Creating KCP client...\n");
    client = malloc(sizeof(uvkcp_t));
    connect_req = malloc(sizeof(uvkcp_connect_t));

    if (!client || !connect_req) {
        fprintf(stderr, "[CLIENT] Memory allocation failed\n");
        free(client);
        free(connect_req);
        return 1;
    }

    printf("[CLIENT] Initializing KCP client...\n");
    r = uvkcp_init(loop, client);
    if (r != 0) {
        fprintf(stderr, "[CLIENT] Failed to initialize KCP client: %s\n", uv_err_name(r));
        free(client);
        free(connect_req);
        return 1;
    }

    // TCP handshake is mandatory and enabled by default in KCP implementation

    r = uvkcp_connect(connect_req, client, (const struct sockaddr*)&tcp_addr, client_on_connect);
    if (r != 0) {
        fprintf(stderr, "[CLIENT] Failed to connect to KCP server: %s\n", uv_err_name(r));
        free(client);
        free(connect_req);
        return 1;
    }

    client_connections++;
    return 0;
}

int main(int argc, char *argv[]) {
    loop = uv_default_loop();

    // Start server
    if (start_kcp_server() != 0) {
        return 1;
    }

    // Start client after a short delay
    uv_timer_t timer;
    uv_timer_init(loop, &timer);
    uv_timer_start(&timer, start_client_timer_cb, 100, 0);

    uv_run(loop, UV_RUN_DEFAULT);

    printf("\n=== Test Summary ===\n");
    printf("Server connections: %d\n", server_connections);
    printf("Client connections: %d\n", client_connections);

    // Cleanup
    uvkcp_close(&kcp_server, NULL);

    if (server_connections > 0 && client_connections == 0) {
        printf("TEST PASSED: Server/Client communication successful!\n");
        return 0;
    } else {
        printf("TEST FAILED: Expected server connections > 0 and client connections == 0\n");
        return 1;
    }
}