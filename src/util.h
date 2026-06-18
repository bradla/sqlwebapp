/* util.h - string builder, HTML escaping, small helpers (C99) */
#ifndef CSQLPAGE_UTIL_H
#define CSQLPAGE_UTIL_H

#include <stddef.h>

/* A growable byte buffer used for building output and reading input. */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} sb_t;

void  sb_init(sb_t *b);
void  sb_free(sb_t *b);
void  sb_putc(sb_t *b, char c);
void  sb_write(sb_t *b, const char *s, size_t n);
void  sb_puts(sb_t *b, const char *s);            /* NUL-terminated */
void  sb_printf(sb_t *b, const char *fmt, ...);

/* Append `s` with HTML special characters (& < > " ') escaped. */
void  sb_put_html(sb_t *b, const char *s);

/* xmalloc family: abort on OOM (CGI process is short-lived). */
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

#endif
