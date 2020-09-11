/******************************************************************************
pkconn.c - Connection objects

This file is Copyright 2011-2020, The Beanstalks Project ehf.

This program is free software: you can redistribute it and/or modify it under
the terms  of the  Apache  License 2.0  as published by the  Apache  Software
Foundation.

This program is distributed in the hope that it will be useful,  but  WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE.  See the Apache License for more details.

You should have received a copy of the Apache License along with this program.
If not, see: <http://www.apache.org/licenses/>

Note: For alternate license terms, see the file COPYING.md.

******************************************************************************/

#define PAGEKITE_CONSTANTS_ONLY
#include "pagekite.h"

#include "pkcommon.h"
#include "pkutils.h"
#include "pkerror.h"
#include "pkhooks.h"
#include "pkconn.h"
#include "pkproto.h"
#include "pkstate.h"
#include "pkblocker.h"
#include "pkmanager.h"
#include "pklogging.h"

#include <ctype.h>


void pkc_reset_conn(struct pk_conn* pkc, unsigned int status)
{
  PK_ADD_MEMORY_CANARY(pkc);
  if ((pkc->status & CONN_STATUS_CHANGING) && !(status & CONN_STATUS_CHANGING)) {
    /* This will warn about the reset unless the status argument
     * explicitly says this is part of an ongoing change. */
    pk_log(PK_LOG_ERROR,
           "%d: BUG! Attempt to reset conn mid-change!", pkc->sockfd);
  }
  pkc->status &= ~CONN_STATUS_BITS;
  pkc->status |= status;
  pkc->activity = pk_time();
  pkc->out_buffer_pos = 0;
  pkc->in_buffer_pos = 0;
  pkc->send_window_kb = CONN_WINDOW_SIZE_KB_INITIAL;
  pkc->read_bytes = 0;
  pkc->read_kb = 0;
  pkc->sent_kb = 0;
  pkc->wrote_bytes = 0;
  pkc->reported_kb = 0;
  if (pkc->sockfd >= 0) PKS_close(pkc->sockfd);
  pkc->sockfd = -1;
  pkc->state = CONN_CLEAR_DATA;
#ifdef HAVE_OPENSSL
  if (pkc->ssl) SSL_free(pkc->ssl);
  pkc->ssl = NULL;
  pkc->want_write = 0;
#endif
}


int pkc_connect(struct pk_conn* pkc, struct addrinfo* ai)
{
  struct timeval to;
  int fd;
  to.tv_sec = pk_state.socket_timeout_s;
  to.tv_usec = 0;
  pkc_reset_conn(pkc, CONN_STATUS_CHANGING|CONN_STATUS_ALLOCATED);
  if ((0 > (fd = PKS_socket(ai->ai_family, ai->ai_socktype,
                            ai->ai_protocol))) ||
      PKS_fail(PKS_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (char *) &to, sizeof(to))) ||
      PKS_fail(PKS_setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (char *) &to, sizeof(to))) ||
      PKS_fail(PKS_connect(fd, ai->ai_addr, ai->ai_addrlen))) {
    pkc->sockfd = -1;
    if (fd >= 0) PKS_close(fd);
    return (pk_error = ERR_CONNECT_CONNECT);
  }

/* Uncomment to poorly simulate a bad network
  int s = rand() % 15;
  fprintf(stderr, "TESTING SLEEP %d\n", s);
  sleep(s);
 */

  /* FIXME: Add support for chaining through socks or HTTP proxies */
  return (pkc->sockfd = fd);
}

int pkc_listen(struct pk_conn* pkc, struct addrinfo* ai, int backlog)
{
  int fd;
  struct sockaddr_in sin;
  socklen_t len = sizeof(sin);

  pkc_reset_conn(pkc, CONN_STATUS_CHANGING |
                      CONN_STATUS_ALLOCATED |
                      CONN_STATUS_LISTENING);
  if ((0 > (fd = PKS_socket(ai->ai_family, ai->ai_socktype,
                            ai->ai_protocol))) ||
      PKS_fail(PKS_bind(fd, ai->ai_addr, ai->ai_addrlen)) ||
      PKS_fail(PKS_listen(fd, backlog))) {
    pkc->sockfd = -1;
    if (fd >= 0) PKS_close(fd);
    return (pk_error = ERR_CONNECT_LISTEN);
  }
  pkc->sockfd = fd;

  if (getsockname(pkc->sockfd, (struct sockaddr *)&sin, &len) != -1)
    return ntohs(sin.sin_port);

  return 1;
}

