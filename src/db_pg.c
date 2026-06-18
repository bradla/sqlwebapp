/* db_pg.c - run a SQLPage-style SQL script against PostgreSQL via libpq.
 *
 * Only compiled when WITH_PG is defined (make PG=1). libpq speaks positional
 * placeholders, so each statement is split out and its $name/:name parameters
 * are rewritten to $1..$N by sqlparse before being sent with PQexecParams.
 *
 * The PGconn is cached across calls. Under FastCGI the process is resident, so
 * this reuses one connection for every request instead of reconnecting each
 * time. (Under plain CGI the process exits after one request, so the cache is
 * used once and closed at exit.)
 */
#ifdef WITH_PG

#include "db_backends.h"
#include "sqlparse.h"
#include "util.h"

#include <libpq-fe.h>
#include <stdlib.h>
#include <string.h>

/* Process-wide cached connection (the FastCGI server is single-threaded). */
static PGconn *g_conn     = NULL;
static char   *g_conninfo = NULL;
static int     g_atexit_done = 0;

static void pg_drop(void) {
    if (g_conn) { PQfinish(g_conn); g_conn = NULL; }
    free(g_conninfo);
    g_conninfo = NULL;
}

static void pg_atexit(void) { pg_drop(); }

/* Return a usable connection for `conninfo`, reusing the cached one when it
   matches and is healthy. Returns NULL and sets *errmsg on failure. */
static PGconn *pg_get(const char *conninfo, char **errmsg) {
    if (g_conn) {
        if (g_conninfo && strcmp(g_conninfo, conninfo) == 0) {
            if (PQstatus(g_conn) != CONNECTION_OK) PQreset(g_conn);
            if (PQstatus(g_conn) == CONNECTION_OK) return g_conn;
        }
        pg_drop();                 /* different URL, or reset failed */
    }

    g_conn = PQconnectdb(conninfo);
    if (PQstatus(g_conn) != CONNECTION_OK) {
        *errmsg = xstrdup(PQerrorMessage(g_conn));
        PQfinish(g_conn);
        g_conn = NULL;
        return NULL;
    }
    g_conninfo = xstrdup(conninfo);
    if (!g_atexit_done) { atexit(pg_atexit); g_atexit_done = 1; }
    return g_conn;
}

/* Roll back any transaction left open by the script so the next request that
   reuses this connection starts from a clean, idle session. */
static void pg_reset_session(PGconn *conn) {
    PGTransactionStatusType ts = PQtransactionStatus(conn);
    if (ts != PQTRANS_IDLE && ts != PQTRANS_UNKNOWN) {
        PGresult *res = PQexec(conn, "ROLLBACK");
        PQclear(res);
    }
}

/* Convert each result row into a row_t and hand it to the callback. */
static void emit_pg_rows(PGresult *res, row_cb cb, void *user) {
    int nrows = PQntuples(res);
    int ncol  = PQnfields(res);
    int r, c;
    row_t row;

    row.len   = (size_t)ncol;
    row.cells = xmalloc((size_t)ncol * sizeof(cell_t));

    for (r = 0; r < nrows; r++) {
        for (c = 0; c < ncol; c++) {
            row.cells[c].name = xstrdup(PQfname(res, c));
            row.cells[c].value =
                PQgetisnull(res, r, c) ? NULL : xstrdup(PQgetvalue(res, r, c));
        }
        cb(&row, user);
        for (c = 0; c < ncol; c++) {
            free(row.cells[c].name);
            free(row.cells[c].value);
        }
    }
    free(row.cells);
}

/* Execute every statement. Returns -1 on success, else the index of the
   failing statement; sets *conn_dead when the failure killed the connection.
   Because PQexecParams delivers a statement's rows all-or-nothing, a failure
   at index i means statements 0..i-1 already emitted and i emitted nothing. */
static int run_statements(PGconn *conn, pg_stmt_t *stmts, size_t nstmt,
                          const cgi_request_t *req, row_cb cb, void *user,
                          char **errmsg, int *conn_dead) {
    size_t i;
    int    p;
    *conn_dead = 0;

    for (i = 0; i < nstmt; i++) {
        pg_stmt_t   *st = &stmts[i];
        const char **values = NULL;
        PGresult    *res;
        ExecStatusType status;

        if (st->nparams > 0) {
            values = xmalloc((size_t)st->nparams * sizeof(char *));
            for (p = 0; p < st->nparams; p++)
                values[p] = map_get(&req->params, st->names[p]); /* NULL -> SQL NULL */
        }

        res = PQexecParams(conn, st->sql, st->nparams, NULL, values, NULL, NULL, 0);
        free(values);

        if (!res) {
            *errmsg = xstrdup(PQerrorMessage(conn));
            if (PQstatus(conn) == CONNECTION_BAD) *conn_dead = 1;
            return (int)i;
        }

        status = PQresultStatus(res);
        if (status == PGRES_TUPLES_OK) {
            emit_pg_rows(res, cb, user);
        } else if (status == PGRES_COMMAND_OK || status == PGRES_EMPTY_QUERY) {
            /* INSERT/UPDATE/DDL or comment-only statement: nothing to render. */
        } else {
            *errmsg = xstrdup(PQresultErrorMessage(res));
            if (PQstatus(conn) == CONNECTION_BAD) *conn_dead = 1;
            PQclear(res);
            return (int)i;
        }
        PQclear(res);
    }
    return -1;
}

int pg_run_script(const char        *conninfo,
                  const char        *sql,
                  const cgi_request_t *req,
                  row_cb             cb,
                  void              *user,
                  char             **errmsg) {
    pg_stmt_t *stmts;
    size_t     nstmt;
    int        attempt, result = 1;

    *errmsg = NULL;
    stmts = sqlparse_split(sql, &nstmt);

    /* attempt 0 may reuse a cached connection; if it turns out to be dead
       before any output, attempt 1 reconnects from scratch. */
    for (attempt = 0; attempt < 2; attempt++) {
        PGconn *conn = pg_get(conninfo, errmsg);
        int     conn_dead = 0, failed;

        if (!conn) { result = 1; break; }

        failed = run_statements(conn, stmts, nstmt, req, cb, user, errmsg, &conn_dead);
        pg_reset_session(conn);

        if (failed < 0) { result = 0; break; }          /* success */

        if (conn_dead && failed == 0 && attempt == 0) {  /* stale: retry once */
            free(*errmsg);
            *errmsg = NULL;
            pg_drop();
            continue;
        }
        result = 1;                                      /* genuine error */
        break;
    }

    sqlparse_free(stmts, nstmt);
    return result;
}

#else /* !WITH_PG: keep this a non-empty translation unit (ISO C) */
typedef int csqlpage_db_pg_no_pg;
#endif /* WITH_PG */
