#!/usr/bin/env bash
# Runs the server in the foreground. Config: DATABASE_URL, JWT_SECRET, HOST, PORT.
# One worker thread = one PostgreSQL connection: 4 per core, at most 90 (max_connections is 100).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
THREADS="${SQLPAGE_THREADS:-$(( 4 * $(nproc) ))}"
[ "$THREADS" -gt 90 ] && THREADS=90
export SQLPAGE_THREADS="$THREADS"
export SQLPAGE_HTTP_LISTEN="${HOST:-127.0.0.1}:${PORT:-3000}"
export SQLPAGE_DATABASE_URL="${DATABASE_URL:?DATABASE_URL is required}"
export SQLPAGE_JWT_SECRET="${JWT_SECRET:?JWT_SECRET is required}"
export SQLPAGE_WEB_ROOT="$HERE/www"
export SQLPAGE_API_PREFIX=/api/
exec "$HERE/csqlpage"
