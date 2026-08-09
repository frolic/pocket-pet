#!/bin/bash
# Build a freshly-stamped dev image and serve it for the watch's OTA loop.
# Usage: tools/ota_serve.sh [port]   (default 8281; ctrl-c stops the server)
set -e
cd "$(dirname "$0")/../device"
PORT="${1:-8281}"
IP="$(ipconfig getifaddr en0)"
export PROJECT_VER="dev-$(date +%Y%m%d-%H%M%S)"
export FROLIC_OTA_URL="http://${IP}:${PORT}"
source "$HOME/esp/esp-idf/export.sh" >/dev/null 2>&1
idf.py build
echo "$PROJECT_VER" > build/version.txt
echo "serving $PROJECT_VER at $FROLIC_OTA_URL"
python3 -m http.server "$PORT" --directory build
