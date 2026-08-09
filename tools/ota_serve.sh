#!/bin/bash
# Build a freshly-stamped dev image and serve it for the watch's OTA loop.
# Usage: tools/ota_serve.sh [port]   (default 8281; ctrl-c stops the server)
set -e
cd "$(dirname "$0")/../device"
PORT="${1:-8281}"
IP="$(ipconfig getifaddr en0)"
# PROJECT_VER is read at CMake configure time, so force a reconfigure —
# a plain incremental build would keep the previous stamp.
export PROJECT_VER="dev-$(date +%Y%m%d-%H%M%S)"
export FROLIC_OTA_URL="http://${IP}:${PORT}"
export FROLIC_OTA_FAST=1
source "$HOME/esp/esp-idf/export.sh" >/dev/null 2>&1
idf.py reconfigure >/dev/null
idf.py build
# version.txt must be the version actually embedded in the binary
# (esp_app_desc_t sits at offset 48 in the image).
python3 - <<'EOF'
with open('build/frolic.bin', 'rb') as f:
    f.seek(48)
    version = f.read(32).split(b'\0')[0].decode()
open('build/version.txt', 'w').write(version + '\n')
print('serving version:', version)
EOF
echo "OTA at ${FROLIC_OTA_URL}"
python3 -m http.server "$PORT" --directory build
