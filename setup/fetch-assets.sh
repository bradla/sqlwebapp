#!/bin/sh
# Download the front-end assets csqlpage references (Tabler CSS + ApexCharts JS)
# into a directory the web server serves as SQLPAGE_ASSETS_BASE (default /assets).
#
#   sudo ./fetch-assets.sh /srv/http/assets
#
# Override versions with TABLER_VER / APEX_VER env vars.
set -e
DEST="${1:-/srv/http/assets}"
TABLER_VER="${TABLER_VER:-1.0.0}"
APEX_VER="${APEX_VER:-3.54.1}"
SDT_VER="${SDT_VER:-9.0.3}"   # simple-datatables (interactive tables)
LEAFLET_VER="${LEAFLET_VER:-1.9.4}"   # Leaflet (maps)

mkdir -p "$DEST"
curl -fsSL "https://cdn.jsdelivr.net/npm/@tabler/core@${TABLER_VER}/dist/css/tabler.min.css" \
     -o "$DEST/tabler.min.css"
curl -fsSL "https://cdn.jsdelivr.net/npm/apexcharts@${APEX_VER}/dist/apexcharts.min.js" \
     -o "$DEST/apexcharts.min.js"
curl -fsSL "https://cdn.jsdelivr.net/npm/simple-datatables@${SDT_VER}/dist/style.css" \
     -o "$DEST/simple-datatables.css"
curl -fsSL "https://cdn.jsdelivr.net/npm/simple-datatables@${SDT_VER}/dist/umd/simple-datatables.js" \
     -o "$DEST/simple-datatables.js"
curl -fsSL "https://cdn.jsdelivr.net/npm/leaflet@${LEAFLET_VER}/dist/leaflet.css" \
     -o "$DEST/leaflet.css"
curl -fsSL "https://cdn.jsdelivr.net/npm/leaflet@${LEAFLET_VER}/dist/leaflet.js" \
     -o "$DEST/leaflet.js"
echo "Wrote tabler.min.css, apexcharts.min.js, simple-datatables.{css,js}, leaflet.{css,js} to $DEST"
