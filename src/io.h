/* io.h - transport abstraction so the request core works under both plain
 * CGI and FastCGI (C99).
 *
 * An io_t is the "subclass" base: a concrete transport embeds io_t as its
 * first member and stores its own state after it, so callbacks can recover
 * the full object by casting the io_t* back. The core only ever sees io_t.
 */
#ifndef CSQLPAGE_IO_H
#define CSQLPAGE_IO_H

#include <stddef.h>

typedef struct io io_t;

struct io {
    /* Look up a CGI variable (PATH_INFO, QUERY_STRING, ...). NULL if unset. */
    const char *(*env)(io_t *self, const char *name);
    /* Read up to n bytes of the request body; returns count, 0 at EOF. */
    size_t      (*read)(io_t *self, char *buf, size_t n);
    /* Write n bytes of the response (status line + headers + body). */
    void        (*write)(io_t *self, const char *buf, size_t n);
};

#endif
