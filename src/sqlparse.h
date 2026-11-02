/* sqlparse.h - split a SQL script into statements and rewrite named
 * parameters ($name / :name) into PostgreSQL positional $1..$N (C99).
 *
 * Needed because libpq only accepts positional placeholders, whereas
 * SQLPage-style .sql files use named parameters. The SQLite backend does
 * not use this (SQLite parses named parameters itself).
 *
 * Two SQLPage extensions are recognised as well:
 *   - sqlpage.fn(args) calls become a parameter whose "name" is the call
 *     text; the app evaluates it (sqlfn.c) and binds the result.
 *   - SET x = expr            runs SELECT expr and stores the value as $x;
 *     SET (x, y) = (query)    runs the query (which may be INSERT/UPDATE/
 *                             DELETE ... RETURNING) and stores its first row.
 *     When expr is a lone parameter or function call, no query is sent.
 */
#ifndef CSQLPAGE_SQLPARSE_H
#define CSQLPAGE_SQLPARSE_H

#include <stddef.h>

typedef struct {
    char  *sql;       /* rewritten statement text (owned), no trailing ';' */
    char **names;     /* ordered, de-duplicated parameter names (owned)    */
    int    nparams;   /* length of `names` == highest $N used              */
    char **set_names; /* SET targets (owned), or NULL for a plain statement */
    int    nset;
    int    set_param; /* >0: the SET value is just $set_param (no query)    */
    int    id;        /* process-unique id, used to name prepared statements */
} pg_stmt_t;

/* Split `script` into non-empty statements. Returns a malloc'd array and
   writes its length to *count. Caller frees with sqlparse_free. */
pg_stmt_t *sqlparse_split(const char *script, size_t *count);
void       sqlparse_free(pg_stmt_t *stmts, size_t count);

#endif
