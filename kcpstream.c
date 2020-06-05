//////////////////////////////////////////////////////
// KCP stream interface implementation
// Copyright 2020, tom zhou<appnet.link@gmail.com>
//////////////////////////////////////////////////////

#include <stdlib.h>
#include <assert.h>
#include "kcp/ikcp.h"
#include "uvkcp.h"


#if 0
// consume KCP Os fd event
static void kcp_consume_osfd(uv_os_sock_t os_fd)
{
    char dummy;
    recv(os_fd, &dummy, sizeof(dummy), 0);
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
void kcp__stream_io(uv_poll_t *handle, int status, int events);

void kcp__stream_init(uv_loop_t* loop, uvkcp_t* stream) {
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
  QUEUE_INIT(&stream->write_queue);
  QUEUE_INIT(&stream->write_completed_queue);
  stream->write_queue_size = 0;
}

int kcp__stream_open(uvkcp_t* kcp, uv_os_sock_t fd, int flags) {
    uv_poll_t *poll = (uv_poll_t *)kcp;

    kcp->fd = fd;

    // init uv_poll_t
    if (uv_poll_init_socket(kcp->aloop, poll, fd) < 0)
    {
        kcp_close(kcp->kcpfd);
        return -1;
    }

    // start polling
    if (uv_poll_start(poll, UV_READABLE, kcp__stream_io) < 0)
    {
        kcp_close(kcp->kcpfd);
        return -1;
    }

    kcp->flags |= flags;

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

void kcp__server_io(uv_poll_t *handle, int status, int events) {
    uvkcp_t *stream = (uvkcp_t *)handle;
    int optlen;


    assert(handle->type == UV_POLL);
    assert(!(stream->flags & UVKCP_FLAG_CLOSING));

    // !!! always consume KCP/OSfd event here
    kcp_consume_osfd(stream->fd);

    if (stream->accepted_kcpfd != -1)
    {
        return;
    }
    
    while (stream->kcpfd != -1) {
        int kcpfd = kcp__accept(stream->kcpfd);

          ///fprintf(stdout, "func:%s, line:%d, errno: %d, %s\n", __FUNCTION__, __LINE__, kcp_getlasterror_code(), kcp_getlasterror_desc());

          if (kcpfd < 0) {
        ///fprintf(stdout, "func:%s, line:%d, errno: %d, %s\n", __FUNCTION__, __LINE__, kcp_getlasterror_code(), kcp_getlasterror_desc());

        if (kcp_getlasterror_code() == KCP_EASYNCRCV /*errno == EAGAIN || errno == EWOULDBLOCK*/) {
          /* No problem. */
          return;
        } else if (kcp_getlasterror_code() == KCP_ESECFAIL /*errno == ECONNABORTED*/) {
          /* ignore */
          continue;
        } else {
          //////kcp__set_sys_error(kcp->loop, uvkcp_translate_kcp_error());
          stream->connection_cb(stream, UV_ECONNREFUSED);
                  // check KCP socket state
                  if (KCP_LISTENING != kcp_getsockstate(stream->kcpfd)) {
                      ///kcp_consume_osfd(stream->fd);
                      return;
                  }
        }
      } else {
        stream->accepted_kcpfd = kcpfd;
        // fill Os fd in case socket broken
        if (kcp_getsockopt(kcpfd, 0, (int)KCP_KCP_OSFD, &stream->accepted_fd, &optlen)) {
                  stream->connection_cb(stream, UV_ECONNABORTED);
              } else {
                  stream->connection_cb(stream, 0);
              }
          }
          
          if (stream->accepted_kcpfd != -1) {
              /* The user hasn't yet accepted called uvkcp_accept() */
              return;
      }
  }
}

int uvkcp_accept(uvkcp_t* server, uvkcp_t* client) {
  uvkcp_t* streamServer;
  uvkcp_t* streamClient;
  int status;

  /* TODO document this */
  assert(server->aloop == client->aloop);

  status = -1;

  streamServer = (uvkcp_t*)server;
  streamClient = (uvkcp_t*)client;

  if (streamServer->accepted_kcpfd == -1) {
    goto out;
  }
  
  streamClient->kcpfd = streamServer->accepted_kcpfd;

  if (kcp__stream_open(streamClient, streamServer->accepted_fd,
        UVKCP_FLAG_READABLE | UVKCP_FLAG_WRITABLE)) {
    /* TODO handle error */
      // clear pending Os fd event
      kcp_consume_osfd(streamServer->accepted_fd);
      kcp_close(streamServer->accepted_kcpfd);

    streamServer->accepted_kcpfd = -1;
    goto out;
  }

  streamServer->accepted_kcpfd = -1;
  status = 0;

out:
  return status;
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
  
    //!!! close KCP socket anyway
    kcp_close(stream->kcpfd); 
  
    // KCP don't need drain
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


  if ((stream->flags & UVKCP_FLAG_CLOSING) ||
      (stream->flags & UVKCP_FLAG_CLOSED)) {
    /* Handle was closed this tick. We've received a stale
     * 'is writable' callback from the event loop, ignore.
     */
    return;
  }

  assert(stream->kcpfd != -1);

  /* Get the request at the head of the queue. */
  req = uvkcp_write_queue_head(stream);
  if (!req) {
    assert(stream->write_queue_size == 0);
    return;
  }

  assert(req->handle == stream);

  iov = &(req->bufs[req->write_index]);
  iovcnt = req->nbufs - req->write_index;

    {
      int next = 1, it = 0;
      n = -1;
      for (it = 0; it < iovcnt; it ++) {
        size_t ilen = 0;
        while (ilen < iov[it].len) {
          int rc = kcp_send(stream->kcpfd, ((char *)iov[it].base)+ilen, iov[it].len-ilen, 0);
          if (rc <= 0) {
            next = 0;
            break;
          } else  {
            if (n == -1) n = 0;
            n += rc;
            ilen += rc;
          }
        }
        if (next == 0) break;
      }
    }

  if (n < 0) {
      if (kcp_getlasterror_code() != KCP_EASYNCSND) {
          /* Error */
          req->error = uvkcp_translate_kcp_error();
          stream->write_queue_size -= kcp__write_req_size(req);
          kcp__write_req_finish(req);
          return;
       }
  } else {
    /* Successful write */

    /* Update the counters. */
    while (n >= 0) {
      uv_buf_t* buf = &(req->bufs[req->write_index]);
      size_t len = buf->len;

      assert(req->write_index < req->nbufs);

      if ((size_t)n < len) {
        buf->base += n;
        buf->len -= n;
        stream->write_queue_size -= n;
        n = 0;
        
        /* Break loop and ensure the watcher is pending. */
        break;
      } else {
        /* Finished writing the buf at index req->write_index. */
        req->write_index++;

        assert((size_t)n >= len);
        n -= len;

        assert(stream->write_queue_size >= len);
        stream->write_queue_size -= len;

        if (req->write_index == req->nbufs) {
          /* Then we're done! */
          assert(n == 0);
          kcp__write_req_finish(req);
          /* TODO: start trying to write the next request. */
          return;
        }
      }
    }
  }

  /* Either we've counted n down to zero or we've got EAGAIN. */
  assert(n == 0 || n == -1);

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


static void kcp__read(uvkcp_t* stream) {
  uv_buf_t buf;
  ssize_t nread;
  int count;


  /* Prevent loop starvation when the data comes in as fast as (or faster than)
   * we can read it. XXX Need to rearm fd if we switch to edge-triggered I/O.
   */
  count = 32;

  while ((stream->read_cb)
      && (stream->flags & UVKCP_FLAG_READABLE)
      && (count-- > 0)) {
    assert(stream->alloc_cb);
    
    buf = uv_buf_init(NULL, 0);
    stream->alloc_cb((uv_handle_t *)stream, 64 * 1024, &buf);
    if (buf.base == NULL || buf.len == 0)
    {
        /* User indicates it can't or won't handle the read. */
        stream->read_cb(stream, UV_ENOBUFS, &buf);
        return;
    }
    assert(buf.base != NULL);
    
    assert(stream->kcpfd != -1);

    // KCP recv
    {
        nread = kcp_recv(stream->kcpfd, buf.base, buf.len, 0);
        if (nread <= 0) {
            // consume Os fd event
            ///kcp_consume_osfd(stream->fd);
      }
        ///fprintf(stdout, "func:%s, line:%d, expect rd: %d, real rd: %d\n", __FUNCTION__, __LINE__, buf.len, nread);

      if (nread < 0) {
        /* Error */
        int kcperr = uvkcp_translate_kcp_error();

        if (kcperr == UV_EAGAIN) {
          /* Wait for the next one. */
                stream->read_cb(stream, 0, &buf);
          return;
        } else if ((kcperr == UV_EPIPE) || (kcperr == UV_ENOTSOCK)) {
                // socket broken or invalid socket as EOF
                stream->read_cb(stream, UV_EOF, &buf);
            return;
        } else {
          /* Error. User should call uv_close(). */
                stream->read_cb(stream, UV_EIO, &buf);
          return;
        }
      } else if (nread == 0) {
        // never go here
            stream->read_cb(stream, 0, &buf);
            return;
        } else {
        /* Successful read */
        ssize_t buflen = buf.len;
            stream->read_cb(stream, nread, &buf);

        /* Return if we didn't fill the buffer, there is no more data to read. */
        if (nread < buflen) {
          return;
        }
      }
    }
  }
}


int uvkcp_shutdown(uvkcp_shutdown_t* req, uvkcp_t* stream, uvkcp_shutdown_cb cb) {
  uv_poll_t *poll = (uv_poll_t *)stream;

  assert((poll->type == UV_POLL) &&
         "uvkcp_shutdown (unix) only supports uv_handle_t right now");
  assert(stream->kcpfd != -1);

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

void kcp__stream_io(uv_poll_t * handle, int status, int events) {
    uvkcp_t *stream = (uvkcp_t *)handle;

    assert(handle->type == UV_POLL);
    assert(stream->kcpfd != -1);

    // !!! always consume KCP/OSfd event here
    kcp_consume_osfd(stream->fd);

    if (stream->connect_req) {
      kcp__stream_connect(stream);
    } else {
    // check KCP event
      int kcpev, optlen;
      
      if (kcp_getsockopt(stream->kcpfd, 0, KCP_KCP_EVENT, &kcpev, &optlen) < 0) {
          // check error anyway
          kcp__read(stream);
          
          kcp__write(stream);
          kcp__write_callbacks(stream);
      } else {
          if (kcpev & (KCP_KCP_EPOLL_IN | KCP_KCP_EPOLL_ERR)) {
              kcp__read(stream);
          }
          if (kcpev & (KCP_KCP_EPOLL_OUT | KCP_KCP_EPOLL_ERR)) {
              kcp__write(stream);
              kcp__write_callbacks(stream);
      }
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

  assert(req);

  if (stream->delayed_error) {
    /* To smooth over the differences between unixes errors that
     * were reported synchronously on the first connect can be delayed
     * until the next tick--which is now.
     */
    error = stream->delayed_error;
    stream->delayed_error = 0;
  } else {
        /* Normal situation: we need to get the socket error from the kernel. */
        assert(stream->kcpfd != -1);
      
      // notes: check socket state until connect successfully
      switch (kcp_getsockstate(stream->kcpfd)) {
      case KCP_CONNECTED:
        error = 0;
        // consume Os fd event
        ///kcp_consume_osfd(stream->fd);
        break;
      case KCP_CONNECTING:
        error = UV_EALREADY;
        break;
      default:
        error = uvkcp_translate_kcp_error();
        // consume Os fd event
        ///kcp_consume_osfd(stream->fd);
        break;
      }
  }

  if (error == UV_EALREADY)
    return;

  stream->connect_req = NULL;

  if (req->cb) {
    //////kcp__set_sys_error(kcp->loop, error);
    req->cb(req, error ? UV_ECONNREFUSED : 0);
  }
}


/* The buffers to be written must remain valid until the callback is called.
 * This is not required for the uv_buf_t array.
 */
int uvkcp_write(uvkcp_write_t *req, uvkcp_t *stream, const uv_buf_t bufs[], unsigned int nbufs, uvkcp_write_cb cb)
{
    int empty_queue;

    if (stream->kcpfd < 0)
    {
        //////kcp__set_sys_error(kcp->loop, EBADF);
        return -1;
    }

    // check flags
    int flags = stream->flags;
    if (!(flags & UVKCP_FLAG_WRITABLE) ||
         (flags & UVKCP_FLAG_SHUTTING) || (flags & UVKCP_FLAG_SHUT) ||
         (flags & UVKCP_FLAG_CLOSING)  || (flags & UVKCP_FLAG_CLOSED))
    {
        printf("uvkcp write rejected\n");
        return -1;
    }

    empty_queue = (stream->write_queue_size == 0);

    /* Initialize the req */
    req->type = UVKCP_REQ_WRITE;
    req->cb = cb;
    req->handle = stream;
    req->error = 0;
    QUEUE_INIT(&req->queue);

    req->bufs = req->bufsml;
    if (nbufs > (sizeof(req->bufsml)/sizeof(req->bufsml[0])))
        req->bufs = malloc(nbufs * sizeof(bufs[0]));

    if (req->bufs == NULL)
        return UV_ENOMEM;

    memcpy(req->bufs, bufs, nbufs * sizeof(uv_buf_t));
    req->nbufs = nbufs;
    req->write_index = 0;
    stream->write_queue_size += kcp__buf_count(bufs, nbufs);

    /* Append the request to write_queue. */
    QUEUE_INSERT_TAIL(&stream->write_queue, &req->queue);

    /* If the queue was empty when this function began, we should attempt to
   * do the write immediately. Otherwise start the write_watcher and wait
   * for the fd to become writable.
   */
    if (stream->connect_req)
    {
        /* Still connecting, do nothing. */
    }
    else if (empty_queue)
    {
        kcp__write(stream);
    } else {
        printf("uvkcp write no buffer\n");
        return UV_ENOBUFS;
    }

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
  if (stream->connect_req != NULL || stream->write_queue_size != 0)
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
    
    /* The UVKCP_FLAG_READABLE flag is irrelevant of the state of the tcp - it just
   * expresses the desired state of the user.
   */
    stream->flags |= UVKCP_FLAG_READABLE;

    /* TODO: try to do the read inline? */
    /* TODO: keep track of tcp state. If we've gotten a EOF then we should
   * not start the IO watcher.
   */
    assert(stream->kcpfd != -1);
    assert(alloc_cb);

    stream->read_cb = read_cb;
    stream->alloc_cb = alloc_cb;

    if (uv_poll_start(poll, UV_READABLE, kcp__stream_io) < 0)
    {
        return -1;
    }

    return 0;
}


int uvkcp_read_stop(uvkcp_t* stream) {
    uv_poll_t *poll = (uv_poll_t *)stream;
    assert(poll->type == UV_POLL);

    if (uv_poll_stop(poll) < 0)
    {
        return -1;
    }
    
    stream->flags   &= ~UVKCP_FLAG_READABLE;
    stream->read_cb  = NULL;
    stream->alloc_cb = NULL;
    
    return 0;
}


int uvkcp_is_readable(uvkcp_t* stream) {
    return stream->flags & UVKCP_FLAG_READABLE;
}


int uvkcp_is_writable(uvkcp_t* stream) {
    return stream->flags & UVKCP_FLAG_WRITABLE;
}


int uvkcp_set_blocking(uvkcp_t* handle, int blocking) {
    return kcp__nonblock(handle->kcpfd, !blocking);
}


size_t uvkcp_get_write_queue_size(const uvkcp_t* stream) {
  return stream->write_queue_size;
}

#endif