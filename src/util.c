/* util.c - implementation (C99) */
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fputs("out of memory\n", stderr); exit(1); }
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) { fputs("out of memory\n", stderr); exit(1); }
    return q;
}

char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char  *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

void sb_init(sb_t *b) {
    b->cap  = 256;
    b->len  = 0;
    b->data = xmalloc(b->cap);
    b->data[0] = '\0';
}

void sb_free(sb_t *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

void sb_clear(sb_t *b) {
    b->len = 0;
    b->data[0] = '\0';
}

static void sb_reserve(sb_t *b, size_t extra) {
    if (b->len + extra + 1 > b->cap) {
        while (b->len + extra + 1 > b->cap) b->cap *= 2;
        b->data = xrealloc(b->data, b->cap);
    }
}

void sb_putc(sb_t *b, char c) {
    sb_reserve(b, 1);
    b->data[b->len++] = c;
    b->data[b->len]   = '\0';
}

void sb_write(sb_t *b, const char *s, size_t n) {
    sb_reserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void sb_puts(sb_t *b, const char *s) {
    if (s) sb_write(b, s, strlen(s));
}

void sb_printf(sb_t *b, const char *fmt, ...) {
    va_list ap;
    int     n;
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    sb_reserve(b, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

void sb_put_html(sb_t *b, const char *s) {
    if (!s) return;
    for (; *s; s++) {
        switch (*s) {
            case '&':  sb_puts(b, "&amp;");  break;
            case '<':  sb_puts(b, "&lt;");   break;
            case '>':  sb_puts(b, "&gt;");   break;
            case '"':  sb_puts(b, "&quot;"); break;
            case '\'': sb_puts(b, "&#39;");  break;
            default:   sb_putc(b, *s);       break;
        }
    }
}
