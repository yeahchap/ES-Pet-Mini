#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

COMMON=(
  -std=c++17
  -Wall
  -Wextra
  -Werror
  -I"$ROOT/tests/stubs"
  -I"$ROOT/include"
)

g++ "${COMMON[@]}" \
  "$ROOT/tests/economy_test.cpp" \
  "$ROOT/tests/nvs_fake.cpp" \
  "$ROOT/src/economy.cpp" \
  -o "$OUT/economy_test"
"$OUT/economy_test"

g++ "${COMMON[@]}" \
  "$ROOT/tests/mission_rewards_test.cpp" \
  "$ROOT/tests/nvs_fake.cpp" \
  "$ROOT/src/mission.cpp" \
  -o "$OUT/mission_rewards_test"
"$OUT/mission_rewards_test"

g++ "${COMMON[@]}" \
  "$ROOT/tests/clock_sync_host_test.cpp" \
  -o "$OUT/clock_sync_test"
"$OUT/clock_sync_test"

for TARGET in classic ultimate; do
  FLAGS=()
  if [[ "$TARGET" == ultimate ]]; then FLAGS+=(-DES_PET_ULTIMATE_XIAO=1); fi
  g++ "${COMMON[@]}" "${FLAGS[@]}" -I"$ROOT/tests/radio_stubs" \
    "$ROOT/tests/xiao_radio_host_test.cpp" "$ROOT/src/xiao_radio.cpp" \
    -o "$OUT/radio_$TARGET"
  "$OUT/radio_$TARGET"
done

echo "RC7, clock and antenna host tests passed"
