/* db.h - SQLite execution layer (C99) */
#ifndef CSQLPAGE_DB_H
#define CSQLPAGE_DB_H

#include "cgi.h"
#include "row.h"

/* Called once per result row produced by the SQL script.
   `row` is valid only for the duration of the call. */
typedef void (*row_cb)(const row_t *row, void *user);

/* Open `db_path`, run every statement in `sql` in order, binding named
   parameters ($x / :x / @x) from the request. Each SELECT row is delivered
   to `cb`. Returns 0 on success; on error writes a message to `*errmsg`
   (caller frees) and returns non-zero. */
int db_run_script(const char        *db_path,
                  const char        *sql,
                  const cgi_request_t *req,
                  row_cb             cb,
                  void              *user,
                  char             **errmsg);

#endif
