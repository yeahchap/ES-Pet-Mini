#!/usr/bin/env bash
set -euo pipefail

PIO="${PIO:-pio}"
if ! command -v "$PIO" >/dev/null 2>&1; then
  if [ -x "$HOME/.platformio/penv/bin/pio" ]; then
    PIO="$HOME/.platformio/penv/bin/pio"
  else
    echo "PlatformIO CLI not found."
    exit 1
  fi
fi

echo "Building ES-PET V0.8L RC7..."
"$PIO" run
