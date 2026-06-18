/* sqlparse.h - split a SQL script into statements and rewrite named
 * parameters ($name / :name) into PostgreSQL positional $1..$N (C99).
 *
 * Needed because libpq only accepts positional placeholders, whereas
 * SQLPage-style .sql files use named parameters. The SQLite backend does
 * not use this (SQLite parses named parameters itself).
 */
#ifndef CSQLPAGE_SQLPARSE_H
#define CSQLPAGE_SQLPARSE_H

#include <stddef.h>

typedef struct {
    char  *sql;       /* rewritten statement text (owned), no trailing ';' */
    char **names;     /* ordered, de-duplicated parameter names (owned)    */
    int    nparams;   /* length of `names` == highest $N used              */
} pg_stmt_t;

/* Split `script` into non-empty statements. Returns a malloc'd array and
   writes its length to *count. Caller frees with sqlparse_free. */
pg_stmt_t *sqlparse_split(const char *script, size_t *count);
void       sqlparse_free(pg_stmt_t *stmts, size_t count);

#endif
