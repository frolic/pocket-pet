#!/bin/bash
# ./flash.sh [debug|release] [port]  — builds and flashes the watch.
set -e
cd "$(dirname "$0")"
MODE="${1:-release}"
PORT="${2:-/dev/cu.usbmodem2101}"
source "$HOME/esp/esp-idf/export.sh" >/dev/null 2>&1
if [ "$MODE" = "debug" ]; then
    rm -f sdkconfig
    FROLIC_DEBUG=1 idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.debug" build
else
    rm -f sdkconfig
    idf.py build
fi
idf.py -p "$PORT" flash
