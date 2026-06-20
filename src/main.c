#define _POSIX_C_SOURCE 200809L  /* clock_gettime / CLOCK_MONOTONIC */
#define _DEFAULT_SOURCE 1        /* getrusage */
/* main.c - csqlpage entry point: plain CGI or FastCGI (C99)
 *
 * Environment (CGI vars + these):
 *   SQLPAGE_WEB_ROOT       directory holding .sql files     (default ".")
 *   SQLPAGE_DATABASE_URL   sqlite://... | postgres://...    (default sqlite)
 *   SQLPAGE_FCGI_BIND      optional "ip:port" or "/sock" to run as FastCGI
 *
 * Transport is chosen automatically: if launched as a FastCGI app (stdin is a
 * listening socket) or SQLPAGE_FCGI_BIND is set, it serves a FastCGI loop;
 * otherwise it handles a single CGI request and exits.
 *
 * Routing: PATH_INFO -> <web_root>/<path>; "" or "/" maps to "index.sql".
 */
#include "cgi.h"
#include "db.h"
#include "fastcgi.h"
#include "io.h"
#include "render.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

static const char *io_env_or(io_t *io, const char *name, const char *dflt) {
    const char *v = io->env(io, name);
    return v ? v : dflt;
}

/* Debug mode reports per-request timing and memory; enabled by SQLPAGE_DEBUG. */
static int debug_enabled(io_t *io) {
    const char *v = io->env(io, "SQLPAGE_DEBUG");
    return v && *v && strcmp(v, "0") != 0;
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Peak resident set size in KB (Linux units), or -1 if unavailable. */
static long peak_rss_kb(void) {
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) return ru.ru_maxrss;
    return -1;
}

/* Reject paths that could escape the web root. */
static int path_is_safe(const char *p) {
    if (strstr(p, "..")) return 0;
    for (; *p; p++) if (*p == '\\') return 0;
    return 1;
}

/* Read an entire file into a NUL-terminated buffer (caller frees). */
static char *read_file(const char *path) {
    FILE  *f = fopen(path, "rb");
    sb_t   sb;
    int    ch;
    if (!f) return NULL;
    sb_init(&sb);
    while ((ch = fgetc(f)) != EOF) sb_putc(&sb, (char)ch);
    fclose(f);
    return sb.data;   /* ownership transferred; NUL-terminated by sb */
}

/* Handle one request on the given transport. */
static void handle_request(io_t *io) {
    cgi_request_t req;
    render_t     *r;
    const char   *web_root, *rel, *dburl;
    char         *fullpath, *sql, *errmsg = NULL;
    double        t_start = now_sec();
    double        db_ms   = 0;
    int           dbg;

    cgi_read(&req, io);
    dbg = debug_enabled(io);
    r = render_new(io_env_or(io, "SQLPAGE_ASSETS_BASE", "/assets"));

    /* Route on PATH_INFO; fall back to SCRIPT_NAME for transports that put the
       request path there instead (notably Apache mod_proxy_fcgi). Then strip
       an optional mount prefix so the app can live under a sub-path. */
    rel = req.path_info;
    if (*rel == '\0') rel = io_env_or(io, "SCRIPT_NAME", "");
    {
        const char *prefix = io_env_or(io, "SQLPAGE_SITE_PREFIX", "");
        size_t      plen   = strlen(prefix);
        if (plen && strncmp(rel, prefix, plen) == 0 &&
            (rel[plen] == '/' || rel[plen] == '\0'))
            rel += plen;
    }
    while (*rel == '/') rel++;              /* drop leading slashes */
    if (*rel == '\0') rel = "index.sql";

    if (!path_is_safe(rel)) {
        if (dbg) render_set_debug(r, (now_sec() - t_start) * 1000, db_ms, peak_rss_kb());
        render_error(r, io, "Forbidden: invalid path");
        cgi_free(&req);
        return;
    }

    web_root = io_env_or(io, "SQLPAGE_WEB_ROOT", ".");
    {
        size_t n = strlen(web_root) + 1 + strlen(rel) + 1;
        fullpath = xmalloc(n);
        snprintf(fullpath, n, "%s/%s", web_root, rel);
    }

    sql = read_file(fullpath);
    if (!sql) {
        sb_t msg; sb_init(&msg);
        sb_printf(&msg, "Not found: %s", fullpath);
        if (dbg) render_set_debug(r, (now_sec() - t_start) * 1000, db_ms, peak_rss_kb());
        render_error(r, io, msg.data);
        sb_free(&msg);
        free(fullpath);
        cgi_free(&req);
        return;
    }

    dburl = io_env_or(io, "SQLPAGE_DATABASE_URL", "sqlite://./sqlpage.db");
    {
        double db_start = now_sec();
        int    rc = db_run_script(dburl, sql, &req, render_row, r, &errmsg);
        db_ms = (now_sec() - db_start) * 1000;
        if (dbg) render_set_debug(r, (now_sec() - t_start) * 1000, db_ms, peak_rss_kb());
        if (rc != 0) {
            sb_t msg; sb_init(&msg);
            sb_printf(&msg, "Database error: %s", errmsg ? errmsg : "(unknown)");
            render_error(r, io, msg.data);
            sb_free(&msg);
            free(errmsg);
        } else {
            render_finish(r, io);
        }
    }

    free(sql);
    free(fullpath);
    cgi_free(&req);
}

/* ---- plain-CGI transport (env + stdin/stdout) -------------------------- */

static const char *cgi_io_env(io_t *self, const char *name) {
    (void)self; return getenv(name);
}
static size_t cgi_io_read(io_t *self, char *buf, size_t n) {
    (void)self; return fread(buf, 1, n, stdin);
}
static void cgi_io_write(io_t *self, const char *buf, size_t n) {
    (void)self; fwrite(buf, 1, n, stdout);
}

int main(void) {
    if (fcgi_should_serve())
        return fcgi_serve(handle_request);

    {
        io_t cgi = { cgi_io_env, cgi_io_read, cgi_io_write };
        handle_request(&cgi);
    }
    return 0;
}
