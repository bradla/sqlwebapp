/* map.h - simple ordered string->string multimap (C99) */
#ifndef CSQLPAGE_MAP_H
#define CSQLPAGE_MAP_H

#include <stddef.h>

typedef struct {
    char *key;
    char *val;
} map_entry_t;

typedef struct {
    map_entry_t *items;
    size_t       len;
    size_t       cap;
} map_t;

void        map_init(map_t *m);
void        map_free(map_t *m);
/* Takes ownership of nothing; copies key and val. */
void        map_add(map_t *m, const char *key, const char *val);
/* Returns last value for key, or NULL. Case-sensitive. */
const char *map_get(const map_t *m, const char *key);

#endif
