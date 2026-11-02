/* json.c - recursive-descent JSON scanner (C99) */
#include "json.h"

#include <string.h>

#define MAX_DEPTH 64

typedef struct {
    const char *p, *end;
    int         depth;
} scan_t;

static void skip_ws(scan_t *s) {
    while (s->p < s->end &&
           (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r'))
        s->p++;
}

static int is_hex(int c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* At an opening quote: consume the string, setting [*start, *stop) to its
   escaped contents. */
static int scan_string(scan_t *s, const char **start, const char **stop) {
    if (s->p >= s->end || *s->p != '"') return 0;
    s->p++;
    if (start) *start = s->p;
    while (s->p < s->end) {
        unsigned char c = (unsigned char)*s->p;
        if (c == '"') {
            if (stop) *stop = s->p;
            s->p++;
            return 1;
        }
        if (c < 0x20) return 0;
        if (c == '\\') {
            s->p++;
            if (s->p >= s->end) return 0;
            if (*s->p == 'u') {
                int i;
                for (i = 1; i <= 4; i++)
                    if (s->p + i >= s->end || !is_hex((unsigned char)s->p[i])) return 0;
                s->p += 4;
            } else if (!strchr("\"\\/bfnrt", *s->p)) {
                return 0;
            }
        }
        s->p++;
    }
    return 0;
}

static int scan_digits(scan_t *s) {
    const char *from = s->p;
    while (s->p < s->end && *s->p >= '0' && *s->p <= '9') s->p++;
    return s->p > from;
}

static int scan_number(scan_t *s) {
    if (s->p < s->end && *s->p == '-') s->p++;
    if (s->p < s->end && *s->p == '0') s->p++;
    else if (!scan_digits(s)) return 0;
    if (s->p < s->end && *s->p == '.') { s->p++; if (!scan_digits(s)) return 0; }
    if (s->p < s->end && (*s->p == 'e' || *s->p == 'E')) {
        s->p++;
        if (s->p < s->end && (*s->p == '+' || *s->p == '-')) s->p++;
        if (!scan_digits(s)) return 0;
    }
    return 1;
}

static int scan_literal(scan_t *s, const char *word) {
    size_t n = strlen(word);
    if ((size_t)(s->end - s->p) < n || memcmp(s->p, word, n) != 0) return 0;
    s->p += n;
    return 1;
}

/* Consume one value; returns its type letter (see json_member) or 0. */
static int scan_value(scan_t *s) {
    skip_ws(s);
    if (s->p >= s->end) return 0;
    switch (*s->p) {
    case '"':
        return scan_string(s, NULL, NULL) ? 's' : 0;
    case 't': return scan_literal(s, "true")  ? 't' : 0;
    case 'f': return scan_literal(s, "false") ? 'f' : 0;
    case 'n': return scan_literal(s, "null")  ? 'z' : 0;
    case '{': case '[': {
        char close = *s->p == '{' ? '}' : ']';
        int  obj   = close == '}';
        if (++s->depth > MAX_DEPTH) return 0;
        s->p++;
        skip_ws(s);
        if (s->p < s->end && *s->p == close) { s->p++; s->depth--; return obj ? 'o' : 'a'; }
        for (;;) {
            if (obj) {
                skip_ws(s);
                if (!scan_string(s, NULL, NULL)) return 0;
                skip_ws(s);
                if (s->p >= s->end || *s->p != ':') return 0;
                s->p++;
            }
            if (!scan_value(s)) return 0;
            skip_ws(s);
            if (s->p >= s->end) return 0;
            if (*s->p == ',') { s->p++; continue; }
            if (*s->p == close) { s->p++; s->depth--; return obj ? 'o' : 'a'; }
            return 0;
        }
    }
    default:
        return scan_number(s) ? 'n' : 0;
    }
}

int json_valid(const char *s, size_t n) {
    scan_t sc;
    sc.p = s; sc.end = s + n; sc.depth = 0;
    if (!scan_value(&sc)) return 0;
    skip_ws(&sc);
    return sc.p == sc.end;
}

int json_member(const char *s, size_t n, const char *key,
                const char **val, size_t *len) {
    scan_t sc;
    size_t klen = strlen(key);
    int    found = 0;

    sc.p = s; sc.end = s + n; sc.depth = 1;
    skip_ws(&sc);
    if (sc.p >= sc.end || *sc.p != '{') return 0;
    sc.p++;
    skip_ws(&sc);
    if (sc.p < sc.end && *sc.p == '}') return 0;
    for (;;) {
        const char *ks, *ke, *vs;
        int         type;
        skip_ws(&sc);
        if (!scan_string(&sc, &ks, &ke)) return 0;
        skip_ws(&sc);
        if (sc.p >= sc.end || *sc.p != ':') return 0;
        sc.p++;
        skip_ws(&sc);
        vs = sc.p;
        if (!(type = scan_value(&sc))) return 0;
        if ((size_t)(ke - ks) == klen && memcmp(ks, key, klen) == 0) {
            found = type;
            if (type == 's') { *val = vs + 1; *len = (size_t)(sc.p - vs) - 2; }
            else             { *val = vs;     *len = (size_t)(sc.p - vs); }
        }
        skip_ws(&sc);
        if (sc.p >= sc.end) return 0;
        if (*sc.p == ',') { sc.p++; continue; }
        if (*sc.p == '}') return found;
        return 0;
    }
}
