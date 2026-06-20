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

    int    debug;           /* append a debug footer?                      */
    double total_ms;        /* whole-request wall time                     */
    double db_ms;           /* time spent in the data/SQL request          */
    long   rss_kb;          /* peak resident memory (KB), or -1            */

    char  *assets_base;     /* URL prefix for Tabler/ApexCharts (owned)    */

    /* chart component accumulation */
    int    chart_seq;       /* unique id counter for chart <div>s          */
    char  *chart_type;      /* line | bar | area | pie | donut (owned)     */
    sb_t   chart_x;         /* JSON-encoded x labels, comma-separated      */
    sb_t   chart_y;         /* JSON-encoded y values, comma-separated      */

    /* interactive ("spreadsheet") table state */
    int    el_seq;          /* unique id counter for interactive elements  */
    int    table_id;        /* id of the active table if interactive, else 0 */
    int    table_search;    /* show a search box?                          */
    int    table_sort;      /* allow column sorting?                       */
    int    table_per_page;  /* rows per page                               */

    /* map component state */
    int    map_id;          /* id of the active map's <div>                */
    char  *map_lat;         /* center latitude prop (owned), or NULL       */
    char  *map_lon;         /* center longitude prop (owned), or NULL      */
    char  *map_zoom;        /* zoom prop (owned), or NULL                  */
    sb_t   map_markers;     /* JSON [[lat,lon,popup],...] without brackets  */
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

/* ---- JSON helpers (for the chart component's data) --------------------- */

static void json_quote(sb_t *b, const char *s) {
    sb_putc(b, '"');
    if (s) for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
            case '"':  sb_puts(b, "\\\""); break;
            case '\\': sb_puts(b, "\\\\"); break;
            case '\n': sb_puts(b, "\\n");  break;
            case '\r': sb_puts(b, "\\r");  break;
            case '\t': sb_puts(b, "\\t");  break;
            default:
                if (c < 0x20) sb_printf(b, "\\u%04x", c);
                else          sb_putc(b, (char)c);
        }
    }
    sb_putc(b, '"');
}

/* True when `s` is parseable as a JSON-safe number (emit unquoted). */
static int is_number(const char *s) {
    char *end;
    if (!s || !*s) return 0;
    strtod(s, &end);
    return *end == '\0';
}

/* Append a JSON value: bare number when numeric, quoted string otherwise,
   `null` when absent. */
static void json_value(sb_t *b, const char *s) {
    if (!s)              sb_puts(b, "null");
    else if (is_number(s)) sb_puts(b, s);
    else                 json_quote(b, s);
}

/* ---- component lifecycle ----------------------------------------------- */

static void component_open(render_t *r, const row_t *props);
static void component_close(render_t *r);
static void component_data_row(render_t *r, const row_t *row);

render_t *render_new(const char *assets_base) {
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
    r->debug         = 0;
    r->total_ms      = 0;
    r->db_ms         = 0;
    r->rss_kb        = -1;
    r->assets_base   = xstrdup(assets_base ? assets_base : "/assets");
    r->chart_seq     = 0;
    r->chart_type    = NULL;
    sb_init(&r->chart_x);
    sb_init(&r->chart_y);
    r->el_seq         = 0;
    r->table_id       = 0;
    r->table_search   = 0;
    r->table_sort     = 0;
    r->table_per_page = 10;
    r->map_id         = 0;
    r->map_lat        = NULL;
    r->map_lon        = NULL;
    r->map_zoom       = NULL;
    sb_init(&r->map_markers);
    return r;
}

/* Treat a prop as true unless it is absent or an explicit falsey token. */
static int truthy(const char *v) {
    return v && *v && strcmp(v, "0") != 0 &&
           strcmp(v, "false") != 0 && strcmp(v, "FALSE") != 0 && strcmp(v, "no") != 0;
}

void render_set_debug(render_t *r, double total_ms, double db_ms, long rss_kb) {
    r->debug    = 1;
    r->total_ms = total_ms;
    r->db_ms    = db_ms;
    r->rss_kb   = rss_kb;
}

