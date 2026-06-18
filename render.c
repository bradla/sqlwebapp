/* render.c - SQLPage-style component dispatch and rendering (C99)
 *
 * Dispatch model (mirrors src/render.rs in the Rust original):
 *   - A row whose `component` column is non-NULL switches the active
 *     component; that row also supplies the component's *top-level props*.
 *   - Rows without a `component` column are *data rows* for the active
 *     component.
 *   - "Header" components (status_code, http_header, redirect, shell) are
 *     only honoured before any body output; they shape the response head.
 *   - The page is wrapped in a default `shell` unless `shell-empty` is used.
 */
#include "render.h"
#include "map.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct render_state {
    sb_t   body;            /* accumulated <body> HTML                      */
    sb_t   headers;         /* extra CGI headers (each ends with \r\n)      */
    int    status;          /* HTTP status code                            */
    char  *redirect;        /* Location: target, or NULL                   */

    int    shell_enabled;   /* wrap output in <html> shell?                */
    char  *title;           /* shell title                                 */

    int    head_flushed;    /* have we committed to the body phase?        */
    char  *current;         /* active body component name (owned), or NULL */
    long   comp_rows;       /* data rows emitted into current component     */
};

/* ---- helpers ----------------------------------------------------------- */

static char *dup_or_null(const char *s) { return s ? xstrdup(s) : NULL; }

static int is_header_component(const char *c) {
    return strcmp(c, "status_code") == 0 ||
           strcmp(c, "http_header") == 0 ||
           strcmp(c, "redirect")    == 0 ||
           strcmp(c, "shell")       == 0 ||
           strcmp(c, "shell-empty") == 0;
}

/* Append a value, HTML-escaped; if NULL, append nothing. */
static void put_esc(sb_t *b, const char *v) { if (v) sb_put_html(b, v); }

/* ---- component lifecycle ----------------------------------------------- */

static void component_open(render_t *r, const row_t *props);
static void component_close(render_t *r);
static void component_data_row(render_t *r, const row_t *row);

render_t *render_new(void) {
    render_t *r = xmalloc(sizeof(*r));
    sb_init(&r->body);
    sb_init(&r->headers);
    r->status        = 200;
    r->redirect      = NULL;
    r->shell_enabled = 1;
    r->title         = NULL;
    r->head_flushed  = 0;
    r->current       = NULL;
    r->comp_rows     = 0;
    return r;
}

/* Open the shell <head> + opening <body> markup into r->body. */
static void shell_open(render_t *r) {
    sb_puts(&r->body, "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n");
    sb_puts(&r->body, "<meta charset=\"utf-8\">\n");
    sb_puts(&r->body, "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n");
    sb_puts(&r->body, "<title>");
    put_esc(&r->body, r->title ? r->title : "SQLPage");
    sb_puts(&r->body, "</title>\n");
    /* Minimal built-in stylesheet; real SQLPage ships Tabler. */
    sb_puts(&r->body,
        "<style>"
        "body{font-family:system-ui,sans-serif;margin:2rem;max-width:60rem;color:#222}"
        "table{border-collapse:collapse;width:100%}"
        "th,td{border:1px solid #ddd;padding:.4rem .6rem;text-align:left}"
        "th{background:#f6f6f6}"
        ".card{border:1px solid #ddd;border-radius:.5rem;padding:1rem;margin:.5rem 0}"
        ".cards{display:flex;gap:1rem;flex-wrap:wrap}"
        "label{display:block;margin:.5rem 0 .2rem;font-weight:600}"
        "input,textarea,select{width:100%;padding:.4rem;box-sizing:border-box}"
        "button{margin-top:1rem;padding:.5rem 1rem}"
        "</style>\n");
    sb_puts(&r->body, "</head>\n<body>\n");
}

/* Commit to the body phase: emit shell head exactly once. */
static void flush_head(render_t *r) {
    if (r->head_flushed) return;
    r->head_flushed = 1;
    if (r->shell_enabled) shell_open(r);
}

/* ---- public row sink --------------------------------------------------- */

