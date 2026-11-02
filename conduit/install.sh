#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: a C compiler and the libraries csqlpage links
# (libpq for PostgreSQL, libcrypt/libxcrypt for bcrypt, libsqlite3 for its SQLite backend).
set -euo pipefail
apt-get update -q
DEBIAN_FRONTEND=noninteractive apt-get install -y -q gcc make pkg-config libc6-dev \
  libpq-dev libcrypt-dev libsqlite3-dev
cc --version | head -1
pkg-config --modversion libpq
