/* map.c (C99) */
#include "map.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

void map_init(map_t *m) {
    m->cap   = 8;
    m->len   = 0;
    m->items = xmalloc(m->cap * sizeof(*m->items));
}

void map_free(map_t *m) {
    size_t i;
    for (i = 0; i < m->len; i++) {
        free(m->items[i].key);
        free(m->items[i].val);
    }
    free(m->items);
    m->items = NULL;
    m->len = m->cap = 0;
}

void map_add(map_t *m, const char *key, const char *val) {
    if (m->len == m->cap) {
        m->cap *= 2;
        m->items = xrealloc(m->items, m->cap * sizeof(*m->items));
    }
    m->items[m->len].key = xstrdup(key);
    m->items[m->len].val = xstrdup(val ? val : "");
    m->len++;
}

const char *map_get(const map_t *m, const char *key) {
    size_t i;
    /* iterate backwards: last value wins (matches typical form semantics) */
    for (i = m->len; i-- > 0;) {
        if (strcmp(m->items[i].key, key) == 0) return m->items[i].val;
    }
    return NULL;
}