static void pkc_reset_error_state()
{
#ifdef HAVE_OPENSSL
  unsigned long ssl_errno;
  char message[257];
  while (0 != (ssl_errno = ERR_get_error())) {
    pk_log(PK_LOG_BE_DATA|PK_LOG_TUNNEL_DATA,
           "Cleared queued SSL ERROR=%ld: %s",
           ssl_errno, ERR_error_string(ssl_errno, message));
  }
  ERR_clear_error();
#endif
  errno = 0;
}

#ifdef HAVE_OPENSSL
static void pkc_start_handshake(struct pk_conn* pkc, int err)
{
  pk_log(PK_LOG_BE_DATA|PK_LOG_TUNNEL_DATA,
         "%d: Started SSL handshake", pkc->sockfd);

  pkc->state = CONN_SSL_HANDSHAKE;
  if (err == SSL_ERROR_WANT_READ) {
    pkc->status |= CONN_STATUS_WANT_READ;
  }
  else if (err == SSL_ERROR_WANT_WRITE) {
    pkc->status |= CONN_STATUS_WANT_WRITE;
  }
}

static void pkc_end_handshake(struct pk_conn *pkc)
{
  char tls_info[256];

  SSL_CIPHER_description(SSL_get_current_cipher(pkc->ssl), tls_info, 256);
  pk_log(PK_LOG_BE_CONNS|PK_LOG_TUNNEL_CONNS,
         "%d: %s connection established: %s", pkc->sockfd,
         SSL_get_version(pkc->ssl),
         collapse_whitespace(tls_info));

  pkc->status &= ~(CONN_STATUS_WANT_WRITE|CONN_STATUS_WANT_READ);
  pkc->state = CONN_SSL_DATA;
}

static void pkc_do_handshake(struct pk_conn *pkc)
{
  int rv;
  pkc_reset_error_state();
  rv = SSL_do_handshake(pkc->ssl);
  if (rv == 1) {
    pkc_end_handshake(pkc);
  }
  else {
    int err = SSL_get_error(pkc->ssl, rv);
    switch (err) {
      case SSL_ERROR_WANT_READ:
        pkc->status |= CONN_STATUS_WANT_READ;
        break;
      case SSL_ERROR_WANT_WRITE:
        pkc->status |= CONN_STATUS_WANT_WRITE;
        break;
      default:
        pk_log(PK_LOG_BE_CONNS|PK_LOG_TUNNEL_CONNS,
               "%d: TLS handshake failed!", pkc->sockfd);
        pkc->status |= CONN_STATUS_BROKEN;
        errno = ECONNRESET;
        break;
    }
  }
}

int pkc_start_ssl(struct pk_conn* pkc, SSL_CTX* ctx, const char* hostname)
{
  long mode;

  mode = SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER;
  mode |= SSL_MODE_AUTO_RETRY;
  mode |= SSL_MODE_ENABLE_PARTIAL_WRITE;
#ifdef SSL_MODE_RELEASE_BUFFERS
  mode |= SSL_MODE_RELEASE_BUFFERS;
#endif

  /* If we have a global preference for particular certificate names in
   * pk_state, do not use the hostname directly as it may give something
   * completely different. */
  if (pk_state.ssl_cert_names)
    if (pk_state.ssl_cert_names[0] != NULL &&
        pk_state.ssl_cert_names[1] == NULL) {
      /* If we only care for one cert name, ask for it. */
      hostname = pk_state.ssl_cert_names[0];
    } else {
      /* Otherwise, just disable SNI */
      hostname = NULL;
    }

  long sm, sa, sc, sf, st;
  sm = sa = sc = sf = st = -1;
  if ((NULL == (pkc->ssl = SSL_new(ctx))) ||
      (mode != (mode & (sm = SSL_set_mode(pkc->ssl, mode)))) ||
      (1 != (sa = SSL_set_app_data(pkc->ssl, pkc))) ||
      (1 != (sc = SSL_set_cipher_list(pkc->ssl, pk_state.ssl_ciphers))) ||
      (1 != (sf = SSL_set_fd(pkc->ssl, PKS(pkc->sockfd)))) ||
      /* FIXME: This should be the certificate name we will validate against */
      (1 != (st = ((hostname == NULL) ? 1 : SSL_set_tlsext_host_name(pkc->ssl, hostname)))))
  {
    if (pkc->ssl != NULL) SSL_free(pkc->ssl);
    pkc->ssl = NULL;
    pk_log(PK_LOG_BE_CONNS | PK_LOG_TUNNEL_CONNS | PK_LOG_ERROR,
           "%d[pkc_start_ssl]: Failed to prepare SSL object!"
           " (ssl=%p, sm=%ld, sa=%ld, sc=%ld, sf=%ld, st=%ld)",
           pkc->sockfd, pkc->ssl, sm, sa, sc, sf, st);
    return -1;
  }

  pk_log(PK_LOG_BE_DATA|PK_LOG_TUNNEL_DATA,
         "%d[pkc_start_ssl]: Starting TLS connection with %s",
         pkc->sockfd, hostname ? hostname : "default");

  SSL_set_connect_state(pkc->ssl);
  pkc_start_handshake(pkc, SSL_ERROR_WANT_WRITE);
  pkc_do_handshake(pkc);

  return (pkc->status & CONN_STATUS_BROKEN) ? -1 : 0;
}
#endif

