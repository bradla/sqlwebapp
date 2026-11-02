#define _POSIX_C_SOURCE 200809L  /* clock_gettime / CLOCK_MONOTONIC */
#define _DEFAULT_SOURCE 1        /* getrusage */
/* main.c - csqlpage entry point: plain CGI, FastCGI or HTTP (C99)
 *
 * Environment (CGI vars + these):
 *   SQLPAGE_WEB_ROOT       directory holding .sql files     (default ".")
 *   SQLPAGE_DATABASE_URL   sqlite://... | postgres://...    (default sqlite)
 *   SQLPAGE_FCGI_BIND      optional "ip:port" or "/sock" to run as FastCGI
 *   SQLPAGE_HTTP_LISTEN    optional "host:port" to run the built-in HTTP server
 *   SQLPAGE_API_PREFIX     paths under it get JSON errors (e.g. "/api/")
 *   SQLPAGE_JWT_SECRET     HS256 key for the sqlpage.user_id()/jwt() functions
 *
 * Transport is chosen automatically: SQLPAGE_HTTP_LISTEN selects the HTTP
 * server; if launched as a FastCGI app (stdin is a listening socket) or
 * SQLPAGE_FCGI_BIND is set, it serves a FastCGI loop; otherwise it handles a
 * single CGI request and exits.
 *
 * Routing: PATH_INFO -> a .sql file under the web root (see route.h);
 * "" or "/" maps to "index.sql".
 */
#include "cgi.h"
#include "db.h"
#include "fastcgi.h"
#include "http.h"
#include "io.h"
#include "render.h"
#include "route.h"
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

/* Routes scanned once at startup by the HTTP server (scripts cached too);
   NULL under CGI/FastCGI, where each request rescans so edits show at once. */
static route_tree_t *g_routes = NULL;

/* Read an entire file into a NUL-terminated buffer (caller frees). */
static char *read_file(const char *path) {
    FILE  *f = fopen(path, "rb");
    sb_t   sb;
    char   buf[4096];
    size_t n;
    if (!f) return NULL;
    sb_init(&sb);
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) sb_write(&sb, buf, n);
    fclose(f);
    return sb.data;   /* ownership transferred; NUL-terminated by sb */
}

/* Handle one request on the given transport. */
static void handle_request(io_t *io) {
    cgi_request_t       req;
    render_t           *r;
    route_tree_t       *routes = g_routes;
    const route_file_t *file;
    const char         *rel, *dburl, *api;
    char               *sql = NULL;
    double              t_start = now_sec();
    double              db_ms   = 0;
    int                 dbg;

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

    api = io->env(io, "SQLPAGE_API_PREFIX");
    if (api && *api && strncmp(rel, api, strlen(api)) == 0) render_set_json_errors(r);

    /* Only files found by scanning the web root can match, so a path can't
       escape it. */
    if (!routes) routes = route_load(io_env_or(io, "SQLPAGE_WEB_ROOT", "."), 0);
    file = route_find(routes, req.method, rel, &req.params);
    if (file) sql = file->sql ? file->sql : read_file(file->path);
    if (!sql) {
        if (dbg) render_set_debug(r, (now_sec() - t_start) * 1000, db_ms, peak_rss_kb());
        render_error(r, io, 404, "Not found");
        goto done;
    }

    dburl = io_env_or(io, "SQLPAGE_DATABASE_URL", "sqlite://./sqlpage.db");
    {
        double     db_start = now_sec();
        db_error_t err;
        int        rc = db_run_script(dburl, sql, &req, render_row, r, &err);
        db_ms = (now_sec() - db_start) * 1000;
        if (dbg) render_set_debug(r, (now_sec() - t_start) * 1000, db_ms, peak_rss_kb());
        if (rc != 0) {
            sb_t msg; sb_init(&msg);
            if (err.status == 500) {
                sb_printf(&msg, "Database error: %s", err.message ? err.message : "(unknown)");
                fprintf(stderr, "%s %s: %s\n", req.method, rel, msg.data);
            } else {
                sb_puts(&msg, err.message ? err.message : "request rejected");
            }
            render_error(r, io, err.status, msg.data);
            sb_free(&msg);
            free(err.message);
        } else {
            render_finish(r, io);
        }
    }

done:
    if (file && !file->sql) free(sql);
    if (routes != g_routes) route_free(routes);
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
    if (http_should_serve()) {
        const char *root = getenv("SQLPAGE_WEB_ROOT");
        g_routes = route_load(root ? root : ".", 1);
        return http_serve(handle_request);
    }
    if (fcgi_should_serve())
        return fcgi_serve(handle_request);

    {
        io_t cgi = { cgi_io_env, cgi_io_read, cgi_io_write };
        handle_request(&cgi);
    }
    return 0;
}
