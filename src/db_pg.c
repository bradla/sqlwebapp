/* db_pg.c - run a SQLPage-style SQL script against PostgreSQL via libpq.
 *
 * Only compiled when WITH_PG is defined (make PG=1). libpq speaks positional
 * placeholders, so each statement is split out and its $name/:name parameters
 * are rewritten to $1..$N by sqlparse before being sent.
 *
 * Each thread keeps its own PGconn for the life of the process (the HTTP
 * server runs one per worker thread; FastCGI and CGI have a single thread).
 * Parsed scripts are cached process-wide, keyed by their text, and every
 * statement is sent as a named prepared statement, prepared once per
 * connection: Postgres parses and plans each statement once, not per request.
 * (That caches query plans, never data.)
 */
#ifdef WITH_PG

#include "db_backends.h"
#include "sqlfn.h"
#include "sqlparse.h"
#include "util.h"

#include <libpq-fe.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- parsed-script cache (process-wide) --------------------------------- */

typedef struct script {
    char          *text;
    pg_stmt_t     *stmts;
    size_t         nstmt;
    struct script *next;
} script_t;

#define SCRIPT_BUCKETS 256
static script_t        *g_scripts[SCRIPT_BUCKETS];
static pthread_mutex_t  g_scripts_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned hash_text(const char *s) {
    unsigned h = 2166136261u;                 /* FNV-1a */
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static script_t *script_get(const char *sql) {
    unsigned  b = hash_text(sql) % SCRIPT_BUCKETS;
    script_t *s;
    pthread_mutex_lock(&g_scripts_lock);
    for (s = g_scripts[b]; s; s = s->next)
        if (strcmp(s->text, sql) == 0) break;
    if (!s) {
        s = xmalloc(sizeof *s);
        s->text  = xstrdup(sql);
        s->stmts = sqlparse_split(sql, &s->nstmt);
        s->next  = g_scripts[b];
        g_scripts[b] = s;
    }
    pthread_mutex_unlock(&g_scripts_lock);
    return s;                                 /* entries are never freed */
}

/* ---- per-thread connection ---------------------------------------------- */

static __thread PGconn        *t_conn     = NULL;
static __thread char          *t_conninfo = NULL;
static __thread unsigned char *t_prepared = NULL;   /* indexed by pg_stmt_t.id */
static __thread int            t_prepared_cap = 0;

static void pg_drop(void) {
    if (t_conn) { PQfinish(t_conn); t_conn = NULL; }
    free(t_conninfo);
    t_conninfo = NULL;
    if (t_prepared) memset(t_prepared, 0, (size_t)t_prepared_cap);
}

/* Return a usable connection for `conninfo`, reusing this thread's one when it
   matches and is healthy. Returns NULL and sets err on failure. */
static PGconn *pg_get(const char *conninfo, db_error_t *err) {
    if (t_conn) {
        if (t_conninfo && strcmp(t_conninfo, conninfo) == 0 &&
            PQstatus(t_conn) == CONNECTION_OK)
            return t_conn;
        pg_drop();                 /* different URL, or the connection broke */
    }

    t_conn = PQconnectdb(conninfo);
    if (PQstatus(t_conn) != CONNECTION_OK) {
        err->message = xstrdup(PQerrorMessage(t_conn));
        PQfinish(t_conn);
        t_conn = NULL;
        return NULL;
    }
    t_conninfo = xstrdup(conninfo);
    return t_conn;
}

/* Roll back any transaction left open by the script (including one cut short
   by a response) so the next request starts from a clean, idle session. */
static void pg_reset_session(PGconn *conn) {
    PGTransactionStatusType ts = PQtransactionStatus(conn);
    if (ts != PQTRANS_IDLE && ts != PQTRANS_UNKNOWN) {
        PGresult *res = PQexec(conn, "ROLLBACK");
        PQclear(res);
    }
}

static void set_db_error(db_error_t *err, PGconn *conn, PGresult *res) {
    const char *state   = res ? PQresultErrorField(res, PG_DIAG_SQLSTATE) : NULL;
    const char *primary = res ? PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY) : NULL;
    err->message = xstrdup(primary ? primary : res ? PQresultErrorMessage(res) : PQerrorMessage(conn));
    if (state) {
        snprintf(err->sqlstate, sizeof err->sqlstate, "%s", state);
        if (strncmp(state, "23", 2) == 0) err->status = 422;  /* integrity violation */
    }
}

/* Prepare `st` on this thread's connection unless already done. */
static PGresult *ensure_prepared(PGconn *conn, const pg_stmt_t *st, char *name) {
    sprintf(name, "s%d", st->id);
    if (st->id >= t_prepared_cap) {
        int cap = t_prepared_cap ? t_prepared_cap : 64;
        while (cap <= st->id) cap *= 2;
        t_prepared = xrealloc(t_prepared, (size_t)cap);
        memset(t_prepared + t_prepared_cap, 0, (size_t)(cap - t_prepared_cap));
        t_prepared_cap = cap;
    }
    if (!t_prepared[st->id]) {
        PGresult *res = PQprepare(conn, name, st->sql, st->nparams, NULL);
        if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) return res;
        PQclear(res);
        t_prepared[st->id] = 1;
    }
    return NULL;
}

