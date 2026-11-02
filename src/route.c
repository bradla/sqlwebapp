/* route.c - implementation (C99 + POSIX dirent) */
#define _DEFAULT_SOURCE 1
#include "route.h"
#include "util.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_SEGS 32

typedef struct {
    char        *stem;      /* name without method and .sql; param name if is_param */
    int          is_param;  /* [name].sql                                           */
    char        *method;    /* "GET", ... or NULL for any method                    */
    route_file_t file;
} rfile_t;

struct route_node {
    char               *name;     /* directory name, or param name if is_param */
    int                 is_param;
    struct route_node  *kids;
    size_t              nkids;
    rfile_t            *files;
    size_t              nfiles;
};

static const char *METHODS[] = { "get", "post", "put", "patch", "delete", "head", "options" };

static char *read_all(const char *path) {
    FILE  *f = fopen(path, "rb");
    sb_t   sb;
    char   buf[4096];
    size_t n;
    if (!f) return NULL;
    sb_init(&sb);
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) sb_write(&sb, buf, n);
    fclose(f);
    return sb.data;
}

/* "[slug]" -> 1 and the inner name; otherwise 0 and a copy. */
static char *seg_name(const char *s, size_t n, int *is_param) {
    char *out;
    *is_param = n > 2 && s[0] == '[' && s[n - 1] == ']';
    if (*is_param) { s++; n -= 2; }
    out = xmalloc(n + 1);
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static void add_file(struct route_node *node, const char *fname, const char *path, int cache_sql) {
    size_t   n = strlen(fname) - 4;             /* drop ".sql" */
    rfile_t *f;
    size_t   i;
    const char *dot;

    node->files = xrealloc(node->files, (node->nfiles + 1) * sizeof(rfile_t));
    f = &node->files[node->nfiles++];
    f->method = NULL;

    /* "name.get.sql" -> stem "name", method GET */
    for (dot = fname + n; dot > fname && dot[-1] != '.'; dot--) {}
    if (dot > fname) {
        size_t mlen = (size_t)(fname + n - dot);
        for (i = 0; i < sizeof METHODS / sizeof *METHODS; i++) {
            if (strlen(METHODS[i]) == mlen && strncmp(dot, METHODS[i], mlen) == 0) {
                size_t k;
                f->method = xstrdup(METHODS[i]);
                for (k = 0; k < mlen; k++) f->method[k] = (char)(f->method[k] - 32);
                n = (size_t)(dot - 1 - fname);
                break;
            }
        }
    }
    f->stem      = seg_name(fname, n, &f->is_param);
    f->file.path = xstrdup(path);
    f->file.sql  = cache_sql ? read_all(path) : NULL;
}

static void scan(struct route_node *node, const char *dir, int cache_sql) {
    DIR           *d = opendir(dir);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        const char *nm = e->d_name;
        size_t      len = strlen(nm);
        struct stat st;
        sb_t        path;

        if (nm[0] == '.') continue;
        sb_init(&path);
        sb_printf(&path, "%s/%s", dir, nm);
        if (stat(path.data, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                struct route_node *kid;
                node->kids = xrealloc(node->kids, (node->nkids + 1) * sizeof *node->kids);
                kid = &node->kids[node->nkids++];
                memset(kid, 0, sizeof *kid);
                kid->name = seg_name(nm, len, &kid->is_param);
                scan(kid, path.data, cache_sql);
            } else if (len > 4 && strcmp(nm + len - 4, ".sql") == 0) {
                add_file(node, nm, path.data, cache_sql);
            }
        }
        sb_free(&path);
    }
    closedir(d);
}

route_tree_t *route_load(const char *web_root, int cache_sql) {
    struct route_node *root = xmalloc(sizeof *root);
    memset(root, 0, sizeof *root);
    root->name = xstrdup("");
    scan(root, web_root, cache_sql);
    return root;
}

