#!/bin/sh
# Builds and runs the host tests with the system C compiler. Run from the repo root.
# cJSON comes from the ESP-IDF that PlatformIO installed.
set -e
CJSON="${CJSON_DIR:-$HOME/.platformio/packages/framework-espidf/components/json/cJSON}"
OUT="${TMPDIR:-/tmp}/telltale_host_tests"
cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -O1 -g \
    -Isrc -I"$CJSON" \
    test/host/test_main.c src/config.c src/units.c src/sk_time.c src/widget_types.c src/alerts.c src/tides.c src/track.c src/history.c "$CJSON/cJSON.c" \
    -lm -o "$OUT"
"$OUT" data/default_config.json examples/*.json
