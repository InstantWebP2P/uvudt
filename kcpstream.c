//////////////////////////////////////////////////////
// KCP stream interface implementation
// Copyright 2020, tom zhou<appnet.link@gmail.com>
//////////////////////////////////////////////////////

#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include "kcp/ikcp.h"
#include "uvkcp.h"


// KCP output function declaration (implemented in uvkcp.c)
extern int kcp_output(const char *buf, int len, ikcpcb *kcp, void *user);

// Helper to get KCP context from uvkcp_t
static kcp_context_t* kcp__get_context(uvkcp_t* stream) {
    if (!stream || !stream->kcp_ctx) {
        return NULL;
    }
    return (kcp_context_t*)stream->kcp_ctx;
}

static size_t kcp__buf_count(const uv_buf_t bufs[], int nbufs)
{
    size_t total = 0;
    int i;

    for (i = 0; i < nbufs; i++)
    {
        total += bufs[i].len;
    }

    return total;
}

static void kcp__stream_connect(uvkcp_t *stream);
static void kcp__write(uvkcp_t *stream);
static void kcp__read(uvkcp_t *stream);
static void kcp__timer_cb(uv_timer_t* timer);
static void kcp__server_io(uvkcp_t *stream);
void kcp__stream_io(uv_poll_t *handle, int status, int events);

void kcp__stream_init(uv_loop_t* loop, uvkcp_t* stream) {
  UVKCP_LOG_FUNC("Initializing KCP stream");

  // hold loop
  stream->aloop = loop;
  stream->flags = 0;

  stream->alloc_cb = NULL;
  stream->read_cb = NULL;
  stream->connection_cb = NULL;
  stream->connect_req = NULL;
  stream->shutdown_req = NULL;
  stream->accepted_kcpfd = -1;
  stream->kcpfd = -1;
  stream->udpfd = -1;
  stream->fd = -1;
  stream->accepted_kcpfd = -1;
  stream->accepted_udpfd = -1;
  stream->accepted_fd = -1;
  stream->delayed_error = 0;
  stream->kcp_ctx = NULL;
  QUEUE_INIT(&stream->write_queue);
  QUEUE_INIT(&stream->write_completed_queue);
  stream->write_queue_size = 0;

  UVKCP_LOG("KCP stream initialized");
}

int kcp__stream_open(uvkcp_t* kcp, uv_os_sock_t fd, int flags) {
    UVKCP_LOG_FUNC("Opening KCP stream with fd=%d", fd);

    uv_poll_t *poll = (uv_poll_t *)kcp;

    kcp->fd = fd;

    // init uv_poll_t
    if (uv_poll_init_socket(kcp->aloop, poll, fd) < 0)
    {
        UVKCP_LOG_ERROR("Failed to initialize poll");
        return -1;
    }

    // start polling for both readable and writable events
    if (uv_poll_start(poll, UV_READABLE | UV_WRITABLE, kcp__stream_io) < 0)
    {
        UVKCP_LOG_ERROR("Failed to start polling");
        return -1;
    }

    kcp->flags |= flags;

    UVKCP_LOG("KCP stream opened successfully");
    return 0;
}


void kcp__stream_destroy(uvkcp_t* stream) {
  uvkcp_write_t* req;
  QUEUE *q;

  if (stream->connect_req) {
      stream->connect_req->cb(stream->connect_req, -1);
      stream->connect_req = NULL;
  }

  while (!QUEUE_EMPTY(&stream->write_queue))
  {
      q = QUEUE_HEAD(&stream->write_queue);
      QUEUE_REMOVE(q);

      req = QUEUE_DATA(q, uvkcp_write_t, queue);
      req->error = UV_ECANCELED;

      if (req->bufs != req->bufsml)
          free(req->bufs);

      if (req->cb)
      {
          req->cb(req, UV_ECANCELED);
      }
  }

  while (!QUEUE_EMPTY(&stream->write_completed_queue))
  {
      q = QUEUE_HEAD(&stream->write_completed_queue);
      QUEUE_REMOVE(q);

      req = QUEUE_DATA(q, uvkcp_write_t, queue);

      if (req->cb)
      {
          req->cb(req, req->error ? UV_ECANCELED : 0);
      }
  }

  if (stream->shutdown_req) {
      if (stream->shutdown_req->cb)
          stream->shutdown_req->cb(stream->shutdown_req, UV_ECANCELED);
      stream->shutdown_req = NULL;
  }
}

