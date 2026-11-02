/* sqlfn.h - SQLPage-style built-in functions, evaluated by the app (C99).
 *
 * A call such as sqlpage.hash_password($password) in a statement is replaced
 * by a bound parameter (see sqlparse.c); its value is computed here, in the
 * app, just before the statement runs. Arguments are $name / :name request
 * parameters, 'string literals', or NULL.
 *
 *   sqlpage.request_body()          raw request body, or NULL when empty
 *   sqlpage.request_json()          the body if it is well-formed JSON, else NULL
 *   sqlpage.user_id()               subject of the "Authorization: Token <jwt>"
 *                                   header; NULL without one; aborts 401 if invalid
 *   sqlpage.require_user_id()       the same, but also aborts 401 without one
 *   sqlpage.token()                 the presented token
 *   sqlpage.jwt(sub)                a fresh HS256 token for sub (30 days)
 *   sqlpage.hash_password(pw)       bcrypt hash, cost 10
 *   sqlpage.verify_password(pw, h)  'true' / 'false'
 */
#ifndef CSQLPAGE_SQLFN_H
#define CSQLPAGE_SQLFN_H

#include "cgi.h"

/* True if s starts a "sqlpage.<ident>(" call. */
int sqlfn_is_call(const char *s);

/* Length of the call text starting at s, through the closing ')', or 0 when
   it is malformed. Skips ')' inside string literals. */
size_t sqlfn_call_len(const char *s);

/* Evaluate `call` (the text as written). On success returns 0 and stores the
   result in *out (malloc'd, or NULL for SQL NULL). On failure returns the
   HTTP status the request should end with (401 for authentication, 500 for
   a bad call) and stores a message in *errmsg (malloc'd). */
int sqlfn_eval(const char *call, cgi_request_t *req, char **out, char **errmsg);

#endif
