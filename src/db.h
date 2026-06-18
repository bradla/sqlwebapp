/* db.h - database execution layer (C99) */
#ifndef CSQLPAGE_DB_H
#define CSQLPAGE_DB_H

#include "cgi.h"
#include "row.h"

/* Called once per result row produced by the SQL script.
   `row` is valid only for the duration of the call. */
typedef void (*row_cb)(const row_t *row, void *user);

/* Run every statement in `sql` against the database named by `url`
   (SQLPAGE_DATABASE_URL). The scheme selects the backend: sqlite:// (or a
   bare path) or postgres:// / postgresql://. Named parameters ($x / :x) are
   bound from the request. Each result row is delivered to `cb`. Returns 0 on
   success; on error writes a message to `*errmsg` (caller frees) and returns
   non-zero. */
int db_run_script(const char        *url,
                  const char        *sql,
                  const cgi_request_t *req,
                  row_cb             cb,
                  void              *user,
                  char             **errmsg);

#endif
