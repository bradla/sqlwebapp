/* sqlfn.c - implementation (C99 + libxcrypt for bcrypt) */
#define _DEFAULT_SOURCE 1
#include "sqlfn.h"
#include "json.h"
#include "jwt.h"

#include <crypt.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TOKEN_TTL   (30L * 24 * 3600)
#define BCRYPT_COST 10
#define MAX_ARGS    4

static int is_ident_start(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static int is_ident(int c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

int sqlfn_is_call(const char *s) {
    const char *p;
    if (strncmp(s, "sqlpage.", 8) != 0 || !is_ident_start((unsigned char)s[8])) return 0;
    for (p = s + 8; is_ident((unsigned char)*p); p++) {}
    while (*p == ' ' || *p == '\t') p++;
    return *p == '(';
}

size_t sqlfn_call_len(const char *s) {
    const char *p = strchr(s, '(');
    if (!p) return 0;
    for (p++; *p; p++) {
        if (*p == '\'') {                       /* skip 'literal' with '' escapes */
            for (p++; *p; p++)
                if (*p == '\'') { if (p[1] == '\'') p++; else break; }
            if (!*p) return 0;
        } else if (*p == ')') {
            return (size_t)(p - s) + 1;
        }
    }
    return 0;
}

static char *dup_or_null(const char *s) { return s ? xstrdup(s) : NULL; }

/* ---- argument parsing --------------------------------------------------- */

typedef struct {
    char  name[64];
    char *args[MAX_ARGS];     /* owned values; NULL = SQL NULL */
    int   nargs;
} call_t;

static void call_free(call_t *c) {
    int i;
    for (i = 0; i < c->nargs; i++) free(c->args[i]);
}

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static int parse_call(const char *s, const cgi_request_t *req, call_t *c) {
    const char *p = s + 8, *start = p;
    c->nargs = 0;
    while (is_ident((unsigned char)*p)) p++;
    if ((size_t)(p - start) >= sizeof c->name) return -1;
    memcpy(c->name, start, (size_t)(p - start));
    c->name[p - start] = '\0';
    p = skip_ws(p);
    if (*p++ != '(') return -1;
    p = skip_ws(p);
    if (*p == ')') return 0;
    for (;;) {
        char *val = NULL;
        if (c->nargs == MAX_ARGS) return -1;
        if (*p == '$' || *p == ':') {                     /* request parameter */
            char name[128];
            size_t n;
            start = ++p;
            while (is_ident((unsigned char)*p)) p++;
            n = (size_t)(p - start);
            if (n == 0 || n >= sizeof name) return -1;
            memcpy(name, start, n);
            name[n] = '\0';
            val = dup_or_null(map_get(&req->params, name));
        } else if (*p == '\'') {                          /* 'literal' */
            sb_t lit;
            sb_init(&lit);
            for (p++; *p; p++) {
                if (*p == '\'') { if (p[1] == '\'') p++; else break; }
                sb_putc(&lit, *p);
            }
            if (*p != '\'') { sb_free(&lit); return -1; }
            p++;
            val = lit.data;
        } else if (strncmp(p, "NULL", 4) == 0 || strncmp(p, "null", 4) == 0) {
            p += 4;
        } else {
            return -1;
        }
        c->args[c->nargs++] = val;
        p = skip_ws(p);
        if (*p == ')') return 0;
        if (*p++ != ',') return -1;
        p = skip_ws(p);
    }
}

/* ---- authentication ----------------------------------------------------- */

/* The token from an "Authorization: Token <jwt>" header, or NULL. */
static const char *presented_token(const cgi_request_t *req) {
    const char *h = req->authorization;
    if (!h || strncmp(h, "Token ", 6) != 0) return NULL;
    return h + 6;
}

/* Verify the header once per request. Returns 0 if there is no header or it
   holds a valid token (req->user_id set), -1 if it is present but invalid. */
static int check_auth(cgi_request_t *req) {
    const char *tok;
    if (req->auth_checked) return req->authorization && !req->user_id ? -1 : 0;
    req->auth_checked = 1;
    if (!req->authorization) return 0;
    tok = presented_token(req);
    if (!tok || !req->jwt_secret) return -1;
    if (jwt_verify(req->jwt_secret, tok, (long)time(NULL), &req->user_id) != 0) return -1;
    {   /* subject must be a decimal id */
        const char *d = req->user_id;
        if (!*d || strlen(d) > 18) goto bad;
        for (; *d; d++) if (*d < '0' || *d > '9') goto bad;
    }
    return 0;
bad:
    free(req->user_id);
    req->user_id = NULL;
    return -1;
}

/* ---- bcrypt (libxcrypt) ------------------------------------------------- */

static char *bcrypt_hash(const char *pw) {
    char               salt[CRYPT_GENSALT_OUTPUT_SIZE];
    struct crypt_data *cd;
    char              *out = NULL;
    if (!crypt_gensalt_rn("$2b$", BCRYPT_COST, NULL, 0, salt, sizeof salt)) return NULL;
    cd = calloc(1, sizeof *cd);
    if (!cd) return NULL;
    if (crypt_rn(pw, salt, cd, sizeof *cd) && cd->output[0] != '*') out = xstrdup(cd->output);
    free(cd);
    return out;
}

static int bcrypt_verify(const char *pw, const char *hash) {
    struct crypt_data *cd = calloc(1, sizeof *cd);
    int                ok = 0;
    if (!cd) return 0;
    if (crypt_rn(pw, hash, cd, sizeof *cd) && cd->output[0] != '*') {
        size_t        n = strlen(hash), i;
        unsigned char diff = 0;
        if (strlen(cd->output) == n) {
            for (i = 0; i < n; i++) diff |= (unsigned char)(cd->output[i] ^ hash[i]);
            ok = diff == 0;
        }
    }
    free(cd);
    return ok;
}

/* ---- dispatch ----------------------------------------------------------- */

static int fail(char **errmsg, int status, const char *msg, const char *name) {
    sb_t b;
    sb_init(&b);
    sb_printf(&b, msg, name);
    *errmsg = b.data;
    return status;
}

int sqlfn_eval(const char *call, cgi_request_t *req, char **out, char **errmsg) {
    call_t      c;
    const char *n;
    int         rc = 0;

    *out = NULL;
    *errmsg = NULL;
    if (parse_call(call, req, &c) != 0) {
        call_free(&c);
        return fail(errmsg, 500, "malformed function call: %s", call);
    }
    n = c.name;

#define ARGS(k) if (c.nargs != (k)) { rc = fail(errmsg, 500, "wrong number of arguments to sqlpage.%s", n); goto done; }

    if (strcmp(n, "request_body") == 0) {
        ARGS(0);
        if (req->body.len) *out = xstrdup(req->body.data);
    } else if (strcmp(n, "request_json") == 0) {
        ARGS(0);
        if (req->body.len && strlen(req->body.data) == req->body.len &&
            json_valid(req->body.data, req->body.len))
            *out = xstrdup(req->body.data);
    } else if (strcmp(n, "user_id") == 0 || strcmp(n, "require_user_id") == 0) {
        ARGS(0);
        if (check_auth(req) != 0)
            rc = fail(errmsg, 401, "invalid token%s", "");
        else if (!req->user_id && n[0] == 'r')
            rc = fail(errmsg, 401, "authentication required%s", "");
        else
            *out = dup_or_null(req->user_id);
    } else if (strcmp(n, "token") == 0) {
        ARGS(0);
        *out = dup_or_null(presented_token(req));
    } else if (strcmp(n, "jwt") == 0) {
        ARGS(1);
        if (!req->jwt_secret) { rc = fail(errmsg, 500, "SQLPAGE_JWT_SECRET is not set%s", ""); goto done; }
        if (c.args[0]) *out = jwt_sign(req->jwt_secret, c.args[0], (long)time(NULL), TOKEN_TTL);
    } else if (strcmp(n, "hash_password") == 0) {
        ARGS(1);
        if (c.args[0] && !(*out = bcrypt_hash(c.args[0])))
            rc = fail(errmsg, 500, "bcrypt failed%s", "");
    } else if (strcmp(n, "verify_password") == 0) {
        ARGS(2);
        *out = xstrdup(c.args[0] && c.args[1] && bcrypt_verify(c.args[0], c.args[1])
                       ? "true" : "false");
    } else {
        rc = fail(errmsg, 500, "unknown function sqlpage.%s", n);
    }
#undef ARGS

done:
    call_free(&c);
    return rc;
}