uvkcp_write_t* uvkcp_write_queue_head(uvkcp_t* stream) {
  QUEUE* q;
  uvkcp_write_t* req;

  if (QUEUE_EMPTY(&stream->write_queue)) {
    return NULL;
  }

  q = QUEUE_HEAD(&stream->write_queue);
  if (!q) {
    return NULL;
  }

  req = QUEUE_DATA(q, uvkcp_write_t, queue);
  assert(req);

  return req;
}


static void kcp__drain(uvkcp_t* stream) {
  uvkcp_shutdown_t* req = NULL;

  assert(!uvkcp_write_queue_head(stream));
  assert(stream->write_queue_size == 0);

  /* Shutdown? */
  if (( stream->flags & UVKCP_FLAG_SHUTTING) &&
      !(stream->flags & UVKCP_FLAG_CLOSING) &&
      !(stream->flags & UVKCP_FLAG_SHUT)) {
    assert(stream->shutdown_req);

    req = stream->shutdown_req;
    stream->shutdown_req = NULL;

    // KCP doesn't need explicit close like UDT
    stream->flags &= ~UVKCP_FLAG_SHUTTING;
    stream->flags |=  UVKCP_FLAG_SHUT;
    if (req->cb) {
        req->cb(req, 0);
    }
  }
}


static size_t kcp__write_req_size(uvkcp_write_t* req) {
  size_t size;

  size = kcp__buf_count(req->bufs + req->write_index,
                        req->nbufs - req->write_index);
  assert(req->handle->write_queue_size >= size);

  return size;
}


static void kcp__write_req_finish(uvkcp_write_t* req) {
  uvkcp_t* stream = req->handle;

  UVKCP_LOG_FUNC("kcp__write_req_finish: Completing write request");

  /* Pop the req off tcp->write_queue. */
  QUEUE_REMOVE(&req->queue);
  if (req->bufs != req->bufsml) {
    free(req->bufs);
  }
  req->bufs = NULL;

  /* Add it to the write_completed_queue where it will have its
   * callback called in the near future.
   */
  QUEUE_INSERT_TAIL(&stream->write_completed_queue, &req->queue);

  UVKCP_LOG("kcp__write_req_finish: Write request moved to completed queue");

  // KCP always polling on read event
  /// kcp__io_feed(kcp->loop, &stream->write_watcher, UV__IO_READ);

}


/* On success returns NULL. On error returns a pointer to the write request
 * which had the error.
 */
