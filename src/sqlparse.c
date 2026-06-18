/* sqlparse.c - statement splitter + named->positional parameter rewriter.
 *
 * Walks the script as a small state machine so that semicolons, $tags and
 * colons inside string literals, quoted identifiers, line/block comments and
 * dollar-quoted bodies are left untouched. Only in "normal" text do we split
 * on ';' and rewrite $name / :name into $1..$N.
 */
#include "sqlparse.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

static int is_ident_start(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static int is_ident(int c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

/* in-progress statement being assembled */
typedef struct {
    sb_t    sql;
    char  **names;
    int     nparams;
    int     cap;
} build_t;

static void build_reset(build_t *b) {
    sb_init(&b->sql);
    b->names   = NULL;
    b->nparams = 0;
    b->cap     = 0;
}

/* Return the 1-based positional index for `name` (length `len`),
   adding it to the list on first use so repeats reuse the same $N. */
static int param_index(build_t *b, const char *name, size_t len) {
    int i;
    for (i = 0; i < b->nparams; i++) {
        if (strlen(b->names[i]) == len && memcmp(b->names[i], name, len) == 0)
            return i + 1;
    }
    if (b->nparams == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 4;
        b->names = xrealloc(b->names, (size_t)b->cap * sizeof(char *));
    }
    b->names[b->nparams] = xmalloc(len + 1);
    memcpy(b->names[b->nparams], name, len);
    b->names[b->nparams][len] = '\0';
    return ++b->nparams;
}

static int only_whitespace(const char *s) {
    for (; *s; s++)
        if (*s != ' ' && *s != '\t' && *s != '\n' && *s != '\r')
            return 0;
    return 1;
}

/* Finalize the current statement into the output array (skipping blanks). */
static void flush(build_t *b, pg_stmt_t **out, size_t *n, size_t *cap) {
    if (only_whitespace(b->sql.data)) {
        sb_free(&b->sql);
        free(b->names);
        build_reset(b);
        return;
    }
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *out = xrealloc(*out, *cap * sizeof(pg_stmt_t));
    }
    (*out)[*n].sql     = xstrdup(b->sql.data);
    (*out)[*n].names   = b->names;
    (*out)[*n].nparams = b->nparams;
    (*n)++;
    sb_free(&b->sql);
    build_reset(b);
}

pg_stmt_t *sqlparse_split(const char *s, size_t *count) {
    pg_stmt_t *out = NULL;
    size_t     n = 0, cap = 0;
    build_t    b;
    build_reset(&b);

    while (*s) {
        char c = *s;

        if (c == '-' && s[1] == '-') {                 /* line comment */
            sb_putc(&b.sql, *s++); sb_putc(&b.sql, *s++);
            while (*s && *s != '\n') sb_putc(&b.sql, *s++);
            continue;
        }
        if (c == '/' && s[1] == '*') {                 /* block comment (nested) */
            int depth = 1;
            sb_putc(&b.sql, *s++); sb_putc(&b.sql, *s++);
            while (*s && depth > 0) {
                if (*s == '/' && s[1] == '*') { depth++; sb_putc(&b.sql, *s++); sb_putc(&b.sql, *s++); }
                else if (*s == '*' && s[1] == '/') { depth--; sb_putc(&b.sql, *s++); sb_putc(&b.sql, *s++); }
                else sb_putc(&b.sql, *s++);
            }
            continue;
        }
        if (c == '\'') {                               /* string literal */
            sb_putc(&b.sql, *s++);
            while (*s) {
                if (*s == '\'' && s[1] == '\'') { sb_putc(&b.sql, *s++); sb_putc(&b.sql, *s++); continue; }
                sb_putc(&b.sql, *s);
                if (*s == '\'') { s++; break; }
                s++;
            }
            continue;
        }
        if (c == '"') {                                /* quoted identifier */
            sb_putc(&b.sql, *s++);
            while (*s) {
                if (*s == '"' && s[1] == '"') { sb_putc(&b.sql, *s++); sb_putc(&b.sql, *s++); continue; }
                sb_putc(&b.sql, *s);
                if (*s == '"') { s++; break; }
                s++;
            }
            continue;
        }
        if (c == '$') {
            const char *p = s + 1;
            size_t      taglen;
            while (is_ident(*p)) p++;
            taglen = (size_t)(p - (s + 1));
            if (*p == '$') {                           /* dollar-quoted string */
                size_t      dellen = (size_t)(p - s) + 1;  /* "$tag$" */
                const char *body   = s + dellen;
                const char *q      = body;
                sb_write(&b.sql, s, dellen);
                while (*q) {
                    if (*q == '$' && strncmp(q, s, dellen) == 0) break;
                    q++;
                }
                if (*q) {
                    sb_write(&b.sql, body, (size_t)(q - body));
                    sb_write(&b.sql, s, dellen);       /* closing == opening text */
                    s = q + dellen;
                } else {
                    sb_puts(&b.sql, body);             /* unterminated: copy rest */
                    s = body + strlen(body);
                }
                continue;
            }
            if (taglen > 0 && is_ident_start((unsigned char)s[1])) {  /* $name */
                int idx = param_index(&b, s + 1, taglen);
                sb_printf(&b.sql, "$%d", idx);
                s = p;
                continue;
            }
            sb_write(&b.sql, s, (size_t)(p - s));      /* $1 positional or stray $ */
            s = p;
            continue;
        }
        if (c == ':') {
            if (s[1] == ':') { sb_putc(&b.sql, *s++); sb_putc(&b.sql, *s++); continue; }  /* ::cast */
            if (is_ident_start((unsigned char)s[1])) {                                    /* :name */
                const char *p = s + 1;
                int         idx;
                while (is_ident(*p)) p++;
                idx = param_index(&b, s + 1, (size_t)(p - (s + 1)));
                sb_printf(&b.sql, "$%d", idx);
                s = p;
                continue;
            }
            sb_putc(&b.sql, *s++);
            continue;
        }
        if (c == ';') { flush(&b, &out, &n, &cap); s++; continue; }

        sb_putc(&b.sql, *s++);
    }
    flush(&b, &out, &n, &cap);

    *count = n;
    return out;
}

void sqlparse_free(pg_stmt_t *stmts, size_t count) {
    size_t i;
    int    j;
    for (i = 0; i < count; i++) {
        free(stmts[i].sql);
        for (j = 0; j < stmts[i].nparams; j++) free(stmts[i].names[j]);
        free(stmts[i].names);
    }
    free(stmts);
}