void render_row(const row_t *row, void *user) {
    render_t   *r    = user;
    const char *comp = row_get(row, "component");

    if (comp && *comp) {
        /* Header components are only valid before body output begins. */
        if (!r->head_flushed && is_header_component(comp)) {
            if (strcmp(comp, "status_code") == 0) {
                const char *c = row_get(row, "status");
                if (c) r->status = atoi(c);
            } else if (strcmp(comp, "redirect") == 0) {
                const char *loc = row_get(row, "link");
                free(r->redirect);
                r->redirect = dup_or_null(loc);
                r->status   = 302;
            } else if (strcmp(comp, "http_header") == 0) {
                /* Each prop name:value becomes a header line. */
                size_t i;
                for (i = 0; i < row->len; i++) {
                    if (strcmp(row->cells[i].name, "component") == 0) continue;
                    if (!row->cells[i].value) continue;
                    sb_printf(&r->headers, "%s: %s\r\n",
                              row->cells[i].name, row->cells[i].value);
                }
            } else if (strcmp(comp, "shell") == 0) {
                const char *t = row_get(row, "title");
                free(r->title);
                r->title = dup_or_null(t);
                r->shell_enabled = 1;
            } else { /* shell-empty */
                r->shell_enabled = 0;
            }
            return;
        }

        /* A body component: start the body phase, then switch component. */
        flush_head(r);
        component_close(r);
        free(r->current);
        r->current   = xstrdup(comp);
        r->comp_rows = 0;
        component_open(r, row);
        return;
    }

    /* No component column: data row for the active component. */
    flush_head(r);
    if (!r->current) {            /* implicit default, like SQLPage's table */
        r->current   = xstrdup("table");
        r->comp_rows = 0;
        component_open(r, row);
    }
    component_data_row(r, row);
    r->comp_rows++;
}

/* ---- the components ---------------------------------------------------- */

static void component_open(render_t *r, const row_t *props) {
    const char *c = r->current;
    if (strcmp(c, "text") == 0) {
        const char *title = row_get(props, "title");
        if (title) { sb_puts(&r->body, "<h2>"); put_esc(&r->body, title); sb_puts(&r->body, "</h2>\n"); }
        /* `contents` prop renders immediately as a paragraph. */
        { const char *contents = row_get(props, "contents");
          if (contents) { sb_puts(&r->body, "<p>"); put_esc(&r->body, contents); sb_puts(&r->body, "</p>\n"); } }
    } else if (strcmp(c, "list") == 0) {
        const char *title = row_get(props, "title");
        if (title) { sb_puts(&r->body, "<h2>"); put_esc(&r->body, title); sb_puts(&r->body, "</h2>\n"); }
        sb_puts(&r->body, "<ul>\n");
    } else if (strcmp(c, "card") == 0) {
        const char *title = row_get(props, "title");
        if (title) { sb_puts(&r->body, "<h2>"); put_esc(&r->body, title); sb_puts(&r->body, "</h2>\n"); }
        sb_puts(&r->body, "<div class=\"cards\">\n");
    } else if (strcmp(c, "form") == 0) {
        const char *action = row_get(props, "action");
        const char *method = row_get(props, "method");
        sb_puts(&r->body, "<form method=\"");
        put_esc(&r->body, method ? method : "post");
        sb_puts(&r->body, "\"");
        if (action) { sb_puts(&r->body, " action=\""); put_esc(&r->body, action); sb_puts(&r->body, "\""); }
        sb_puts(&r->body, ">\n");
    }
    /* `table` opens lazily on its first data row (it needs column names). */
}