static void kcp__write(uvkcp_t* stream) {
  uvkcp_write_t* req;
  uv_buf_t *iov;
  int iovcnt;
  ssize_t n;

  UVKCP_LOG_FUNC("kcp__write: Processing write request");

  if ((stream->flags & UVKCP_FLAG_CLOSING) ||
      (stream->flags & UVKCP_FLAG_CLOSED)) {
    /* Handle was closed this tick. We've received a stale
     * 'is writable' callback from the event loop, ignore.
     */
    UVKCP_LOG("kcp__write: Stream is closing/closed, ignoring write");
    return;
  }

  kcp_context_t* ctx = kcp__get_context(stream);
  if (!ctx || !ctx->kcp) {
    UVKCP_LOG_ERROR("kcp__write: Invalid KCP context or KCP instance");
    return;
  }

  /* Get the request at the head of the queue. */
  req = uvkcp_write_queue_head(stream);
  if (!req) {
    assert(stream->write_queue_size == 0);
    UVKCP_LOG("kcp__write: No write requests in queue");
    return;
  }

  assert(req->handle == stream);

  iov = &(req->bufs[req->write_index]);
  iovcnt = req->nbufs - req->write_index;

  UVKCP_LOG("kcp__write: Processing request with iovcnt=%d, write_index=%u, total_bufs=%u",
            iovcnt, req->write_index, req->nbufs);

  {
    int next = 1, it = 0;
    n = -1;
    for (it = 0; it < iovcnt; it ++) {
      size_t ilen = 0;
      UVKCP_LOG("kcp__write: Processing buffer %d, length=%zu", it, iov[it].len);
      while (ilen < iov[it].len) {
        // Use KCP send function - KCP handles fragmentation internally
        int rc = ikcp_send(ctx->kcp, ((char *)iov[it].base)+ilen, iov[it].len-ilen);
        UVKCP_LOG("kcp__write: ikcp_send returned %d for buffer %d (offset=%zu, remaining=%zu)",
                  rc, it, ilen, iov[it].len-ilen);
        if (rc < 0) {
          UVKCP_LOG_ERROR("kcp__write: KCP send failed with error %d", rc);
          next = 0;
          break;
        } else  {
          if (n == -1) n = 0;
          n += (iov[it].len - ilen);
          ilen = iov[it].len; // KCP sends entire buffer
          UVKCP_LOG("kcp__write: Successfully sent %zu bytes from buffer %d",
                    iov[it].len - ilen, it);
        }
      }
      if (next == 0) break;
    }
  }

  if (n < 0) {
      /* Error */
      UVKCP_LOG_ERROR("kcp__write: Write failed, total_bytes=%zd", n);
      req->error = UV_EIO;
      stream->write_queue_size -= kcp__write_req_size(req);
      kcp__write_req_finish(req);
      return;
  } else {
    /* Successful write */
    UVKCP_LOG("kcp__write: Successfully wrote %zd bytes", n);

    /* Update the counters. */
    while (n >= 0) {
      uv_buf_t* buf = &(req->bufs[req->write_index]);
      size_t len = buf->len;

      assert(req->write_index < req->nbufs);

      if ((size_t)n < len) {
        UVKCP_LOG("kcp__write: Partial write - wrote %zu of %zu bytes from buffer %u",
                  (size_t)n, len, req->write_index);
        buf->base += n;
        buf->len -= n;
        stream->write_queue_size -= n;
        n = 0;

        /* Break loop and ensure the watcher is pending. */
        break;
      } else {
        /* Finished writing the buf at index req->write_index. */
        UVKCP_LOG("kcp__write: Completed buffer %u (%zu bytes)", req->write_index, len);
        req->write_index++;

        assert((size_t)n >= len);
        n -= len;

        assert(stream->write_queue_size >= len);
        stream->write_queue_size -= len;

        if (req->write_index == req->nbufs) {
          /* Then we're done! */
          assert(n == 0);
          UVKCP_LOG("kcp__write: Completed all buffers in request");
          kcp__write_req_finish(req);
          /* TODO: start trying to write the next request. */
          return;
        }
      }
    }
  }

  /* Either we've counted n down to zero or we've got EAGAIN. */
  assert(n == 0 || n == -1);

  UVKCP_LOG("kcp__write: Write operation completed, remaining_bytes=%zd", n);
  /* We're not done. */

}

static void kcp__write_callbacks(uvkcp_t* stream) {
  uvkcp_write_t* req;
  QUEUE* q;


  while (!QUEUE_EMPTY(&stream->write_completed_queue)) {
    /* Pop a req off write_completed_queue. */
    q = QUEUE_HEAD(&stream->write_completed_queue);
    req = QUEUE_DATA(q, uvkcp_write_t, queue);
    QUEUE_REMOVE(q);

    /* NOTE: call callback AFTER freeing the request data. */
    if (req->cb) {
      req->cb(req, req->error ? -1 : 0);
    }
  }

  assert(QUEUE_EMPTY(&stream->write_completed_queue));

  /* Write queue drained. */
  if (!uvkcp_write_queue_head(stream)) {
    kcp__drain(stream);
  }
}


static void kcp__read_udp_data(uvkcp_t* stream) {
  kcp_context_t* ctx = kcp__get_context(stream);
  if (!ctx || !ctx->kcp || ctx->udp_fd == -1) {
    UVKCP_LOG("kcp__read_udp_data: Invalid context or UDP socket");
    return;
  }

  char buffer[65536];
  struct sockaddr_storage peer_addr;
  socklen_t addr_len = sizeof(peer_addr);

  int count = 32; // Prevent loop starvation
  int packets_read = 0;
  int total_bytes = 0;

  UVKCP_LOG_FUNC("kcp__read_udp_data: Starting UDP data read loop");

  while (count-- > 0) {
    ssize_t nread = recvfrom(ctx->udp_fd, buffer, sizeof(buffer), 0,
                            (struct sockaddr*)&peer_addr, &addr_len);

    if (nread <= 0) {
      if (nread < 0) {
        UVKCP_LOG("kcp__read_udp_data: recvfrom error: %s (errno=%d)", strerror(errno), errno);
      } else {
        UVKCP_LOG("kcp__read_udp_data: No more data available");
      }
      break; // No more data or error
    }

    packets_read++;
    total_bytes += nread;

    UVKCP_LOG("kcp__read_udp_data: Received UDP packet %d, size=%zd bytes", packets_read, nread);

    // Track statistics
    ctx->pktRecvTotal++;
    ctx->bytesRecvTotal += nread;

    // Feed the raw UDP data to KCP
    int rc = ikcp_input(ctx->kcp, buffer, nread);
    if (rc < 0) {
      UVKCP_LOG_ERROR("kcp__read_udp_data: KCP input failed with error %d", rc);
      // KCP input error, but continue processing
      break;
    } else {
      UVKCP_LOG("kcp__read_udp_data: Successfully fed %zd bytes to KCP", nread);
    }
  }

  if (packets_read > 0) {
    UVKCP_LOG("kcp__read_udp_data: Completed - read %d packets, total %d bytes", packets_read, total_bytes);
  }
}

