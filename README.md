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

Named-parameter handling differs by backend: SQLite parses `$name`/`:name`
itself; for PostgreSQL (which only accepts positional `$1`) csqlpage splits the
script and rewrites `$name`/`:name` into `$1..$N` (see `sqlparse.c`), correctly
skipping string literals, quoted identifiers, `--`/`/* */` comments, `::` casts
and `$tag$…$tag$` dollar-quoted bodies. The PostgreSQL URL is passed to libpq
verbatim, so any libpq connection URI/option works.

The PostgreSQL connection is **cached for the life of the process**. Under
FastCGI (resident process) this means one connection is reused across all
requests instead of reconnecting each time. A connection dropped between
requests (server restart, idle timeout) is detected and transparently
re-established; any transaction left open by a script is rolled back before the
connection is reused, so session state never leaks between requests.

### Components implemented
`shell` / `shell-empty`, `text`, `table` (optionally interactive), `list`,
`card`, `form`, `chart` (ApexCharts), `map` (Leaflet), and the header
components `status_code`, `http_header`, `redirect`. Styled with Tabler — see
[Styling & charts](#styling--charts).

## Build

Requires a C99 compiler and SQLite dev headers (`libsqlite3-dev` / `sqlite3`).
PostgreSQL support additionally needs libpq dev headers (`libpq-dev` /
`postgresql-libs`) and `pkg-config`.

```sh
make                       # SQLite only (default) -> ./csqlpage
make PG=1                  # also enable the PostgreSQL backend
```

Run `make clean` when switching between the two — Make rebuilds on file
timestamps, not on changed flags, so it won't recompile just because `PG=1`
was added or dropped.

## Try it locally (no web server)

```sh
# create the demo database
sqlite3 sqlpage.db 'CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT, email TEXT);
  INSERT INTO users(name,email) VALUES ("Ada","ada@example.com"),("Linus","linus@example.com");'

# simulate a CGI invocation
SQLPAGE_WEB_ROOT=www SQLPAGE_DATABASE_URL=sqlite://sqlpage.db \
  PATH_INFO=/index.sql QUERY_STRING=name=Ada REQUEST_METHOD=GET ./csqlpage
```

### PostgreSQL

```sh
make PG=1
SQLPAGE_WEB_ROOT=www \
  SQLPAGE_DATABASE_URL='postgresql://user:pass@localhost/mydb' \
  PATH_INFO=/index.sql QUERY_STRING=name=Ada REQUEST_METHOD=GET ./csqlpage
# unix socket + peer auth: 'postgresql:///mydb?host=/run/postgresql'
```
In Apache, set the same URL via `SetEnv SQLPAGE_DATABASE_URL …`.

## Styling & charts

Pages are styled with [Tabler](https://tabler.io) and the `chart` component
renders with [ApexCharts](https://apexcharts.com) — the same libraries SQLPage
uses. csqlpage only emits the `<link>`/`<script>` tags and Tabler class names;
the front-end web server serves the actual asset bytes (it's far better at that
than the app would be). Fetch them once into a directory it serves:

```sh
sudo ./deploy/fetch-assets.sh /srv/http/assets   # tabler.min.css + apexcharts.min.js
```

Then expose that directory and tell csqlpage where it lives. Apache:
```apache
Alias /assets /srv/http/assets
<Directory "/srv/http/assets">
    Require all granted
</Directory>
```
The asset URL prefix defaults to `/assets`; override with `SQLPAGE_ASSETS_BASE`
(e.g. a CDN base, or a different mount). nginx serves it with a plain
`location /assets/ { root /srv/http; }`.

```apache
<VirtualHost *:80>
    ServerName app.example.com

    ProxyPass        "/app/" "fcgi://127.0.0.1:9000/"
    ProxyPassReverse "/app/" "fcgi://127.0.0.1:9000/"
    Alias /assets /srv/http/assets
    <Directory "/srv/http/assets">
        Require all granted
    </Directory>

    # Pass these into every CGI invocation.
    SetEnv SQLPAGE_WEB_ROOT     /srv/http/sqlpages
    #SetEnv SQLPAGE_DATABASE_URL sqlite:///srv/http/csqlpage/app.db
    SetEnv SQLPAGE_DATABASE_URL postgresql://username:password@localhost/mydb

    # Map the URL prefix /app to the csqlpage binary.
    # A request to /app/users.sql  ->  PATH_INFO=/users.sql
    # A request to /app  or  /app/ ->  PATH_INFO empty -> index.sql
    # Enable for just cgi
    #ScriptAlias /app /srv/http/cgi-bin/csqlpage

    #<Directory "/srv/http/cgi-bin">
    #   # AllowOverride None
    #    Options +ExecCGI
    #    #AddHandler cgi-script .py
    #    Require all granted
    #</Directory>

    # Optional: make the app the site root instead of living under /app.
    # ScriptAlias / /var/www/cgi-bin/csqlpage/
    # (note the trailing slash on both sides for the root form)

    ErrorLog  /var/log/httpd/csqlpage_error.log
    CustomLog /var/log/httpd/csqlpage_access.log combined
</VirtualHost>

```
