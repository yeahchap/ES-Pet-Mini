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

echo "RC7 host tests passed"