static void kcp__read(uvkcp_t* stream) {
  uv_buf_t buf;
  ssize_t nread;
  int count;

  UVKCP_LOG_FUNC("kcp__read: Starting KCP read operation");

  // First, read any available UDP data and feed it to KCP
  kcp__read_udp_data(stream);

  /* Prevent loop starvation when the data comes in as fast as (or faster than)
   * we can read it. XXX Need to rearm fd if we switch to edge-triggered I/O.
   */
  count = 32;
  int read_attempts = 0;

  while ((stream->read_cb)
      && (stream->flags & UVKCP_FLAG_READABLE)
      && (count-- > 0)) {
    read_attempts++;
    assert(stream->alloc_cb);

    buf = uv_buf_init(NULL, 0);
    stream->alloc_cb((uv_handle_t *)stream, 64 * 1024, &buf);
    if (buf.base == NULL || buf.len == 0)
    {
        UVKCP_LOG("kcp__read: User buffer allocation failed - base=%p, len=%zu", buf.base, buf.len);
        /* User indicates it can't or won't handle the read. */
        stream->read_cb(stream, UV_ENOBUFS, &buf);
        return;
    }
    assert(buf.base != NULL);

    UVKCP_LOG("kcp__read: Attempt %d - allocated buffer: base=%p, len=%zu", read_attempts, buf.base, buf.len);

    kcp_context_t* ctx = kcp__get_context(stream);
    if (!ctx || !ctx->kcp) {
        UVKCP_LOG_ERROR("kcp__read: Invalid KCP context or KCP instance");
        stream->read_cb(stream, UV_ENOTCONN, &buf);
        return;
    }

    // KCP recv
    {
        nread = ikcp_recv(ctx->kcp, buf.base, buf.len);
        UVKCP_LOG("kcp__read: ikcp_recv returned %zd (buffer_len=%zu)", nread, buf.len);

        if (nread < 0) {
        /* Error */
        if (nread == -1) {
          UVKCP_LOG("kcp__read: No data available from KCP");
          /* No data available, wait for next data */
          stream->read_cb(stream, 0, &buf);
          return;
        } else if (nread == -2) {
          UVKCP_LOG_ERROR("kcp__read: Buffer too small for KCP data (buffer_len=%zu)", buf.len);
          /* Buffer too small, need larger buffer */
          stream->read_cb(stream, UV_ENOBUFS, &buf);
          return;
        } else if (nread == -3) {
          UVKCP_LOG_ERROR("kcp__read: Invalid KCP state");
          /* Invalid KCP state */
          stream->read_cb(stream, UV_EIO, &buf);
          return;
        } else {
          UVKCP_LOG_ERROR("kcp__read: Other KCP error: %zd", nread);
          /* Other error */
          stream->read_cb(stream, UV_EIO, &buf);
          return;
        }
      } else if (nread == 0) {
        UVKCP_LOG("kcp__read: No data available (nread=0)");
        // No data available
        stream->read_cb(stream, 0, &buf);
        return;
      } else {
        /* Successful read */
        ssize_t buflen = buf.len;
        UVKCP_LOG("kcp__read: Successfully read %zd bytes from KCP", nread);
        stream->read_cb(stream, nread, &buf);

        /* Return if we didn't fill the buffer, there is no more data to read. */
        if (nread < buflen) {
          UVKCP_LOG("kcp__read: Partial read (%zd < %zu), stopping read loop", nread, buflen);
          return;
        }
      }
    }
  }

  if (read_attempts > 0) {
    UVKCP_LOG("kcp__read: Completed - %d read attempts made", read_attempts);
  }
}


