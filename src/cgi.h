/* cgi.h - CGI request parsing (C99) */
#ifndef CSQLPAGE_CGI_H
#define CSQLPAGE_CGI_H

#include "io.h"
#include "map.h"
#include "util.h"

typedef struct {
    const char *method;      /* REQUEST_METHOD, e.g. "GET" (never NULL)   */
    const char *path_info;   /* PATH_INFO, e.g. "/users.sql" (never NULL) */
    map_t       params;      /* merged query-string + form-body params    */
    map_t       cookies;     /* parsed from HTTP_COOKIE                    */
    sb_t        body;        /* raw request body bytes                    */
} cgi_request_t;

/* Populate `r` from the request transport `io`. Returns 0 on success.
   `r` borrows env strings from `io`, which must outlive `r`. */
int  cgi_read(cgi_request_t *r, io_t *io);
void cgi_free(cgi_request_t *r);

/* URL-decode `src` (application/x-www-form-urlencoded) into a fresh string.
   Caller frees. */
char *url_decode(const char *src);

#endif
