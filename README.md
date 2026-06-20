## What it does

- Parses a CGI request (env + stdin): `PATH_INFO`, `QUERY_STRING`, form body, cookies.
- Routes `PATH_INFO` to a `.sql` file under the web root (`/` → `index.sql`).
- Runs every statement in that file against **SQLite** or **PostgreSQL**,
  chosen by the `SQLPAGE_DATABASE_URL` scheme.
- Named parameters in SQL (`$name`, `:name`) are bound from request params.
- Each result row whose `component` column is set selects a component and
  supplies its top-level props; following rows are data rows for it.
- Output is wrapped in a default HTML `shell`.

### Database backends
| URL scheme | Backend | Library |
|---|---|---|
| `sqlite://<path>` or a bare path | SQLite | `libsqlite3` (always) |
| `postgres://…` / `postgresql://…` | PostgreSQL | `libpq` (build with `PG=1`) |