int uvkcp_shutdown(uvkcp_shutdown_t* req, uvkcp_t* stream, uvkcp_shutdown_cb cb) {
  uv_poll_t *poll = (uv_poll_t *)stream;

  assert((poll->type == UV_POLL) &&
         "uvkcp_shutdown (unix) only supports uv_handle_t right now");

  kcp_context_t* ctx = kcp__get_context(stream);
  if (!ctx || !ctx->kcp) {
    return -1;
  }

  if (!(stream->flags & UVKCP_FLAG_WRITABLE) ||
        stream->flags & UVKCP_FLAG_SHUT ||
        stream->flags & UVKCP_FLAG_CLOSED ||
        stream->flags & UVKCP_FLAG_CLOSING) {
    return -1;
  }

  /* Initialize request */
  req->type = UVKCP_REQ_SHUTDOWN;
  req->handle = stream;
  req->cb = cb;
  stream->shutdown_req = req;
  stream->flags |= UVKCP_FLAG_SHUTTING;

  return 0;
}

// Timer callback is implemented in uvkcp.c - this is just a forward declaration
extern void kcp__timer_cb(uv_timer_t* timer);

static void kcp__server_io(uvkcp_t* stream) {
    kcp_context_t* ctx = kcp__get_context(stream);
    if (!ctx || !ctx->is_listening) {
        return;
    }

    // For KCP with TCP handshake, the server I/O processing is different:
    // - TCP connections are handled by the TCP server in uvkcp.c
    // - UDP data processing happens in the individual client streams
    // - The server doesn't have its own UDP socket - each client gets its own

    UVKCP_LOG("Server I/O called - no UDP socket to process (TCP handshake only)");
}

void kcp__stream_io(uv_poll_t * handle, int status, int events) {
    UVKCP_LOG_FUNC("Stream I/O: status=%d, events=%d", status, events);

    uvkcp_t *stream = (uvkcp_t *)handle;

    assert(handle->type == UV_POLL);

    kcp_context_t* ctx = kcp__get_context(stream);
    if (!ctx) {
        UVKCP_LOG_ERROR("Invalid context");
        return;
    }

    // Handle server mode - server doesn't have UDP socket, only TCP handshake
    if (ctx->is_listening && !ctx->is_connected) {
        UVKCP_LOG("Processing server I/O (TCP handshake only)");
        kcp__server_io(stream);
        return;
    }

    // Client connections must have UDP socket and KCP instance
    if (!ctx->kcp || ctx->udp_fd == -1) {
        UVKCP_LOG_ERROR("Invalid KCP instance or UDP socket");
        return;
    }

    if (stream->connect_req) {
      kcp_context_t* ctx = kcp__get_context(stream);
      // For KCP with TCP handshake, only process connection request if
      // TCP handshake is not in progress (no pending_connect_req)
      if (ctx && ctx->pending_connect_req) {
        UVKCP_LOG("TCP handshake in progress, deferring connection processing");
      } else {
        UVKCP_LOG("Processing connection request");
        kcp__stream_connect(stream);
      }
    } else {
      // Start timer if not already active
      if (!ctx->timer_active) {
        IUINT32 current = uv_now(ctx->loop);
        ikcp_update(ctx->kcp, current);
        IUINT32 next_update = ikcp_check(ctx->kcp, current);
        if (next_update == 0) {
          // Update immediately and check again
          ikcp_update(ctx->kcp, current);
          next_update = ikcp_check(ctx->kcp, current);
        }
        if (next_update > 0) {
          uv_timer_start(&ctx->timer_handle, kcp__timer_cb, next_update, 0);
          ctx->timer_active = 1;
          UVKCP_LOG("Started KCP timer, next update in %u ms", next_update);
        }
      }

      // Process readable events - always try to read UDP data first
      if (events & UV_READABLE) {
        kcp__read(stream);
      }

      // Process writable events - try to write queued data
      if (events & UV_WRITABLE) {
        kcp__write(stream);
        kcp__write_callbacks(stream);
      }

      // Update KCP to flush any pending output, but only if we have data to send
      // This is more efficient than always calling ikcp_update()
      IUINT32 current = uv_now(ctx->loop);
      IUINT32 next_update = ikcp_check(ctx->kcp, current);
      if (next_update == 0) {
        // KCP has work to do immediately
        ikcp_update(ctx->kcp, current);
      }
    }
}