/* Append a fixed debug bar reporting timing and memory. */
static void append_debug_bar(render_t *r) {
    size_t bytes = r->body.len;   /* response size so far, before the bar */
    sb_puts(&r->body,
        "<div style=\"position:fixed;bottom:0;left:0;right:0;z-index:9999;"
        "background:#222;color:#9f9;font:12px/1.4 monospace;"
        "padding:.4rem .8rem;border-top:2px solid #9f9\">");
    sb_printf(&r->body,
        "csqlpage debug &middot; total %.2f ms &middot; data/SQL %.2f ms "
        "&middot; response %lu B",
        r->total_ms, r->db_ms, (unsigned long)bytes);
    if (r->rss_kb >= 0)
        sb_printf(&r->body, " &middot; peak RSS %ld KB", r->rss_kb);
    sb_puts(&r->body, "</div>\n");
}

/* Open the shell <head> + opening <body> markup into r->body.
   Tabler provides the styling; ApexCharts powers the chart component. Both are
   loaded from SQLPAGE_ASSETS_BASE, which the front-end web server serves. */
static void shell_open(render_t *r) {
    const char *base = r->assets_base;
    sb_puts(&r->body, "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n");
    sb_puts(&r->body, "<meta charset=\"utf-8\">\n");
    sb_puts(&r->body, "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n");
    sb_puts(&r->body, "<title>");
    put_esc(&r->body, r->title ? r->title : "SQLPage");
    sb_puts(&r->body, "</title>\n");
    sb_printf(&r->body, "<link rel=\"stylesheet\" href=\"%s/tabler.min.css\">\n", base);
    sb_printf(&r->body, "<link rel=\"stylesheet\" href=\"%s/simple-datatables.css\">\n", base);
    sb_printf(&r->body, "<link rel=\"stylesheet\" href=\"%s/leaflet.css\">\n", base);
    sb_printf(&r->body, "<script src=\"%s/apexcharts.min.js\"></script>\n", base);
    sb_printf(&r->body, "<script src=\"%s/simple-datatables.js\"></script>\n", base);
    sb_printf(&r->body, "<script src=\"%s/leaflet.js\"></script>\n", base);
    sb_puts(&r->body, "</head>\n<body>\n");
    sb_puts(&r->body,
        "<div class=\"page\"><div class=\"page-wrapper\"><div class=\"page-body\">\n"
        "<div class=\"container-xl\">\n");
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

static void card_header(render_t *r, const char *title) {
    if (!title) return;
    sb_puts(&r->body, "<div class=\"card-header\"><h3 class=\"card-title\">");
    put_esc(&r->body, title);
    sb_puts(&r->body, "</h3></div>\n");
}

static void component_open(render_t *r, const row_t *props) {
    const char *c     = r->current;
    const char *title = row_get(props, "title");

    if (strcmp(c, "text") == 0) {
        if (title) { sb_puts(&r->body, "<h2 class=\"mb-2\">"); put_esc(&r->body, title); sb_puts(&r->body, "</h2>\n"); }
        { const char *contents = row_get(props, "contents");
          if (contents) { sb_puts(&r->body, "<p>"); put_esc(&r->body, contents); sb_puts(&r->body, "</p>\n"); } }

    } else if (strcmp(c, "table") == 0) {
        const char *pp = row_get(props, "per_page");
        r->table_search   = truthy(row_get(props, "search"));
        r->table_sort     = truthy(row_get(props, "sort"));
        r->table_per_page = pp ? atoi(pp) : 10;
        if (r->table_per_page <= 0) r->table_per_page = 10;
        /* interactive ("spreadsheet") only when sort/search requested */
        r->table_id = (r->table_search || r->table_sort) ? ++r->el_seq : 0;
        sb_puts(&r->body, "<div class=\"card mb-3\">\n");
        card_header(r, title);
        /* the <table> opens lazily on the first data row (needs columns) */

    } else if (strcmp(c, "list") == 0) {
        sb_puts(&r->body, "<div class=\"card mb-3\">\n");
        card_header(r, title);
        sb_puts(&r->body, "<div class=\"list-group list-group-flush\">\n");

    } else if (strcmp(c, "card") == 0) {
        if (title) { sb_puts(&r->body, "<h2 class=\"mb-2\">"); put_esc(&r->body, title); sb_puts(&r->body, "</h2>\n"); }
        sb_puts(&r->body, "<div class=\"row row-cards mb-3\">\n");

    } else if (strcmp(c, "form") == 0) {
        const char *action = row_get(props, "action");
        const char *method = row_get(props, "method");
        sb_puts(&r->body, "<form class=\"card mb-3\" method=\"");
        put_esc(&r->body, method ? method : "post");
        sb_puts(&r->body, "\"");
        if (action) { sb_puts(&r->body, " action=\""); put_esc(&r->body, action); sb_puts(&r->body, "\""); }
        sb_puts(&r->body, ">\n<div class=\"card-body\">\n");
        if (title) { sb_puts(&r->body, "<h3 class=\"card-title mb-3\">"); put_esc(&r->body, title); sb_puts(&r->body, "</h3>\n"); }

    } else if (strcmp(c, "chart") == 0) {
        const char *type = row_get(props, "type");
        free(r->chart_type);
        r->chart_type = xstrdup(type ? type : "line");
        sb_clear(&r->chart_x);
        sb_clear(&r->chart_y);
        r->chart_seq++;
        sb_puts(&r->body, "<div class=\"card mb-3\">\n");
        card_header(r, title);
        sb_printf(&r->body, "<div class=\"card-body\"><div id=\"chart%d\"></div></div>\n", r->chart_seq);

    } else if (strcmp(c, "map") == 0) {
        free(r->map_lat);  r->map_lat  = dup_or_null(row_get(props, "latitude"));
        free(r->map_lon);  r->map_lon  = dup_or_null(row_get(props, "longitude"));
        free(r->map_zoom); r->map_zoom = dup_or_null(row_get(props, "zoom"));
        sb_clear(&r->map_markers);
        r->map_id = ++r->el_seq;
        sb_puts(&r->body, "<div class=\"card mb-3\">\n");
        card_header(r, title);
        sb_printf(&r->body,
            "<div class=\"card-body\"><div id=\"map%d\" style=\"height:400px\"></div></div>\n",
            r->map_id);
    }
}

static void component_data_row(render_t *r, const row_t *row) {
    const char *c = r->current;

    if (strcmp(c, "table") == 0) {
        size_t i;
        if (r->comp_rows == 0) {            /* first row: build header */
            sb_puts(&r->body, "<div class=\"table-responsive\">"
                              "<table class=\"table table-vcenter card-table\"");
            if (r->table_id) sb_printf(&r->body, " id=\"table%d\"", r->table_id);
            sb_puts(&r->body, ">\n<thead>\n<tr>");
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
        if (link) { sb_puts(&r->body, "<a class=\"list-group-item list-group-item-action\" href=\"");
                    put_esc(&r->body, link); sb_puts(&r->body, "\">"); }
        else      { sb_puts(&r->body, "<div class=\"list-group-item\">"); }
        sb_puts(&r->body, "<div class=\"fw-bold\">"); put_esc(&r->body, title ? title : ""); sb_puts(&r->body, "</div>");
        if (desc) { sb_puts(&r->body, "<div class=\"text-secondary\">"); put_esc(&r->body, desc); sb_puts(&r->body, "</div>"); }
        sb_puts(&r->body, link ? "</a>\n" : "</div>\n");

    } else if (strcmp(c, "card") == 0) {
        const char *title = row_get(row, "title");
        const char *desc  = row_get(row, "description");
        const char *link  = row_get(row, "link");
        sb_puts(&r->body, "<div class=\"col-sm-6 col-lg-4\"><div class=\"card card-sm\"><div class=\"card-body\">");
        if (title) { sb_puts(&r->body, "<h3 class=\"card-title\">"); put_esc(&r->body, title); sb_puts(&r->body, "</h3>"); }
        if (desc)  { sb_puts(&r->body, "<p class=\"text-secondary\">"); put_esc(&r->body, desc); sb_puts(&r->body, "</p>"); }
        if (link)  { sb_puts(&r->body, "<a class=\"btn btn-primary\" href=\""); put_esc(&r->body, link);
                     sb_puts(&r->body, "\">More</a>"); }
        sb_puts(&r->body, "</div></div></div>\n");

    } else if (strcmp(c, "form") == 0) {
        const char *name  = row_get(row, "name");
        const char *label = row_get(row, "label");
        const char *type  = row_get(row, "type");
        const char *value = row_get(row, "value");
        sb_puts(&r->body, "<div class=\"mb-3\"><label class=\"form-label\">");
        put_esc(&r->body, label ? label : (name ? name : ""));
        sb_puts(&r->body, "</label>");
        if (type && strcmp(type, "textarea") == 0) {
            sb_puts(&r->body, "<textarea class=\"form-control\" name=\"");
            put_esc(&r->body, name ? name : ""); sb_puts(&r->body, "\">");
            put_esc(&r->body, value); sb_puts(&r->body, "</textarea>");
        } else {
            sb_puts(&r->body, "<input class=\"form-control\" type=\"");
            put_esc(&r->body, type ? type : "text");
            sb_puts(&r->body, "\" name=\"");
            put_esc(&r->body, name ? name : "");
            sb_puts(&r->body, "\" value=\"");
            put_esc(&r->body, value);
            sb_puts(&r->body, "\">");
        }
        sb_puts(&r->body, "</div>\n");

    } else if (strcmp(c, "text") == 0) {
        const char *contents = row_get(row, "contents");
        sb_puts(&r->body, "<p>"); put_esc(&r->body, contents); sb_puts(&r->body, "</p>\n");

    } else if (strcmp(c, "chart") == 0) {
        /* accumulate one (x, y) point as JSON for ApexCharts */
        if (r->comp_rows > 0) { sb_putc(&r->chart_x, ','); sb_putc(&r->chart_y, ','); }
        json_value(&r->chart_x, row_get(row, "x"));
        json_value(&r->chart_y, row_get(row, "y"));

    } else if (strcmp(c, "map") == 0) {
        /* accumulate one marker [lat, lon, popup] as JSON for Leaflet */
        const char *popup = row_get(row, "title");
        if (!popup) popup = row_get(row, "description");
        if (r->comp_rows > 0) sb_putc(&r->map_markers, ',');
        sb_putc(&r->map_markers, '[');
        json_value(&r->map_markers, row_get(row, "latitude"));
        sb_putc(&r->map_markers, ',');
        json_value(&r->map_markers, row_get(row, "longitude"));
        sb_putc(&r->map_markers, ',');
        json_value(&r->map_markers, popup);     /* number or quoted string or null */
        sb_putc(&r->map_markers, ']');
    }
}

static void component_close(render_t *r) {
    if (!r->current) return;
    if (strcmp(r->current, "table") == 0) {
        if (r->comp_rows > 0) sb_puts(&r->body, "</tbody>\n</table></div>\n"); /* tbody, table, responsive */
        sb_puts(&r->body, "</div>\n");                                          /* card */
        if (r->table_id && r->comp_rows > 0)                                    /* make it a spreadsheet */
            sb_printf(&r->body,
                "<script>new simpleDatatables.DataTable("
                "document.getElementById('table%d'),"
                "{searchable:%s,sortable:%s,paging:true,perPage:%d});</script>\n",
                r->table_id, r->table_search ? "true" : "false",
                r->table_sort ? "true" : "false", r->table_per_page);
    } else if (strcmp(r->current, "list") == 0) {
        sb_puts(&r->body, "</div></div>\n");                                    /* list-group, card */
    } else if (strcmp(r->current, "card") == 0) {
        sb_puts(&r->body, "</div>\n");                                          /* row row-cards */
    } else if (strcmp(r->current, "form") == 0) {
        sb_puts(&r->body, "<button class=\"btn btn-primary\" type=\"submit\">Submit</button>\n"
                          "</div>\n</form>\n");                                  /* button, card-body, form */
    } else if (strcmp(r->current, "chart") == 0) {
        const char *t   = r->chart_type ? r->chart_type : "line";
        int         pie = (strcmp(t, "pie") == 0 || strcmp(t, "donut") == 0);
        sb_printf(&r->body, "<script>new ApexCharts(document.getElementById('chart%d'),{",
                  r->chart_seq);
        sb_printf(&r->body, "chart:{type:'%s',height:320}", t);
        if (pie) {
            sb_puts(&r->body, ",series:["); sb_write(&r->body, r->chart_y.data, r->chart_y.len);
            sb_puts(&r->body, "],labels:["); sb_write(&r->body, r->chart_x.data, r->chart_x.len);
            sb_puts(&r->body, "]");
        } else {
            sb_puts(&r->body, ",series:[{name:'value',data:[");
            sb_write(&r->body, r->chart_y.data, r->chart_y.len);
            sb_puts(&r->body, "]}],xaxis:{categories:[");
            sb_write(&r->body, r->chart_x.data, r->chart_x.len);
            sb_puts(&r->body, "]}");
        }
        sb_puts(&r->body, "}).render();</script>\n");
        sb_puts(&r->body, "</div>\n");   /* close the chart's card */

    } else if (strcmp(r->current, "map") == 0) {
        sb_printf(&r->body,
            "<script>(function(){var m=L.map('map%d');"
            "L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',"
            "{maxZoom:19,attribution:'&copy; OpenStreetMap'}).addTo(m);var pts=[",
            r->map_id);
        sb_write(&r->body, r->map_markers.data, r->map_markers.len);
        sb_puts(&r->body,
            "];var g=L.featureGroup();pts.forEach(function(p){var k=L.marker([p[0],p[1]]);"
            "if(p[2]!=null)k.bindPopup(String(p[2]));k.addTo(g);});g.addTo(m);");
        if (r->map_lat && r->map_lon && is_number(r->map_lat) && is_number(r->map_lon)) {
            const char *z = (r->map_zoom && is_number(r->map_zoom)) ? r->map_zoom : "13";
            sb_printf(&r->body, "m.setView([%s,%s],%s);", r->map_lat, r->map_lon, z);
        } else {
            sb_puts(&r->body,
                "if(pts.length)m.fitBounds(g.getBounds().pad(0.2));else m.setView([0,0],2);");
        }
        sb_puts(&r->body, "})();</script>\n</div>\n");   /* close IIFE + card */
    }
}

/* ---- finalization ------------------------------------------------------ */

static void emit_headers(render_t *r, sb_t *out) {
    sb_printf(out, "Status: %d\r\n", r->status);
    if (r->redirect) sb_printf(out, "Location: %s\r\n", r->redirect);
    if (r->headers.len) sb_write(out, r->headers.data, r->headers.len);
    sb_puts(out, "Content-Type: text/html; charset=utf-8\r\n");
    sb_puts(out, "\r\n");
}

static void render_done(render_t *r) {
    sb_free(&r->body);
    sb_free(&r->headers);
    sb_free(&r->chart_x);
    sb_free(&r->chart_y);
    sb_free(&r->map_markers);
    free(r->redirect);
    free(r->title);
    free(r->current);
    free(r->assets_base);
    free(r->chart_type);
    free(r->map_lat);
    free(r->map_lon);
    free(r->map_zoom);
    free(r);
}

void render_finish(render_t *r, io_t *io) {
    sb_t out;
    flush_head(r);          /* ensure a head even for empty output */
    component_close(r);
    if (r->shell_enabled && !r->redirect)
        sb_puts(&r->body, "</div></div></div></div>\n");  /* container/page-body/wrapper/page */
    if (r->debug && !r->redirect)
        append_debug_bar(r);
    if (r->shell_enabled && !r->redirect)
        sb_puts(&r->body, "</body>\n</html>\n");

    sb_init(&out);
    emit_headers(r, &out);
    if (!r->redirect) sb_write(&out, r->body.data, r->body.len);
    io->write(io, out.data, out.len);
    sb_free(&out);

    render_done(r);
}

void render_error(render_t *r, io_t *io, const char *message) {
    /* Nothing has been written to the transport yet (output is buffered),
       so always emit a clean 500 with proper CGI headers. */
    sb_t out;
    sb_init(&out);
    sb_puts(&out, "Status: 500\r\nContent-Type: text/html; charset=utf-8\r\n\r\n");
    sb_puts(&out, "<!DOCTYPE html><html><head><title>Error</title></head><body>\n");
    sb_puts(&out, "<pre style=\"color:#b00;white-space:pre-wrap\">");
    sb_put_html(&out, message);
    sb_puts(&out, "</pre>\n");
    if (r->debug)
        sb_printf(&out, "<p style=\"font:12px monospace;color:#666\">"
                  "debug &middot; total %.2f ms &middot; data/SQL %.2f ms"
                  " &middot; peak RSS %ld KB</p>\n",
                  r->total_ms, r->db_ms, r->rss_kb);
    sb_puts(&out, "</body></html>\n");
    io->write(io, out.data, out.len);
    sb_free(&out);

    render_done(r);
}
