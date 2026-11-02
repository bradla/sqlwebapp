/* http.h - built-in HTTP/1.1 server (Linux: epoll + POSIX threads).
 *
 * An alternative to CGI/FastCGI for serving csqlpage directly or behind a
 * reverse proxy. Worker threads share one epoll instance with one-shot
 * registrations: an idle worker takes the next ready connection, handles its
 * complete requests, and hands it back. A worker blocks while it handles a
 * request, so it is also the unit of database concurrency (one PostgreSQL
 * connection each). Keep-alive and pipelining are supported; idle
 * connections are never closed by the server.
 *
 *   SQLPAGE_HTTP_LISTEN   "host:port" to listen on (enables this mode)
 *   SQLPAGE_THREADS       worker threads (default: 4 per CPU)
 */
#ifndef CSQLPAGE_HTTP_H
#define CSQLPAGE_HTTP_H

#include "io.h"

/* True when SQLPAGE_HTTP_LISTEN is set. */
int http_should_serve(void);

/* Serve forever, handing each request to `handle` as an io_t whose env()
   provides the CGI variables. Returns non-zero on a fatal startup error. */
int http_serve(void (*handle)(io_t *io));

#endif