/**
 * We get called here from directly following a call to connect(2).
 * In order to determine if we've errored out or succeeded must call
 * getsockopt.
 */
static void kcp__stream_connect(uvkcp_t* stream) {
  int error;
  uvkcp_connect_t* req = stream->connect_req;

  UVKCP_LOG_FUNC("kcp__stream_connect: Processing connection request");

  assert(req);

  if (stream->delayed_error) {
    /* To smooth over the differences between unixes errors that
     * were reported synchronously on the first connect can be delayed
     * until the next tick--which is now.
     */
    error = stream->delayed_error;
    stream->delayed_error = 0;
    UVKCP_LOG("kcp__stream_connect: Using delayed error: %d", error);
  } else {
        kcp_context_t* ctx = kcp__get_context(stream);
        if (!ctx) {
            error = UV_ENOTCONN;
            UVKCP_LOG_ERROR("kcp__stream_connect: Invalid KCP context");
        } else if (!ctx->kcp) {
            error = UV_ENOTCONN;
            UVKCP_LOG("kcp__stream_connect: KCP instance not created yet (TCP handshake in progress)");
        } else if (ctx->is_connected) {
            error = 0;
            UVKCP_LOG("kcp__stream_connect: Connection already established");
        } else {
            error = UV_EALREADY;
            UVKCP_LOG("kcp__stream_connect: Connection still in progress (TCP handshake)");
        }
  }

  if (error == UV_EALREADY) {
    UVKCP_LOG("kcp__stream_connect: Connection still in progress, returning");
    return;
  }

  stream->connect_req = NULL;
  UVKCP_LOG("kcp__stream_connect: Cleared connect_req, calling callback with status=%d", error);

  if (req->cb) {
    req->cb(req, error);
  }
}


/* The buffers to be written must remain valid until the callback is called.
 * This is not required for the uv_buf_t array.
 */
int uvkcp_write(uvkcp_write_t *req, uvkcp_t *stream, const uv_buf_t bufs[], unsigned int nbufs, uvkcp_write_cb cb)
{
    int empty_queue;

    UVKCP_LOG_FUNC("uvkcp_write: nbufs=%u", nbufs);

    kcp_context_t* ctx = kcp__get_context(stream);
    if (!ctx || !ctx->kcp) {
        UVKCP_LOG_ERROR("uvkcp_write: Invalid KCP context or KCP instance");
        return -1;
    }

    // check flags
    int flags = stream->flags;
    if (!(flags & UVKCP_FLAG_WRITABLE) ||
         (flags & UVKCP_FLAG_SHUTTING) || (flags & UVKCP_FLAG_SHUT) ||
         (flags & UVKCP_FLAG_CLOSING)  || (flags & UVKCP_FLAG_CLOSED))
    {
        UVKCP_LOG_ERROR("uvkcp_write: Invalid stream state - flags=0x%x", flags);
        return -1;
    }

    empty_queue = (stream->write_queue_size == 0);
    UVKCP_LOG("uvkcp_write: empty_queue=%d, current_queue_size=%zu", empty_queue, stream->write_queue_size);

    /* Initialize the req */
    req->type = UVKCP_REQ_WRITE;
    req->cb = cb;
    req->handle = stream;
    req->error = 0;
    QUEUE_INIT(&req->queue);

    req->bufs = req->bufsml;
    if (nbufs > (sizeof(req->bufsml)/sizeof(req->bufsml[0])))
        req->bufs = malloc(nbufs * sizeof(bufs[0]));

    if (req->bufs == NULL) {
        UVKCP_LOG_ERROR("uvkcp_write: Failed to allocate buffer array");
        return UV_ENOMEM;
    }

    memcpy(req->bufs, bufs, nbufs * sizeof(uv_buf_t));
    req->nbufs = nbufs;
    req->write_index = 0;

    size_t total_bytes = kcp__buf_count(bufs, nbufs);
    stream->write_queue_size += total_bytes;

    UVKCP_LOG("uvkcp_write: Adding %zu bytes to write queue, new queue_size=%zu",
              total_bytes, stream->write_queue_size);

    /* Append the request to write_queue. */
    QUEUE_INSERT_TAIL(&stream->write_queue, &req->queue);

    /* If the queue was empty when this function began, we should attempt to
   * do the write immediately. Otherwise start the write_watcher and wait
   * for the fd to become writable.
   */
    if (ctx && !ctx->is_connected)
    {
        /* Still connecting or TCP handshake in progress, do nothing. */
        UVKCP_LOG("uvkcp_write: Still connecting (connect_req=%p, is_connected=%d), deferring write",
                  stream->connect_req, ctx ? ctx->is_connected : -1);
    }
    else if (empty_queue)
    {
        UVKCP_LOG("uvkcp_write: Queue was empty, attempting immediate write");
        kcp__write(stream);
    } else {
        UVKCP_LOG("uvkcp_write: Queue not empty, returning UV_ENOBUFS");
        return UV_ENOBUFS;
    }

    UVKCP_LOG("uvkcp_write: Successfully queued write request");
    return 0;
}