static void free_node(struct route_node *n) {
    size_t i;
    for (i = 0; i < n->nkids; i++) free_node(&n->kids[i]);
    for (i = 0; i < n->nfiles; i++) {
        free(n->files[i].stem);
        free(n->files[i].method);
        free(n->files[i].file.path);
        free(n->files[i].file.sql);
    }
    free(n->kids);
    free(n->files);
    free(n->name);
}

void route_free(route_tree_t *t) {
    if (!t) return;
    free_node(t);
    free(t);
}

/* ---- resolution --------------------------------------------------------- */

typedef struct {
    const char *method;
    char      **segs;
    int         nsegs;
    const char *bind_name[MAX_SEGS];
    const char *bind_val[MAX_SEGS];
    int         nbind;
} query_t;

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        int x = (*a >= 'a' && *a <= 'z') ? *a - 32 : *a;
        int y = (*b >= 'a' && *b <= 'z') ? *b - 32 : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

/* File in `node` named `stem` (or any [param] file when stem is NULL) for the
   method: a method-specific file first, then an any-method one. */
static rfile_t *pick_file(const struct route_node *node, const char *stem, const char *method) {
    rfile_t *any = NULL;
    size_t   i;
    for (i = 0; i < node->nfiles; i++) {
        rfile_t *f = &node->files[i];
        if (stem ? (f->is_param || strcmp(f->stem, stem) != 0) : !f->is_param) continue;
        if (f->method && ieq(f->method, method)) return f;
        if (!f->method && !any) any = f;
    }
    return any;
}

static rfile_t *resolve(const struct route_node *node, query_t *q, int i) {
    const char *seg;
    size_t      k;
    rfile_t    *f;

    if (i == q->nsegs) return pick_file(node, "index", q->method);
    seg = q->segs[i];

    if (i == q->nsegs - 1) {
        size_t len = strlen(seg);
        if (len > 4 && strcmp(seg + len - 4, ".sql") == 0) {   /* explicit /x.sql */
            char *stem = xstrdup(seg);
            stem[len - 4] = '\0';
            f = pick_file(node, stem, q->method);
            free(stem);
            if (f) return f;
        }
        if ((f = pick_file(node, seg, q->method)) != NULL) return f;
    }
    for (k = 0; k < node->nkids; k++)                           /* literal dir */
        if (!node->kids[k].is_param && strcmp(node->kids[k].name, seg) == 0 &&
            (f = resolve(&node->kids[k], q, i + 1)) != NULL)
            return f;
    if (q->nbind == MAX_SEGS) return NULL;
    if (i == q->nsegs - 1 && (f = pick_file(node, NULL, q->method)) != NULL) {
        q->bind_name[q->nbind] = f->stem;                      /* [param].sql */
        q->bind_val[q->nbind++] = seg;
        return f;
    }
    for (k = 0; k < node->nkids; k++) {                         /* [param]/ dir */
        if (!node->kids[k].is_param) continue;
        q->bind_name[q->nbind] = node->kids[k].name;
        q->bind_val[q->nbind++] = seg;
        if ((f = resolve(&node->kids[k], q, i + 1)) != NULL) return f;
        q->nbind--;
    }
    return NULL;
}

const route_file_t *route_find(const route_tree_t *t, const char *method,
                               const char *path, map_t *params) {
    char       *copy = xstrdup(path), *p, *save, *segs[MAX_SEGS];
    query_t     q;
    rfile_t    *f;
    int         i;

    q.method = method;
    q.segs   = segs;
    q.nsegs  = 0;
    q.nbind  = 0;
    for (p = strtok_r(copy, "/", &save); p; p = strtok_r(NULL, "/", &save)) {
        if (q.nsegs == MAX_SEGS) { free(copy); return NULL; }
        segs[q.nsegs++] = p;
    }

    f = resolve(t, &q, 0);
    if (f)
        for (i = 0; i < q.nbind; i++) map_add(params, q.bind_name[i], q.bind_val[i]);
    free(copy);
    return f ? &f->file : NULL;
}