/* Convert each result row into a row_t and hand it to the callback.
   Returns non-zero when the callback asked to stop. */
static int emit_pg_rows(PGresult *res, row_cb cb, void *user) {
    int nrows = PQntuples(res);
    int ncol  = PQnfields(res);
    int r, c, stop = 0;
    row_t row;

    row.len   = (size_t)ncol;
    row.cells = xmalloc((size_t)ncol * sizeof(cell_t));

    for (r = 0; r < nrows && !stop; r++) {
        for (c = 0; c < ncol; c++) {
            row.cells[c].name  = PQfname(res, c);
            row.cells[c].value = PQgetisnull(res, r, c) ? NULL : PQgetvalue(res, r, c);
        }
        stop = cb(&row, user);
    }
    free(row.cells);
    return stop;
}

/* Store a SET statement's result row (or NULLs when there is none). */
static void store_set(cgi_request_t *req, const pg_stmt_t *st, PGresult *res) {
    int i;
    for (i = 0; i < st->nset; i++) {
        const char *v = NULL;
        if (PQntuples(res) > 0 && i < PQnfields(res) && !PQgetisnull(res, 0, i))
            v = PQgetvalue(res, 0, i);
        map_add(&req->params, st->set_names[i], v);
    }
}

enum { RUN_OK, RUN_STOPPED, RUN_FAILED };

/* Execute every statement. Sets *progressed once any statement has completed
   on the server (so a retry could repeat it) and *conn_dead when a failure
   killed the connection. */
static int run_statements(PGconn *conn, const script_t *sc, cgi_request_t *req,
                          row_cb cb, void *user, db_error_t *err,
                          int *progressed, int *conn_dead) {
    size_t i;
    *conn_dead = 0;

    for (i = 0; i < sc->nstmt; i++) {
        const pg_stmt_t *st = &sc->stmts[i];
        const char     **values = NULL;
        char           **owned  = NULL;     /* function results to free */
        char             name[24];
        PGresult        *res;
        ExecStatusType   status = PGRES_EMPTY_QUERY;
        int              p, rc = RUN_OK;

        if (st->nparams > 0) {
            values = xmalloc((size_t)st->nparams * sizeof(char *));
            owned  = xmalloc((size_t)st->nparams * sizeof(char *));
            for (p = 0; p < st->nparams; p++) {
                owned[p] = NULL;
                if (strncmp(st->names[p], "sqlpage.", 8) == 0) {
                    int s = sqlfn_eval(st->names[p], req, &owned[p], &err->message);
                    if (s) {
                        err->status = s;
                        while (p >= 0) free(owned[p--]);
                        free(owned); free(values);
                        return RUN_FAILED;
                    }
                    values[p] = owned[p];
                } else {
                    values[p] = map_get(&req->params, st->names[p]); /* NULL -> SQL NULL */
                }
            }
        }

        if (st->set_param > 0) {            /* SET x = $n: no query needed */
            map_add(&req->params, st->set_names[0], values[st->set_param - 1]);
        } else if ((res = ensure_prepared(conn, st, name)) != NULL ||
                   !(res = PQexecPrepared(conn, name, st->nparams, values, NULL, NULL, 0)) ||
                   ((status = PQresultStatus(res)) != PGRES_TUPLES_OK &&
                    status != PGRES_COMMAND_OK && status != PGRES_EMPTY_QUERY)) {
            set_db_error(err, conn, res);
            if (PQstatus(conn) == CONNECTION_BAD) *conn_dead = 1;
            PQclear(res);
            rc = RUN_FAILED;
        } else {
            *progressed = 1;
            if (st->nset > 0)                store_set(req, st, res);
            else if (status == PGRES_TUPLES_OK && PQntuples(res) > 0 &&
                     emit_pg_rows(res, cb, user))
                rc = RUN_STOPPED;
            PQclear(res);
        }

        for (p = 0; p < st->nparams; p++) free(owned[p]);
        free(owned);
        free(values);
        if (rc != RUN_OK) return rc;
    }
    return RUN_OK;
}

int pg_run_script(const char    *conninfo,
                  const char    *sql,
                  cgi_request_t *req,
                  row_cb         cb,
                  void          *user,
                  db_error_t    *err) {
    const script_t *sc = script_get(sql);
    int             attempt;

    /* attempt 0 may reuse a cached connection; if it turns out to be dead
       before any output, attempt 1 reconnects from scratch. */
    for (attempt = 0; attempt < 2; attempt++) {
        PGconn *conn = pg_get(conninfo, err);
        int     progressed = 0, conn_dead = 0, rc;

        if (!conn) return 1;

        rc = run_statements(conn, sc, req, cb, user, err, &progressed, &conn_dead);
        if (!conn_dead) pg_reset_session(conn);

        if (rc != RUN_FAILED) return 0;

        if (conn_dead) {
            pg_drop();
            if (!progressed && attempt == 0) {   /* stale connection: retry once */
                free(err->message);
                err->message = NULL;
                err->sqlstate[0] = '\0';
                err->status = 500;
                continue;
            }
        }
        return 1;                             /* genuine error */
    }
    return 1;
}

#else /* !WITH_PG: keep this a non-empty translation unit (ISO C) */
typedef int csqlpage_db_pg_no_pg;
#endif /* WITH_PG */
