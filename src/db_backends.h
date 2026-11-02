/* db_backends.h - internal: per-backend script runners (C99).
 * Each has the same shape as db_run_script but receives a backend-specific
 * target (a file path for SQLite, a libpq conninfo/URI for PostgreSQL). */
#ifndef CSQLPAGE_DB_BACKENDS_H
#define CSQLPAGE_DB_BACKENDS_H

#include "db.h"   /* row_cb, db_error_t, cgi_request_t */

int sqlite_run_script(const char *path, const char *sql,
                      cgi_request_t *req, row_cb cb, void *user,
                      db_error_t *err);

int pg_run_script(const char *conninfo, const char *sql,
                  cgi_request_t *req, row_cb cb, void *user,
                  db_error_t *err);

#endif
