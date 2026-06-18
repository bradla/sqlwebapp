/* row.c (C99) */
#include "row.h"
#include <string.h>

const char *row_get(const row_t *row, const char *name) {
    size_t i;
    for (i = 0; i < row->len; i++) {
        if (strcmp(row->cells[i].name, name) == 0)
            return row->cells[i].value; /* may be NULL */
    }
    return NULL;
}