static void uvkcp_try_write_cb(uvkcp_write_t* req, int status) {
  /* Should not be called */
  abort();
}

int uvkcp_try_write(uvkcp_t* stream,
                    const uv_buf_t bufs[],
                    unsigned int nbufs) {
  int r;
  size_t written;
  size_t req_size;
  uvkcp_write_t req;

  /* Connecting or already writing some data */
  kcp_context_t* ctx = kcp__get_context(stream);
  if ((ctx && !ctx->is_connected) || stream->write_queue_size != 0)
    return UV_EAGAIN;

  r = uvkcp_write(&req, stream, bufs, nbufs, uvkcp_try_write_cb);
  if (r != 0) return r;

  /* Remove not written bytes from write queue size */
  written = kcp__buf_count(bufs, nbufs);
  if (req.bufs != NULL)
    req_size = kcp__write_req_size(&req);
  else
    req_size = 0;
  written -= req_size;
  stream->write_queue_size -= req_size;

  /* Unqueue request, regardless of immediateness */
  QUEUE_REMOVE(&req.queue);
  if (req.bufs != req.bufsml) free(req.bufs);
  req.bufs = NULL;

  if (written == 0 && req_size != 0)
    return req.error < 0 ? req.error : UV_EAGAIN;
  else
    return written;
}

int uvkcp_write2(
    uvkcp_write_t* req,
    uvkcp_t* stream,
    const uv_buf_t bufs[],
    unsigned int nbufs,
    uv_stream_t* send_handle,  // !!! not used, for compatibility with Node.js
    uvkcp_write_cb cb) {
  assert(send_handle == NULL);
  return uvkcp_write(req, stream, bufs, nbufs, cb);
}

int uvkcp_read_start(uvkcp_t *stream, uv_alloc_cb alloc_cb,
                     uvkcp_read_cb read_cb)
{
    uv_poll_t *poll = (uv_poll_t *)stream;
    assert(poll->type == UV_POLL);

    UVKCP_LOG_FUNC("uvkcp_read_start: Starting read operations");

    /* The UVKCP_FLAG_READABLE flag is irrelevant of the state of the tcp - it just
   * expresses the desired state of the user.
   */
    stream->flags |= UVKCP_FLAG_READABLE;
    UVKCP_LOG("uvkcp_read_start: Set UVKCP_FLAG_READABLE flag");

    /* TODO: try to do the read inline? */
    /* TODO: keep track of tcp state. If we've gotten a EOF then we should
   * not start the IO watcher.
   */
    kcp_context_t* ctx = kcp__get_context(stream);
    if (!ctx || !ctx->kcp) {
        UVKCP_LOG_ERROR("uvkcp_read_start: Invalid KCP context or KCP instance");
        return -1;
    }

    assert(alloc_cb);

    stream->read_cb = read_cb;
    stream->alloc_cb = alloc_cb;
    UVKCP_LOG("uvkcp_read_start: Set read_cb=%p, alloc_cb=%p", read_cb, alloc_cb);

    if (uv_poll_start(poll, UV_READABLE, kcp__stream_io) < 0)
    {
        UVKCP_LOG_ERROR("uvkcp_read_start: Failed to start polling");
        return -1;
    }

    UVKCP_LOG("uvkcp_read_start: Successfully started polling for readable events");
    return 0;
}


