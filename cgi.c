/* cgi.c - read a CGI request from the environment and stdin (C99) */
#include "cgi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *getenv_or(const char *name, const char *dflt) {
    const char *v = getenv(name);
    return v ? v : dflt;
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

char *url_decode(const char *src) {
    size_t n   = strlen(src);
    char  *out = xmalloc(n + 1);
    size_t o   = 0, i;
    for (i = 0; i < n; i++) {
        if (src[i] == '+') {
            out[o++] = ' ';
        } else if (src[i] == '%' && i + 2 < n + 1 && src[i + 1] && src[i + 2]) {
            int hi = hexval((unsigned char)src[i + 1]);
            int lo = hexval((unsigned char)src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out[o++] = (char)((hi << 4) | lo);
                i += 2;
            } else {
                out[o++] = src[i];
            }
        } else {
            out[o++] = src[i];
        }
    }
    out[o] = '\0';
    return out;
}

/* Parse "a=1&b=2&c" into map, URL-decoding keys and values. */
static void parse_urlencoded(map_t *m, const char *s) {
    if (!s || !*s) return;
    while (*s) {
        const char *amp = strchr(s, '&');
        size_t      pairlen = amp ? (size_t)(amp - s) : strlen(s);
        const char *eq = memchr(s, '=', pairlen);
        char       *key, *val;

        if (eq) {
            char *kraw = xmalloc((size_t)(eq - s) + 1);
            char *vraw = xmalloc(pairlen - (size_t)(eq - s));
            memcpy(kraw, s, (size_t)(eq - s));
            kraw[eq - s] = '\0';
            memcpy(vraw, eq + 1, pairlen - (size_t)(eq - s) - 1);
            vraw[pairlen - (size_t)(eq - s) - 1] = '\0';
            key = url_decode(kraw);
            val = url_decode(vraw);
            free(kraw);
            free(vraw);
        } else {
            char *kraw = xmalloc(pairlen + 1);
            memcpy(kraw, s, pairlen);
            kraw[pairlen] = '\0';
            key = url_decode(kraw);
            val = xstrdup("");
            free(kraw);
        }
        if (*key) map_add(m, key, val);
        free(key);
        free(val);

        if (!amp) break;
        s = amp + 1;
    }
}

/* Parse "name=value; other=thing" cookie header. */
static void parse_cookies(map_t *m, const char *s) {
    if (!s) return;
    while (*s) {
        const char *sep = strchr(s, ';');
        size_t      seglen = sep ? (size_t)(sep - s) : strlen(s);
        const char *eq = memchr(s, '=', seglen);
        if (eq) {
            const char *kstart = s;
            while (kstart < eq && (*kstart == ' ' || *kstart == '\t')) kstart++;
            {
                size_t klen = (size_t)(eq - kstart);
                size_t vlen = seglen - (size_t)(eq - s) - 1;
                char  *k = xmalloc(klen + 1);
                char  *v = xmalloc(vlen + 1);
                memcpy(k, kstart, klen); k[klen] = '\0';
                memcpy(v, eq + 1, vlen); v[vlen] = '\0';
                if (*k) map_add(m, k, v);
                free(k);
                free(v);
            }
        }
        if (!sep) break;
        s = sep + 1;
    }
}

int cgi_read(cgi_request_t *r) {
    const char *clen_s;

    r->method    = getenv_or("REQUEST_METHOD", "GET");
    r->path_info = getenv_or("PATH_INFO", "");
    map_init(&r->params);
    map_init(&r->cookies);
    sb_init(&r->body);

    parse_urlencoded(&r->params, getenv("QUERY_STRING"));
    parse_cookies(&r->cookies, getenv("HTTP_COOKIE"));

    /* Read request body (POST/PUT) up to CONTENT_LENGTH. */
    clen_s = getenv("CONTENT_LENGTH");
    if (clen_s) {
        long  clen = strtol(clen_s, NULL, 10);
        long  got  = 0;
        int   ch;
        while (got < clen && (ch = getchar()) != EOF) {
            sb_putc(&r->body, (char)ch);
            got++;
        }
        /* If form-encoded, fold body params into the param map too. */
        {
            const char *ct = getenv("CONTENT_TYPE");
            if (ct && strstr(ct, "application/x-www-form-urlencoded"))
                parse_urlencoded(&r->params, r->body.data);
        }
    }
    return 0;
}

void cgi_free(cgi_request_t *r) {
    map_free(&r->params);
    map_free(&r->cookies);
    sb_free(&r->body);
}
