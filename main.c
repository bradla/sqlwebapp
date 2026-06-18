/* main.c - csqlpage CGI entry point (C99)
 *
 * Environment:
 *   SQLPAGE_WEB_ROOT       directory holding .sql files   (default ".")
 *   SQLPAGE_DATABASE_URL   sqlite path or sqlite://path    (default "./sqlpage.db")
 *
 * Routing: PATH_INFO -> <web_root>/<path>; "" or "/" maps to "index.sql".
 */
#include "cgi.h"
#include "db.h"
#include "render.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *getenv_or(const char *n, const char *d) {
    const char *v = getenv(n);
    return v ? v : d;
}

/* Reject paths that could escape the web root. */
static int path_is_safe(const char *p) {
    if (strstr(p, "..")) return 0;
    for (; *p; p++) if (*p == '\\' || *p == '\0') return 0;
    return 1;
}

/* Resolve the DB filename from SQLPAGE_DATABASE_URL (strip sqlite:// scheme). */
static const char *db_path(void) {
    const char *url = getenv_or("SQLPAGE_DATABASE_URL", "./sqlpage.db");
    if (strncmp(url, "sqlite://", 9) == 0) url += 9;
    return url;
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

int main(void) {
    cgi_request_t req;
    render_t     *r;
    const char   *web_root;
    const char   *rel;
    char         *fullpath;
    char         *sql;
    char         *errmsg = NULL;

    cgi_read(&req);
    r = render_new();

    /* Map the request path to a .sql file. */
    rel = req.path_info;
    while (*rel == '/') rel++;             /* drop leading slashes */
    if (*rel == '\0') rel = "index.sql";

    if (!path_is_safe(rel)) {
        render_error(r, "Forbidden: invalid path");
        cgi_free(&req);
        return 0;
    }

    web_root = getenv_or("SQLPAGE_WEB_ROOT", ".");
    {
        size_t n = strlen(web_root) + 1 + strlen(rel) + 1;
        fullpath = xmalloc(n);
        snprintf(fullpath, n, "%s/%s", web_root, rel);
    }

    sql = read_file(fullpath);
    if (!sql) {
        sb_t msg; sb_init(&msg);
        sb_printf(&msg, "Not found: %s", fullpath);
        /* 404-style: report as error page */
        render_error(r, msg.data);
        sb_free(&msg);
        free(fullpath);
        cgi_free(&req);
        return 0;
    }

    if (db_run_script(db_path(), sql, &req, render_row, r, &errmsg) != 0) {
        sb_t msg; sb_init(&msg);
        sb_printf(&msg, "Database error: %s", errmsg ? errmsg : "(unknown)");
        render_error(r, msg.data);
        sb_free(&msg);
        free(errmsg);
    } else {
        render_finish(r);
    }

    free(sql);
    free(fullpath);
    cgi_free(&req);
    return 0;
}
