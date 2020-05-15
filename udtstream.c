//////////////////////////////////////////////////////
// UDT4 stream interface implementation
// Copyright 2020, tom zhou<appnet.link@gmail.com>
//////////////////////////////////////////////////////

#include "udtc.h"
#include "uvudt.h"
#include <assert.h>


// consume UDT Os fd event
static void udt_consume_osfd(uv_os_sock_t os_fd)
{
	char dummy;
    recv(os_fd, &dummy, sizeof(dummy), 0);
}

static size_t udt__buf_count(const uv_buf_t bufs[], int nbufs)
{
    size_t total = 0;
    int i;

    for (i = 0; i < nbufs; i++)
    {
        total += bufs[i].len;
    }

    return total;
}

static void udt__stream_connect(uvudt_t *stream);
static void udt__write(uvudt_t *stream);
static void udt__read(uvudt_t *stream);
void udt__stream_io(uv_poll_t *handle, int status, int events);

void udt__stream_init(uv_loop_t* loop, uvudt_t* stream) {
  // hold loop
  stream->aloop = loop;
  stream->flags = 0;

  stream->alloc_cb = NULL;
  stream->read_cb = NULL;
  stream->connection_cb = NULL;
  stream->connect_req = NULL;
  stream->shutdown_req = NULL;
  stream->accepted_udtfd = -1;
  stream->udtfd = -1;
  stream->udpfd = -1;
  stream->fd = -1;
  stream->accepted_udtfd = -1;
  stream->accepted_udpfd = -1;
  stream->accepted_fd = -1;
  stream->delayed_error = 0;
  QUEUE_INIT(&stream->write_queue);
  QUEUE_INIT(&stream->write_completed_queue);
  stream->write_queue_size = 0;
}

int udt__stream_open(uvudt_t* udt, uv_os_sock_t fd, int flags) {
    uv_poll_t *poll = (uv_poll_t *)udt;

    udt->fd = fd;

    // init uv_poll_t
    if (uv_poll_init_socket(udt->aloop, poll, fd) < 0)
    {
        udt_close(udt->udtfd);
        return -1;
    }

    // start polling
    if (uv_poll_start(poll, UV_READABLE, udt__stream_io) < 0)
    {
        udt_close(udt->udtfd);
        return -1;
    }

    udt->flags |= flags;

    return 0;
}