int pkc_wait(struct pk_conn* pkc, int timeout_ms)
{
  int rv;
  set_non_blocking(pkc->sockfd);
  do {
    PK_TRACE_LOOP("waiting");
    rv = wait_fd(pkc->sockfd, timeout_ms);
  } while ((rv < 0) && (errno == EINTR));
  if (0 > set_blocking(pkc->sockfd))
    pk_log(PK_LOG_BE_DATA|PK_LOG_TUNNEL_DATA|PK_LOG_ERROR,
           "%d[pkc_wait]: Failed to set socket blocking", pkc->sockfd);
  return rv;
}

ssize_t pkc_read(struct pk_conn* pkc)
{
  char *errfmt;
  ssize_t bytes, delta;
  int ssl_errno = SSL_ERROR_NONE;

  switch (pkc->state) {
#ifdef HAVE_OPENSSL
    case CONN_SSL_DATA:
      pkc_reset_error_state();
      bytes = SSL_read(pkc->ssl, PKC_IN(*pkc), PKC_IN_FREE(*pkc));
      if (bytes < 0) ssl_errno = SSL_get_error(pkc->ssl, bytes);
      break;
    case CONN_SSL_HANDSHAKE:
      if (!(pkc->status & CONN_STATUS_BROKEN)) {
        pkc_do_handshake(pkc);
        return 0;
      }
      bytes = 0;
#endif
    default:
      bytes = PKS_read(pkc->sockfd, PKC_IN(*pkc), PKC_IN_FREE(*pkc));
  }

  if (bytes > 0) {
    if (pk_state.log_mask & PK_LOG_TRACE) {
      pk_log_raw_data(PK_LOG_TRACE, "R", pkc->sockfd, PKC_IN(*pkc), bytes);
    }

    pkc->in_buffer_pos += bytes;
    pkc->activity = pk_time(0);

    /* Update KB counter and window... this is a bit messy. */
    pkc->read_bytes += bytes;
    while (pkc->read_bytes >= 1024) {
      pkc->read_kb += 1;
      pkc->read_bytes -= 1024;
    }
  }
  else if (bytes == 0) {
    pk_log(PK_LOG_BE_DATA|PK_LOG_TUNNEL_DATA, "pkc_read() hit EOF");
    pkc->status |= CONN_STATUS_CLS_READ;
  }
  else {
    errfmt = "%d: pkc_read(), errno=%d, ssl_errno=%d";
#ifdef HAVE_OPENSSL
    switch (ssl_errno) {
      case SSL_ERROR_WANT_WRITE:
        errfmt = "%d: pkc_read() starting handshake, errno=%d, ssl_errno=%d";
        pkc_start_handshake(pkc, ssl_errno);
        break;
      case SSL_ERROR_WANT_READ:
      case SSL_ERROR_SYSCALL:
      case SSL_ERROR_NONE:
#endif
        switch (errno) {
          case 0:
          case EINTR:
          case EAGAIN:
            errfmt = "%d: pkc_read() should retry, errno=%d, ssl_errno=%d";
            break;
          default:
            pkc->status |= CONN_STATUS_BROKEN;
            errfmt = "%d: pkc_read() broken, errno=%d, ssl_errno=%d";
            break;
        }
#ifdef HAVE_OPENSSL
        break;
      default:
        pkc->status |= CONN_STATUS_BROKEN;
        errfmt = "%d: pkc_read() broken, errno=%d, ssl_errno=%d";
        break;
    }
    pk_log(PK_LOG_BE_DATA|PK_LOG_TUNNEL_DATA,
           errfmt, pkc->sockfd, errno, ssl_errno);
#endif
  }
  return bytes;
}

