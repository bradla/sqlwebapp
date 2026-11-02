/* json.h - minimal JSON validation and top-level member lookup (C99).
 *
 * Not a DOM: just enough to reject malformed request bodies before they reach
 * SQL (where a failed ::json cast would be a 500) and to read JWT claims.
 */
#ifndef CSQLPAGE_JSON_H
#define CSQLPAGE_JSON_H

#include <stddef.h>

/* True when s[0..n) is exactly one well-formed JSON value (RFC 8259). */
int json_valid(const char *s, size_t n);

/* Look up `key` in the top-level object s[0..n). Returns the value's type:
   's' string, 'n' number, 't' true, 'f' false, 'z' null, 'o' object, 'a'
   array, or 0 when s is not an object or has no such key (last duplicate
   wins). val and len receive the raw value text; for strings, the text between
   the quotes, still escaped. */
int json_member(const char *s, size_t n, const char *key,
                const char **val, size_t *len);

#endif
