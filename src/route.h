/* route.h - file-system routing with methods and path parameters (C99).
 *
 * The web root is scanned into a tree. A request path maps to a .sql file:
 *
 *   /users.sql              users.sql                (the file, as before)
 *   /api/tags               api/tags.get.sql         (method-specific, or)
 *                           api/tags.sql             (any method)
 *   /api/user/              api/user/index[.get].sql
 *   /api/articles/abc-1     api/articles/[slug].get.sql   binds $slug = abc-1
 *   /api/articles/abc-1/comments/7
 *                           api/articles/[slug]/comments/[id].delete.sql
 *
 * Literal names win over [param] names; method-specific files win over
 * any-method ones. "" or "/" maps to index.sql.
 */
#ifndef CSQLPAGE_ROUTE_H
#define CSQLPAGE_ROUTE_H

#include "map.h"

typedef struct {
    char *path;     /* file path under the web root (owned)          */
    char *sql;      /* file contents when loaded with cache_sql, else NULL */
} route_file_t;

typedef struct route_node route_tree_t;

/* Scan `web_root`. With cache_sql, file contents are read now and kept
   (the server never rereads them: restart to pick up edits). */
route_tree_t       *route_load(const char *web_root, int cache_sql);
void                route_free(route_tree_t *t);

/* Resolve `path` (URL-decoded) for `method`. On success returns the file and
   adds the path parameters to `params`; returns NULL when nothing matches. */
const route_file_t *route_find(const route_tree_t *t, const char *method,
                               const char *path, map_t *params);

#endif
