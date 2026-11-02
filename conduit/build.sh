#!/usr/bin/env bash
# Builds csqlpage with the PostgreSQL backend -> ./csqlpage. Idempotent.
set -euo pipefail
cd "$(dirname "$0")"
make PG=1
test -x csqlpage
echo "built csqlpage"
