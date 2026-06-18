/* fastcgi.h - minimal FastCGI responder (C99 + POSIX sockets).
 *
 * A dependency-free implementation of the FastCGI protocol sufficient to act
 * as a responder behind Apache (mod_proxy_fcgi / mod_fcgid), nginx, lighttpd,
 * etc. The rest of csqlpage stays plain ANSI/C99; only this file uses sockets.
 */
#ifndef CSQLPAGE_FASTCGI_H
#define CSQLPAGE_FASTCGI_H

#include "io.h"

/* True when the process should run as a FastCGI app: either stdin (fd 0) is a
   listening socket (launched by the web server / spawn-fcgi), or the env var
   SQLPAGE_FCGI_BIND requests an explicit address to listen on. */
int fcgi_should_serve(void);

/* Run the FastCGI accept loop, handing each request to `handle` as an io_t.
   Does not return under normal operation; returns non-zero on fatal error. */
int fcgi_serve(void (*handle)(io_t *io));

#endif