static void component_data_row(render_t *r, const row_t *row) {
    const char *c = r->current;

    if (strcmp(c, "table") == 0) {
        size_t i;
        if (r->comp_rows == 0) {            /* first row: build header */
            sb_puts(&r->body, "<table>\n<thead>\n<tr>");
            for (i = 0; i < row->len; i++) {
                if (strcmp(row->cells[i].name, "component") == 0) continue;
                sb_puts(&r->body, "<th>");
                put_esc(&r->body, row->cells[i].name);
                sb_puts(&r->body, "</th>");
            }
            sb_puts(&r->body, "</tr>\n</thead>\n<tbody>\n");
        }
        sb_puts(&r->body, "<tr>");
        for (i = 0; i < row->len; i++) {
            if (strcmp(row->cells[i].name, "component") == 0) continue;
            sb_puts(&r->body, "<td>");
            put_esc(&r->body, row->cells[i].value);
            sb_puts(&r->body, "</td>");
        }
        sb_puts(&r->body, "</tr>\n");

    } else if (strcmp(c, "list") == 0) {
        const char *title = row_get(row, "title");
        const char *desc  = row_get(row, "description");
        const char *link  = row_get(row, "link");
        sb_puts(&r->body, "<li>");
        if (link) { sb_puts(&r->body, "<a href=\""); put_esc(&r->body, link); sb_puts(&r->body, "\">"); }
        put_esc(&r->body, title ? title : "");
        if (link) sb_puts(&r->body, "</a>");
        if (desc) { sb_puts(&r->body, " &mdash; "); put_esc(&r->body, desc); }
        sb_puts(&r->body, "</li>\n");

    } else if (strcmp(c, "card") == 0) {
        const char *title = row_get(row, "title");
        const char *desc  = row_get(row, "description");
        const char *link  = row_get(row, "link");
        sb_puts(&r->body, "<div class=\"card\">");
        if (title) { sb_puts(&r->body, "<h3>"); put_esc(&r->body, title); sb_puts(&r->body, "</h3>"); }
        if (desc)  { sb_puts(&r->body, "<p>");  put_esc(&r->body, desc);  sb_puts(&r->body, "</p>"); }
        if (link)  { sb_puts(&r->body, "<a href=\""); put_esc(&r->body, link);
                     sb_puts(&r->body, "\">More</a>"); }
        sb_puts(&r->body, "</div>\n");

    } else if (strcmp(c, "form") == 0) {
        const char *name  = row_get(row, "name");
        const char *label = row_get(row, "label");
        const char *type  = row_get(row, "type");
        const char *value = row_get(row, "value");
        sb_puts(&r->body, "<label>");
        put_esc(&r->body, label ? label : (name ? name : ""));
        sb_puts(&r->body, "</label>\n");
        if (type && strcmp(type, "textarea") == 0) {
            sb_puts(&r->body, "<textarea name=\"");
            put_esc(&r->body, name ? name : ""); sb_puts(&r->body, "\">");
            put_esc(&r->body, value); sb_puts(&r->body, "</textarea>\n");
        } else {
            sb_puts(&r->body, "<input type=\"");
            put_esc(&r->body, type ? type : "text");
            sb_puts(&r->body, "\" name=\"");
            put_esc(&r->body, name ? name : "");
            sb_puts(&r->body, "\" value=\"");
            put_esc(&r->body, value);
            sb_puts(&r->body, "\">\n");
        }

    } else if (strcmp(c, "text") == 0) {
        const char *contents = row_get(row, "contents");
        sb_puts(&r->body, "<p>"); put_esc(&r->body, contents); sb_puts(&r->body, "</p>\n");
    }
}

static void component_close(render_t *r) {
    if (!r->current) return;
    if (strcmp(r->current, "table") == 0) {
        if (r->comp_rows > 0) sb_puts(&r->body, "</tbody>\n</table>\n");
    } else if (strcmp(r->current, "list") == 0) {
        sb_puts(&r->body, "</ul>\n");
    } else if (strcmp(r->current, "card") == 0) {
        sb_puts(&r->body, "</div>\n");
    } else if (strcmp(r->current, "form") == 0) {
        sb_puts(&r->body, "<button type=\"submit\">Submit</button>\n</form>\n");
    }
}

/* ---- finalization ------------------------------------------------------ */

static void emit_headers(render_t *r) {
    printf("Status: %d\r\n", r->status);
    if (r->redirect) printf("Location: %s\r\n", r->redirect);
    if (r->headers.len) fwrite(r->headers.data, 1, r->headers.len, stdout);
    printf("Content-Type: text/html; charset=utf-8\r\n");
    printf("\r\n");
}

void render_finish(render_t *r) {
    flush_head(r);          /* ensure a head even for empty output */
    component_close(r);
    if (r->shell_enabled && !r->redirect)
        sb_puts(&r->body, "</body>\n</html>\n");

    emit_headers(r);
    if (!r->redirect) fwrite(r->body.data, 1, r->body.len, stdout);

    sb_free(&r->body);
    sb_free(&r->headers);
    free(r->redirect);
    free(r->title);
    free(r->current);
    free(r);
}

void render_error(render_t *r, const char *message) {
    /* The body is buffered until now, so nothing has reached stdout yet:
       always emit a clean 500 with proper CGI headers. */
    printf("Status: 500\r\nContent-Type: text/html; charset=utf-8\r\n\r\n");
    printf("<!DOCTYPE html><html><head><title>Error</title></head><body>\n");
    printf("<pre style=\"color:#b00;white-space:pre-wrap\">");
    /* escape into a temp buffer */
    { sb_t e; sb_init(&e); sb_put_html(&e, message);
      fwrite(e.data, 1, e.len, stdout); sb_free(&e); }
    printf("</pre>\n</body></html>\n");

    sb_free(&r->body);
    sb_free(&r->headers);
    free(r->redirect);
    free(r->title);
    free(r->current);
    free(r);
}
