/* row.h - a single result row: ordered list of named cell values (C99) */
#ifndef CSQLPAGE_ROW_H
#define CSQLPAGE_ROW_H

#include <stddef.h>

typedef struct {
    char *name;    /* column name (owned)                 */
    char *value;   /* text value (owned), or NULL for SQL NULL */
} cell_t;

typedef struct {
    cell_t *cells;
    size_t  len;
} row_t;

/* Returns the value of column `name`, or NULL if absent or SQL NULL. */
const char *row_get(const row_t *row, const char *name);

#endif
