/* db.c - run a SQLPage-style SQL script against SQLite (C99) */
#include "db.h"
#include "util.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Bind each named parameter of `stmt` from the request param map.
   SQLite parses the SQL and reports parameter names like "$id" / ":id",
   so we reuse its parser instead of writing our own. Unknown params -> NULL. */
static void bind_named_params(sqlite3_stmt *stmt, const cgi_request_t *req) {
    int n = sqlite3_bind_parameter_count(stmt);
    int i;
    for (i = 1; i <= n; i++) {
        const char *pname = sqlite3_bind_parameter_name(stmt, i);
        const char *val   = NULL;
        if (pname && pname[1]) val = map_get(&req->params, pname + 1); /* skip sigil */
        if (val) sqlite3_bind_text(stmt, i, val, -1, SQLITE_TRANSIENT);
        else     sqlite3_bind_null(stmt, i);
    }
}

/* Build a row_t from the current step of a SELECT statement, invoke cb. */
static void emit_row(sqlite3_stmt *stmt, row_cb cb, void *user) {
    int    ncol = sqlite3_column_count(stmt);
    row_t  row;
    int    c;

    row.len   = (size_t)ncol;
    row.cells = xmalloc((size_t)ncol * sizeof(cell_t));
    for (c = 0; c < ncol; c++) {
        const char *name = sqlite3_column_name(stmt, c);
        row.cells[c].name = xstrdup(name ? name : "");
        if (sqlite3_column_type(stmt, c) == SQLITE_NULL) {
            row.cells[c].value = NULL;
        } else {
            const unsigned char *txt = sqlite3_column_text(stmt, c);
            row.cells[c].value = xstrdup(txt ? (const char *)txt : "");
        }
    }

    cb(&row, user);

    for (c = 0; c < ncol; c++) {
        free(row.cells[c].name);
        free(row.cells[c].value);
    }
    free(row.cells);
}

int db_run_script(const char        *db_path,
                  const char        *sql,
                  const cgi_request_t *req,
                  row_cb             cb,
                  void              *user,
                  char             **errmsg) {
    sqlite3     *db   = NULL;
    const char  *tail = sql;
    int          rc;

    *errmsg = NULL;
    rc = sqlite3_open(db_path, &db);
    if (rc != SQLITE_OK) {
        *errmsg = xstrdup(sqlite3_errmsg(db));
        sqlite3_close(db);
        return 1;
    }

    /* Step through each statement in the script in turn. */
    while (tail && *tail) {
        sqlite3_stmt *stmt = NULL;
        const char   *next = NULL;

        rc = sqlite3_prepare_v2(db, tail, -1, &stmt, &next);
        if (rc != SQLITE_OK) {
            *errmsg = xstrdup(sqlite3_errmsg(db));
            sqlite3_close(db);
            return 1;
        }
        if (!stmt) { tail = next; continue; } /* whitespace / comment only */

        bind_named_params(stmt, req);

        for (;;) {
            rc = sqlite3_step(stmt);
            if (rc == SQLITE_ROW) {
                emit_row(stmt, cb, user);
            } else if (rc == SQLITE_DONE) {
                break;
            } else {
                *errmsg = xstrdup(sqlite3_errmsg(db));
                sqlite3_finalize(stmt);
                sqlite3_close(db);
                return 1;
            }
        }
        sqlite3_finalize(stmt);
        tail = next;
    }

    sqlite3_close(db);
    return 0;
}