int pkc_pending(struct pk_conn* pkc)
{
#ifdef HAVE_OPENSSL
  switch (pkc->state) {
    case CONN_SSL_DATA:
    case CONN_SSL_HANDSHAKE:
      return SSL_pending(pkc->ssl);
  }
#endif
  return 0;
}

ssize_t pkc_raw_write(struct pk_conn* pkc, char* data, ssize_t length) {
  ssize_t wrote = 0;
  pkc_reset_error_state();
  switch (pkc->state) {
#ifdef HAVE_OPENSSL
    case CONN_SSL_DATA:
      if (pkc->want_write > 0) length = pkc->want_write;
      pkc->want_write = 0;
      if (length) {
        wrote = SSL_write(pkc->ssl, data, length);
        if (wrote < 0) {
          int err = SSL_get_error(pkc->ssl, wrote);
          switch (err) {
            case SSL_ERROR_NONE:
              break;
            case SSL_ERROR_WANT_WRITE:
              pk_log(PK_LOG_BE_DATA|PK_LOG_TUNNEL_DATA,
                     "%d: %p/%d/%d/WANT_WRITE", pkc->sockfd, data, wrote, length);
              pkc->status |= CONN_STATUS_WANT_WRITE;
              pkc->want_write = length;
              break;
            default:
              if (0 == errno) errno = EIO;
              pk_log(PK_LOG_BE_DATA|PK_LOG_TUNNEL_DATA,
                     "%d: SSL_ERROR=%d: %p/%d/%d",
                     pkc->sockfd, err, data, wrote, length);
          }
        }
      }
      break;

    case CONN_SSL_HANDSHAKE:
      if (!(pkc->status & CONN_STATUS_BROKEN)) pkc_do_handshake(pkc);
      return 0;
#endif

    default:
      if (length)
        wrote = PKS_write(pkc->sockfd, data, length);
  }
  if (wrote > 0) {
    if (pk_state.log_mask & PK_LOG_TRACE) {
      pk_log_raw_data(PK_LOG_TRACE, "W", pkc->sockfd, data, wrote);
    }
    pkc->wrote_bytes += wrote;
  }
  return wrote;
}

void pkc_report_progress(struct pk_conn* pkc, char *sid, struct pk_conn* feconn)
{
  char buffer[256];
  int bytes;
  if (pkc->wrote_bytes >= CONN_REPORT_INCREMENT*1024) {
    pkc->reported_kb += (pkc->wrote_bytes/1024);
    pkc->wrote_bytes %= 1024;
    bytes = pk_format_skb(buffer, sid, pkc->reported_kb);
    pkc_write(feconn, buffer, bytes);
    pk_log(PK_LOG_BE_DATA|PK_LOG_TUNNEL_DATA,
           "%d: sid=%s, wrote_bytes=%d, reported_kb=%d",
           pkc->sockfd, sid, pkc->wrote_bytes, pkc->reported_kb);
  }
}

