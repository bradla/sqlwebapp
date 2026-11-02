/* db.h - database execution layer (C99) */
#ifndef CSQLPAGE_DB_H
#define CSQLPAGE_DB_H

#include "cgi.h"
#include "row.h"

/* Called once per result row produced by the SQL script.
   `row` is valid only for the duration of the call. Returning non-zero stops
   the script: no further rows or statements run (the response is complete). */
typedef int (*row_cb)(const row_t *row, void *user);

/* Why a script failed. */
typedef struct {
    char *message;      /* malloc'd; caller frees                          */
    char  sqlstate[6];  /* SQLSTATE of a database error, else ""           */
    int   status;       /* HTTP status to answer with: 401 when a sqlpage.
                           auth function rejected the request, 422 for an
                           integrity violation (class 23), else 500        */
} db_error_t;

/* Run every statement in `sql` against the database named by `url`
   (SQLPAGE_DATABASE_URL). The scheme selects the backend: sqlite:// (or a
   bare path) or postgres:// / postgresql://. Named parameters ($x / :x) are
   bound from the request; SET statements add to them. Each result row is
   delivered to `cb`. Returns 0 on success; on error fills `*err` and returns
   non-zero. */
int db_run_script(const char    *url,
                  const char    *sql,
                  cgi_request_t *req,
                  row_cb         cb,
                  void          *user,
                  db_error_t    *err);

#endif
