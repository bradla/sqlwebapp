# C: csqlpage (SQL-file endpoints) + libpq

**Front: nginx**

The Conduit API written as `.sql` files and served by [csqlpage](https://github.com/), a small C99 clone of
[SQLPage](https://sql-page.com): each endpoint is one SQL script under `www/`, and PostgreSQL builds the JSON
response itself (`json_build_object` / `json_agg`). The C side is a generic runtime: an HTTP server, a router,
JWT/bcrypt functions callable from SQL, and a libpq executor.

| Piece | Version |
|---|---|
| Language | C99, gcc (Ubuntu 24.04) |
| HTTP | built in: HTTP/1.1, keep-alive, epoll worker threads (`src/http.c`) |
| Postgres | libpq (`libpq-dev`, Ubuntu 24.04), named prepared statements |
| JWT | built in: HS256 with a dependency-free SHA-256/HMAC (`src/sha256.c`, `src/jwt.c`) |
| bcrypt | libxcrypt `crypt_rn` / `crypt_gensalt_rn`, `$2b$`, cost 10 (`libcrypt-dev`) |

## How an endpoint looks

Routes are files: `www/api/articles/[slug]/comments.post.sql` serves `POST /api/articles/:slug/comments` and
binds `$slug`. A script is a list of statements. The first row with `component = 'json'` is the whole response
(its `status` and `contents`) and ends the script, so a script reads as guards followed by the happy path:

```sql
SET viewer = sqlpage.require_user_id();          -- 401 unless a valid token (checked in C, no query)
SET body   = sqlpage.request_json();             -- the body if it is well-formed JSON, else NULL

SELECT 'json' AS component, 422 AS status, '{"errors":{"body":["..."]}}' AS contents
WHERE NOT coalesce((SELECT json_typeof(c -> 'body') = 'string' AND c ->> 'body' <> ''
                      FROM (SELECT $body::json -> 'comment' AS c) s), false);

WITH c AS (INSERT INTO comments (article_id, author_id, body) SELECT a.id, $viewer::bigint, ... RETURNING ...)
SELECT 'json' AS component, 201 AS status, json_build_object('comment', ...) AS contents FROM c JOIN users u ...;

SELECT 'json' AS component, 404 AS status, '{"errors":{"body":["article not found"]}}' AS contents;
```

`SET x = expr` stores a value as `$x` for later statements; when the value is a lone `sqlpage.*()` call it is
computed in the app without a query. A unique violation (SQLSTATE class 23) answers 422.

## Multicore model

One process with `SQLPAGE_THREADS` worker threads (4 per core, capped at 90), each owning one PostgreSQL
connection. All workers wait on one epoll instance with `EPOLLONESHOT` registrations and take one ready
connection at a time, so an idle worker always picks up the next request: a slow request (a bcrypt login)
delays only its own connection. A worker blocks on Postgres while it serves a request; the thread count is the
concurrency. (An earlier version gave each thread its own epoll and connections; one slow request then stalled
every connection queued on that thread, and p99 was 1.5 s at 3,000 local users instead of 2 ms.)

## Optimizations

- Every statement is a named prepared statement, prepared once per connection: Postgres parses and plans each
  statement once instead of per request. The parsed scripts and the route tree are loaded at startup.
- Responses are built in Postgres in one round trip where possible: reads (article, comments, profile, tags)
  are one statement; the list and the feed are a validation guard plus one statement whose filter branches are
  one-time filters on parameters, so only the applicable branch runs.
- Fallback statements (404, 401) come after the happy path, so they only run when it returned nothing.
- No per-request logging; idle connections are never closed by the server (Nginx closes first).
