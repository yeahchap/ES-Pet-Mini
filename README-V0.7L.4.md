# Trimagotchi V0.7L.4 — Lloyd Step Calibration

Calibration-only patch following the first same-route field test.

Observed rounded field values:

```text
Reference Trimagotchi: ~3.5K steps
Lloyd Mini:            ~3.9K steps
```

That is roughly an 11% high read on Lloyd's software pedometer.

## Change

The peak/cadence detector is **not** changed.

Instead, detected step credits are multiplied by:

```text
0.90
```

Implemented as an integer fractional accumulator:

```text
90 / 100
```

so there is no floating-point drift and short walks do not repeatedly lose
fractional steps through truncation.

Example:

```text
10 detected cadence steps -> 9 banked steps
100 detected             -> 90 banked steps
1000 detected            -> 900 banked steps
```

WALKIES recognition remains based on the existing cadence logic; this patch is
only intended to make TODAY / ALL TIME step totals better match the reference
unit.

Existing historical totals are left untouched. Calibration applies to new
steps from this firmware onward.

## Build

```bash
cd ~/trimagotchi-v0.7L.4-step-calibration
rm -rf .pio sdkconfig sdkconfig.old
pio run
```

## App-only flash

```bash
~/.platformio/penv/bin/python \
  ~/.platformio/packages/tool-esptoolpy/esptool.py \
  --chip esp32c6 \
  -p /dev/ttyACM0 \
  -b 460800 \
  --before no_reset \
  --after no_reset \
  write_flash \
  --flash_size 4MB \
  0x010000 .pio/build/trimagotchi-c6/firmware.bin
```

Do not erase flash.