int uvkcp_read_stop(uvkcp_t* stream) {
    uv_poll_t *poll = (uv_poll_t *)stream;
    assert(poll->type == UV_POLL);

    UVKCP_LOG_FUNC("uvkcp_read_stop: Stopping read operations");

    if (uv_poll_stop(poll) < 0)
    {
        UVKCP_LOG_ERROR("uvkcp_read_stop: Failed to stop polling");
        return -1;
    }

    stream->flags   &= ~UVKCP_FLAG_READABLE;
    stream->read_cb  = NULL;
    stream->alloc_cb = NULL;
    UVKCP_LOG("uvkcp_read_stop: Cleared UVKCP_FLAG_READABLE flag and callbacks");

    return 0;
}


int uvkcp_is_readable(uvkcp_t* stream) {
    return stream->flags & UVKCP_FLAG_READABLE;
}


int uvkcp_is_writable(uvkcp_t* stream) {
    return stream->flags & UVKCP_FLAG_WRITABLE;
}


int uvkcp_set_blocking(uvkcp_t* handle, int blocking) {
    // KCP is inherently non-blocking, this is a no-op
    return 0;
}


size_t uvkcp_get_write_queue_size(const uvkcp_t* stream) {
  return stream->write_queue_size;
}

int uvkcp_accept(uvkcp_t* server, uvkcp_t* client) {
  kcp_context_t* server_ctx = kcp__get_context(server);
  kcp_context_t* client_ctx = kcp__get_context(client);

  if (!server_ctx || !client_ctx) {
    return UV_EINVAL;
  }

  if (!server_ctx->is_listening) {
    return UV_EINVAL;
  }

  if (client_ctx->is_connected || client_ctx->is_listening) {
    return UV_EISCONN;
  }

  // For KCP over UDP, each client connection uses a separate UDP socket
  // Create a new UDP socket for this client connection
  int domain = (server_ctx->server_addr.ss_family == AF_INET) ? AF_INET : AF_INET6;
  int sock = socket(domain, SOCK_DGRAM, 0);
  if (sock < 0) {
    UVKCP_LOG_ERROR("Failed to create UDP socket for client: %s", strerror(errno));
    return uv_translate_sys_error(errno);
  }

  // Bind the client socket to the same address family as the server
  // Use port 0 to let the OS assign an available port
  struct sockaddr_storage client_addr;
  socklen_t client_addr_len = sizeof(client_addr);
  memset(&client_addr, 0, sizeof(client_addr));

  if (domain == AF_INET) {
    struct sockaddr_in *addr_in = (struct sockaddr_in*)&client_addr;
    addr_in->sin_family = AF_INET;
    addr_in->sin_addr.s_addr = INADDR_ANY;
    addr_in->sin_port = 0; // Let OS assign port
    client_addr_len = sizeof(struct sockaddr_in);
  } else {
    struct sockaddr_in6 *addr_in6 = (struct sockaddr_in6*)&client_addr;
    addr_in6->sin6_family = AF_INET6;
    addr_in6->sin6_addr = in6addr_any;
    addr_in6->sin6_port = 0; // Let OS assign port
    client_addr_len = sizeof(struct sockaddr_in6);
  }

  if (bind(sock, (struct sockaddr*)&client_addr, client_addr_len) < 0) {
    UVKCP_LOG_ERROR("Failed to bind client UDP socket: %s", strerror(errno));
    close(sock);
    return uv_translate_sys_error(errno);
  }

  client_ctx->udp_fd = sock;

  // Copy the peer address from server context (set by kcp__server_io)
  memcpy(&client_ctx->peer_addr, &server_ctx->peer_addr, sizeof(server_ctx->peer_addr));
  client_ctx->peer_addr_len = server_ctx->peer_addr_len;

  // KCP instance creation deferred until TCP handshake completes
  // TCP handshake required to exchange conversation ID and peer address
  client_ctx->kcp = NULL;
  client_ctx->is_connected = 0;

  // Open the stream using the client's dedicated UDP socket
  if (kcp__stream_open(client, sock, UVKCP_FLAG_READABLE | UVKCP_FLAG_WRITABLE) < 0) {
    UVKCP_LOG_ERROR("Failed to open client stream");
    close(sock);
    ikcp_release(client_ctx->kcp);
    client_ctx->kcp = NULL;
    return UV_EIO;
  }

  UVKCP_LOG("Accepted KCP client connection on dedicated fd=%d (TCP handshake required)", sock);
  return 0;
}