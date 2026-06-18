/* db.c - choose a database backend from the SQLPAGE_DATABASE_URL scheme.
 *
 *   postgres://... / postgresql://...  -> PostgreSQL (libpq, needs WITH_PG)
 *   sqlite://<path>  or  bare <path>   -> SQLite
 */
#include "db.h"
#include "db_backends.h"
#include "util.h"

#include <string.h>

int db_run_script(const char        *url,
                  const char        *sql,
                  const cgi_request_t *req,
                  row_cb             cb,
                  void              *user,
                  char             **errmsg) {
    if (strncmp(url, "postgres://", 11) == 0 ||
        strncmp(url, "postgresql://", 13) == 0) {
#ifdef WITH_PG
        /* libpq accepts the postgres:// URI form verbatim. */
        return pg_run_script(url, sql, req, cb, user, errmsg);
#else
        *errmsg = xstrdup("PostgreSQL support not compiled in "
                          "(rebuild with: make PG=1)");
        return 1;
#endif
    }

    if (strncmp(url, "sqlite://", 9) == 0) url += 9;
    return sqlite_run_script(url, sql, req, cb, user, errmsg);
}