void udt__stream_destroy(uvudt_t* stream) {
  uvudt_write_t* req;
  QUEUE *q;
  uv_poll_t *poll = (uv_poll_t *)stream;


  if (stream->connect_req) {
      stream->connect_req->cb(stream->connect_req, -1);
      stream->connect_req = NULL;
  }

  while (!QUEUE_EMPTY(&stream->write_queue))
  {
      q = QUEUE_HEAD(&stream->write_queue);
      QUEUE_REMOVE(q);

      req = QUEUE_DATA(q, uvudt_write_t, queue);
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

      req = QUEUE_DATA(q, uvudt_write_t, queue);

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

void udt__server_io(uv_poll_t *handle, int status, int events) {
    uvudt_t *stream = (uvudt_t *)handle;
    int fd, udtfd, optlen;


    assert(handle->type == UV_POLL);
    assert(!(stream->flags & UVUDT_FLAG_CLOSING));

    // !!! always consume UDT/OSfd event here
    udt_consume_osfd(stream->fd);

    if (stream->accepted_udtfd != -1)
    {
        return;
    }
    
    while (stream->udtfd != -1) {
		  udtfd = udt__accept(stream->udtfd);

          ///fprintf(stdout, "func:%s, line:%d, errno: %d, %s\n", __FUNCTION__, __LINE__, udt_getlasterror_code(), udt_getlasterror_desc());

          if (udtfd < 0) {
			  ///fprintf(stdout, "func:%s, line:%d, errno: %d, %s\n", __FUNCTION__, __LINE__, udt_getlasterror_code(), udt_getlasterror_desc());

			  if (udt_getlasterror_code() == UDT_EASYNCRCV /*errno == EAGAIN || errno == EWOULDBLOCK*/) {
				  /* No problem. */
				  return;
			  } else if (udt_getlasterror_code() == UDT_ESECFAIL /*errno == ECONNABORTED*/) {
				  /* ignore */
				  continue;
			  } else {
				  //////udt__set_sys_error(udt->loop, uvudt_translate_udt_error());
				  stream->connection_cb(stream, UV_ECONNREFUSED);
                  // check UDT socket state
                  if (UDT_LISTENING != udt_getsockstate(stream->udtfd)) {
                      ///udt_consume_osfd(stream->fd);
                      return;
                  }
			  }
		  } else {
			  stream->accepted_udtfd = udtfd;
			  // fill Os fd in case socket broken
			  if (udt_getsockopt(udtfd, 0, (int)UDT_UDT_OSFD, &stream->accepted_fd, &optlen)) {
                  stream->connection_cb(stream, UV_ECONNABORTED);
              } else {
                  stream->connection_cb(stream, 0);
              }
          }
          
          if (stream->accepted_udtfd != -1) {
              /* The user hasn't yet accepted called uvudt_accept() */
              return;
		  }
  }
}


int uvudt_accept(uvudt_t* server, uvudt_t* client) {
  uvudt_t* streamServer;
  uvudt_t* streamClient;
  int status;
  uv_poll_t *srvpoll = &server->poll;
  uv_poll_t *clnpoll = &client->poll;

  /* TODO document this */
  assert(server->aloop == client->aloop);

  status = -1;

  streamServer = (uvudt_t*)server;
  streamClient = (uvudt_t*)client;

  if (streamServer->accepted_udtfd == -1) {
    goto out;
  }
  
  streamClient->udtfd = streamServer->accepted_udtfd;

  if (udt__stream_open(streamClient, streamServer->accepted_fd,
        UVUDT_FLAG_READABLE | UVUDT_FLAG_WRITABLE)) {
	  /* TODO handle error */
      // clear pending Os fd event
      udt_consume_osfd(streamServer->accepted_fd);
      udt_close(streamServer->accepted_udtfd);

	  streamServer->accepted_udtfd = -1;
	  goto out;
  }

  streamServer->accepted_udtfd = -1;
  status = 0;

out:
  return status;
}


uvudt_write_t* uvudt_write_queue_head(uvudt_t* stream) {
  QUEUE* q;
  uvudt_write_t* req;

  if (QUEUE_EMPTY(&stream->write_queue)) {
    return NULL;
  }

  q = QUEUE_HEAD(&stream->write_queue);
  if (!q) {
    return NULL;
  }

  req = QUEUE_DATA(q, uvudt_write_t, queue);
  assert(req);

  return req;
}


static void udt__drain(uvudt_t* stream) {
  uvudt_shutdown_t* req;
  uv_poll_t *poll = (uv_poll_t *)stream;

  assert(!uvudt_write_queue_head(stream));
  assert(stream->write_queue_size == 0);

  /* Shutdown? */
  if (( stream->flags & UVUDT_FLAG_SHUTTING) &&
      !(stream->flags & UVUDT_FLAG_CLOSING) &&
      !(stream->flags & UVUDT_FLAG_SHUT)) {
    assert(stream->shutdown_req);

    req = stream->shutdown_req;
    stream->shutdown_req = NULL;
  
    //!!! close UDT socket anyway
    udt_close(stream->udtfd); 
  
    // UDT don't need drain
    stream->flags &= ~UVUDT_FLAG_SHUTTING;
    stream->flags |=  UVUDT_FLAG_SHUT;
    if (req->cb) {
        req->cb(req, 0);
    }
  }
}


static size_t udt__write_req_size(uvudt_write_t* req) {
  size_t size;

  size = udt__buf_count(req->bufs + req->write_index,
                        req->nbufs - req->write_index);
  assert(req->handle->write_queue_size >= size);

  return size;
}


static void udt__write_req_finish(uvudt_write_t* req) {
  uvudt_t* stream = req->handle;
  uv_poll_t *poll = (uv_poll_t *)stream;

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

  // UDT always polling on read event
  /// udt__io_feed(udt->loop, &stream->write_watcher, UV__IO_READ);

}


/* On success returns NULL. On error returns a pointer to the write request
 * which had the error.
 */
static void udt__write(uvudt_t* stream) {
  uvudt_write_t* req;
  uv_buf_t *iov;
  int iovcnt;
  ssize_t n;
  uv_poll_t *poll = (uv_poll_t *)stream;


  if ((stream->flags & UVUDT_FLAG_CLOSING) ||
      (stream->flags & UVUDT_FLAG_CLOSED)) {
    /* Handle was closed this tick. We've received a stale
     * 'is writable' callback from the event loop, ignore.
     */
    return;
  }

  assert(stream->udtfd != -1);

  /* Get the request at the head of the queue. */
  req = uvudt_write_queue_head(stream);
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
				  int rc = udt_send(stream->udtfd, ((char *)iov[it].base)+ilen, iov[it].len-ilen, 0);
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
      if (udt_getlasterror_code() != UDT_EASYNCSND) {
          /* Error */
          req->error = uvudt_translate_udt_error();
          stream->write_queue_size -= udt__write_req_size(req);
          udt__write_req_finish(req);
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
          udt__write_req_finish(req);
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

static void udt__write_callbacks(uvudt_t* stream) {
  uvudt_write_t* req;
  QUEUE* q;
  uv_poll_t *poll = (uv_poll_t *)stream;

  while (!QUEUE_EMPTY(&stream->write_completed_queue)) {
    /* Pop a req off write_completed_queue. */
    q = QUEUE_HEAD(&stream->write_completed_queue);
    req = QUEUE_DATA(q, uvudt_write_t, queue);
    QUEUE_REMOVE(q);

    /* NOTE: call callback AFTER freeing the request data. */
    if (req->cb) {
      req->cb(req, req->error ? -1 : 0);
    }
  }

  assert(QUEUE_EMPTY(&stream->write_completed_queue));

  /* Write queue drained. */
  if (!uvudt_write_queue_head(stream)) {
    udt__drain(stream);
  }
}


static void udt__read(uvudt_t* stream) {
  uv_buf_t buf;
  ssize_t nread;
  int count;
  uv_poll_t *poll = (uv_poll_t *)stream;


  /* Prevent loop starvation when the data comes in as fast as (or faster than)
   * we can read it. XXX Need to rearm fd if we switch to edge-triggered I/O.
   */
  count = 32;

  while ((stream->read_cb)
      && (stream->flags & UVUDT_FLAG_READABLE)
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
    
    assert(stream->udtfd != -1);

    // UDT recv
    {
        nread = udt_recv(stream->udtfd, buf.base, buf.len, 0);
        if (nread <= 0) {
            // consume Os fd event
            ///udt_consume_osfd(stream->fd);
    	}
        ///fprintf(stdout, "func:%s, line:%d, expect rd: %d, real rd: %d\n", __FUNCTION__, __LINE__, buf.len, nread);

    	if (nread < 0) {
    		/* Error */
    		int udterr = uvudt_translate_udt_error();

    		if (udterr == UV_EAGAIN) {
    			/* Wait for the next one. */
                stream->read_cb(stream, 0, &buf);
    			return;
    		} else if ((udterr == UV_EPIPE) || (udterr == UV_ENOTSOCK)) {
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


int uvudt_shutdown(uvudt_shutdown_t* req, uvudt_t* stream, uvudt_shutdown_cb cb) {
  uv_poll_t *poll = (uv_poll_t *)stream;

  assert((poll->type == UV_POLL) &&
         "uvudt_shutdown (unix) only supports uv_handle_t right now");
  assert(stream->udtfd != -1);

  if (!(stream->flags & UVUDT_FLAG_WRITABLE) ||
        stream->flags & UVUDT_FLAG_SHUT ||
        stream->flags & UVUDT_FLAG_CLOSED ||
        stream->flags & UVUDT_FLAG_CLOSING) {
    return -1;
  }

  /* Initialize request */
  req->type = UVUDT_REQ_SHUTDOWN;
  req->handle = stream;
  req->cb = cb;
  stream->shutdown_req = req;
  stream->flags |= UVUDT_FLAG_SHUTTING;

  return 0;
}

void udt__stream_io(uv_poll_t * handle, int status, int events) {
    uvudt_t *stream = (uvudt_t *)handle;

    assert(handle->type == UV_POLL);
    assert(stream->udtfd != -1);

    // !!! always consume UDT/OSfd event here
    udt_consume_osfd(stream->fd);

    if (stream->connect_req) {
      udt__stream_connect(stream);
    } else {
	  // check UDT event
      int udtev, optlen;
      
      if (udt_getsockopt(stream->udtfd, 0, UDT_UDT_EVENT, &udtev, &optlen) < 0) {
          // check error anyway
          udt__read(stream);
          
          udt__write(stream);
          udt__write_callbacks(stream);
      } else {
          if (udtev & (UDT_UDT_EPOLL_IN | UDT_UDT_EPOLL_ERR)) {
              udt__read(stream);
          }
          if (udtev & (UDT_UDT_EPOLL_OUT | UDT_UDT_EPOLL_ERR)) {
              udt__write(stream);
              udt__write_callbacks(stream);
		  }
      }
    }
}


/**
 * We get called here from directly following a call to connect(2).
 * In order to determine if we've errored out or succeeded must call
 * getsockopt.
 */
static void udt__stream_connect(uvudt_t* stream) {
  int error;
  uvudt_connect_t* req = stream->connect_req;
  socklen_t errorsize = sizeof(int);
  uv_poll_t *poll = (uv_poll_t *)stream;

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
	      assert(stream->udtfd != -1);
      
		  // notes: check socket state until connect successfully
		  switch (udt_getsockstate(stream->udtfd)) {
		  case UDT_CONNECTED:
			  error = 0;
			  // consume Os fd event
			  ///udt_consume_osfd(stream->fd);
			  break;
		  case UDT_CONNECTING:
			  error = UV_EALREADY;
			  break;
		  default:
			  error = uvudt_translate_udt_error();
			  // consume Os fd event
			  ///udt_consume_osfd(stream->fd);
			  break;
		  }
  }

  if (error == UV_EALREADY)
    return;

  stream->connect_req = NULL;

  if (req->cb) {
    //////udt__set_sys_error(udt->loop, error);
    req->cb(req, error ? UV_ECONNREFUSED : 0);
  }
}


/* The buffers to be written must remain valid until the callback is called.
 * This is not required for the uv_buf_t array.
 */
int uvudt_write(uvudt_write_t *req, uvudt_t *stream, const uv_buf_t bufs[], unsigned int nbufs, uvudt_write_cb cb)
{
    int empty_queue;
    uv_poll_t *poll = (uv_poll_t *)stream;

    if (stream->udtfd < 0)
    {
        //////udt__set_sys_error(udt->loop, EBADF);
        return -1;
    }

    // check flags
    int flags = stream->flags;
    if (!(flags & UVUDT_FLAG_WRITABLE) ||
         (flags & UVUDT_FLAG_SHUTTING) || (flags & UVUDT_FLAG_SHUT) ||
         (flags & UVUDT_FLAG_CLOSING)  || (flags & UVUDT_FLAG_CLOSED))
    {
        printf("uvudt write rejected\n");
        return -1;
    }

    empty_queue = (stream->write_queue_size == 0);

    /* Initialize the req */
    req->type = UVUDT_REQ_WRITE;
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
    stream->write_queue_size += udt__buf_count(bufs, nbufs);

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
        udt__write(stream);
    } else {
        printf("uvudt write no buffer\n");
        return UV_ENOBUFS;
    }

    return 0;
}

static void uvudt_try_write_cb(uvudt_write_t* req, int status) {
  /* Should not be called */
  abort();
}

int uvudt_try_write(uvudt_t* stream,
                    const uv_buf_t bufs[],
                    unsigned int nbufs) {
  int r;
  size_t written;
  size_t req_size;
  uvudt_write_t req;

  /* Connecting or already writing some data */
  if (stream->connect_req != NULL || stream->write_queue_size != 0)
    return UV_EAGAIN;

  r = uvudt_write(&req, stream, bufs, nbufs, uvudt_try_write_cb);
  if (r != 0) return r;

  /* Remove not written bytes from write queue size */
  written = udt__buf_count(bufs, nbufs);
  if (req.bufs != NULL)
    req_size = udt__write_req_size(&req);
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

int uvudt_write2(
    uvudt_write_t* req,
    uvudt_t* stream,
    const uv_buf_t bufs[],
    unsigned int nbufs,
    uv_stream_t* send_handle,  // !!! not used, for compatibility with Node.js
    uvudt_write_cb cb) {
  assert(send_handle == NULL);
  return uvudt_write(req, stream, bufs, nbufs, cb);
}

int uvudt_read_start(uvudt_t *stream, uv_alloc_cb alloc_cb,
                     uvudt_read_cb read_cb)
{
    uv_poll_t *poll = (uv_poll_t *)stream;
    assert(poll->type == UV_POLL);
    
    /* The UVUDT_FLAG_READABLE flag is irrelevant of the state of the tcp - it just
   * expresses the desired state of the user.
   */
    stream->flags |= UVUDT_FLAG_READABLE;

    /* TODO: try to do the read inline? */
    /* TODO: keep track of tcp state. If we've gotten a EOF then we should
   * not start the IO watcher.
   */
    assert(stream->udtfd != -1);
    assert(alloc_cb);

    stream->read_cb = read_cb;
    stream->alloc_cb = alloc_cb;

    if (uv_poll_start(poll, UV_READABLE, udt__stream_io) < 0)
    {
        return -1;
    }

    return 0;
}


int uvudt_read_stop(uvudt_t* stream) {
    uv_poll_t *poll = (uv_poll_t *)stream;
    assert(poll->type == UV_POLL);

    if (uv_poll_stop(poll) < 0)
    {
        return -1;
    }
    
    stream->flags   &= ~UVUDT_FLAG_READABLE;
    stream->read_cb  = NULL;
    stream->alloc_cb = NULL;
    
    return 0;
}


int uvudt_is_readable(uvudt_t* stream) {
    return stream->flags & UVUDT_FLAG_READABLE;
}


int uvudt_is_writable(uvudt_t* stream) {
    return stream->flags & UVUDT_FLAG_WRITABLE;
}


int uvudt_set_blocking(uvudt_t* handle, int blocking) {
    return udt__nonblock(!blocking);
}


size_t uvudt_get_write_queue_size(const uvudt_t* stream) {
  return stream->write_queue_size;
}