ssize_t pkc_flush(struct pk_conn* pkc, char *data, ssize_t length, int mode,
                  char* where)
{
  ssize_t flushed, wrote, bytes;
  flushed = wrote = errno = bytes = 0;
  int loops_left = 1000;

  if (pkc->sockfd < 0) {
    pk_log(PK_LOG_BE_DATA|PK_LOG_TUNNEL_DATA|PK_LOG_ERROR,
           "%d[%s]: Bogus flush?", pkc->sockfd, where);
    return -1;
  }

  if (mode == BLOCKING_FLUSH) {
    pk_log(PK_LOG_BE_DATA|PK_LOG_TUNNEL_DATA,
           "%d[%s]: Attempting blocking flush", pkc->sockfd, where);
    if (0 > set_blocking(pkc->sockfd))
      pk_log(PK_LOG_BE_DATA|PK_LOG_TUNNEL_DATA|PK_LOG_ERROR,
             "%d[%s]: Failed to set socket blocking", pkc->sockfd, where);
  }

  /* First, flush whatever was in the conn buffers */
  do {
    PK_TRACE_LOOP("flushing");
    wrote = pkc_raw_write(pkc, pkc->out_buffer, pkc->out_buffer_pos);
    if (wrote > 0) {
      if (pkc->out_buffer_pos > wrote) {
        memmove(pkc->out_buffer,
                pkc->out_buffer + wrote,
                pkc->out_buffer_pos - wrote);
      }
      pkc->out_buffer_pos -= wrote;
      flushed += wrote;
    }
    else if ((errno != EINTR) && (errno != 0))
      break;
  } while ((mode == BLOCKING_FLUSH) &&
           (pkc->out_buffer_pos > 0) &&
           (loops_left-- > 0));

  if (loops_left <= 0) {
    pk_log(PK_LOG_BE_DATA|PK_LOG_TUNNEL_DATA|PK_LOG_ERROR,
           "%d[%s]: BUG! Flush failed after 1000 iterations",
           pkc->sockfd, where);
    errno = EIO;
    return -1;
  }

  /* At this point we either have a non-EINTR error, or we've flushed
   * everything.  Return errors, else continue. */
  if (wrote < 0) {
    flushed = wrote;
    if ((errno != EAGAIN) && (errno != EWOULDBLOCK) && (errno != 0)) {
      pkc->status |= CONN_STATUS_CLS_WRITE;
      pk_log(PK_LOG_BE_DATA|PK_LOG_TUNNEL_DATA,
             "%d[%s]: errno=%d, closing", pkc->sockfd, where, errno);
    }
  }
  else if ((NULL != data) &&
           (mode == BLOCKING_FLUSH) &&
           (pkc->out_buffer_pos == 0)) {
    /* So far so good, everything has been flushed. Write the new data! */
    flushed = wrote = 0;
    while (length > wrote) {
      PK_TRACE_LOOP("writing");
      bytes = pkc_raw_write(pkc, data+wrote, length-wrote);
      if (bytes > 0) {
        wrote += bytes;
        flushed += bytes;
      }
      else if ((errno != EINTR) && (errno != 0)) {
        break;
      }
      else if (loops_left-- <= 0) {
        pk_log(PK_LOG_BE_DATA|PK_LOG_TUNNEL_DATA|PK_LOG_ERROR,
               "%d[%s]: BUG! Flush failed after 1000 iterations",
               pkc->sockfd, where);
        errno = EIO;
        break;
      }
    }
    /* At this point, if we have a non-EINTR error, bytes is < 0 and we
     * want to return that.  Otherwise, return how much got written. */
    if (bytes < 0) {
      flushed = bytes;
      if ((errno != EAGAIN) && (errno != EWOULDBLOCK) && (errno != 0))
        pkc->status |= CONN_STATUS_CLS_WRITE;
    }
  }

  if (mode == BLOCKING_FLUSH) {
    set_non_blocking(pkc->sockfd);
    pk_log(PK_LOG_BE_DATA|PK_LOG_TUNNEL_DATA,
           "%d[%s]: Blocking flush complete.", pkc->sockfd, where);
  }
  return flushed;
}

ssize_t pkc_write(struct pk_conn* pkc, char* data, ssize_t length)
{
  ssize_t wleft;
  ssize_t wrote = 0;

  /* 1. Try to flush already buffered data. */
  if (pkc->out_buffer_pos)
    pkc_flush(pkc, NULL, 0, NON_BLOCKING_FLUSH, "pkc_write/1");

  /* 2. If successful, try to write new data (0 copies!) */
  if (0 == pkc->out_buffer_pos) {
    errno = 0;
    do {
      PK_TRACE_LOOP("writing");
      wrote = pkc_raw_write(pkc, data, length);
    } while ((wrote < 0) && ((errno == EINTR) || (errno == 0)));
  }

  if (wrote < length) {
    if (wrote < 0) /* Ignore errors, for now */
      wrote = 0;

    wleft = length-wrote;
    if (wleft <= PKC_OUT_FREE(*pkc)) {
      /* 2a. Have data left, there is space in our buffer: buffer it! */
      memcpy(PKC_OUT(*pkc), data+wrote, wleft);
      pkc->out_buffer_pos += wleft;
    }
    else {
      /* 2b. If new+old data > buffer size, do a blocking write. */
      if (0 > pkc_flush(pkc, data+wrote, length-wrote, BLOCKING_FLUSH,
                        "pkc_write/2")) {
        /* Give up and return an error. We are broken. */
        return -1;
      }
    }
  }

  return length;
}
