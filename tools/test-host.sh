#!/usr/bin/env bash
# Host-side tests for the platform independent VK core and feed parser.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${TMPDIR:-/tmp}/vk_core_test"
OUT2="${TMPDIR:-/tmp}/vk_feed_test"

cc -std=c11 -Wall -Wextra -O1 -g \
   -I"$ROOT/src" \
   "$ROOT/src/vk.c" \
   "$ROOT/src/vendor/cJSON.c" \
   "$ROOT/tests/test_vk.c" \
   -lm -o "$OUT"
"$OUT"

cc -std=c11 -Wall -Wextra -O1 -g \
   -I"$ROOT/src" \
   "$ROOT/src/feed.c" \
   "$ROOT/src/log.c" \
   "$ROOT/src/history.c" \
   "$ROOT/src/vendor/cJSON.c" \
   "$ROOT/tests/test_feed.c" \
   -lm -o "$OUT2"
"$OUT2"
