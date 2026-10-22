# 04 — Ultimate Personal ES-PET Mini (XIAO ESP32-C6)

**Status: LSM6DS3 port compiled with ESP-IDF 6.0.1; physical bench validation pending.**
The production/known-good RC7 remains on `main`. Do not replace the working
personal pet until we have a verified backup and restoration procedure.

## Hardware: proposed pinout for tomorrow's loom

Only **one** 1.54-inch **200x200 FOUR-COLOUR** e-paper (JD79660/GDEM0154F51H).
This is not the Nano OLED and is not the larger next-gen ES-PET AI display.

| XIAO silk | C6 GPIO | Connection |
|---|---:|---|
| D0 | 0 | Battery sense input from 470k/220k divider, if fitted |
| D1 | 1 | 12mm **passive** piezo disc via 120 ohm series resistor to GND |
| D2 | 2 | LSM6DS3 INT1 (active high, latched; wake source) |
| D3 | 21 | e-paper BUSY |
| D4 | 22 | LSM6DS3 SDA |
| D5 | 23 | LSM6DS3 SCL |
| D6 | 16 | e-paper DC |
| D7 | 17 | e-paper CS |
| D8 | 19 | e-paper SCLK |
| D9 | 20 | e-paper RST |
| D10 | 18 | e-paper DIN/MOSI |
| 3V3 | — | e-paper VCC and LSM6DS3 breakout VCC **if rated for 3.3V** |
| GND | — | common display, IMU, piezo and battery-divider ground |

Piezo: GPIO1 -> 120 ohm -> ceramic disc -> GND. **Only passive ceramic piezo**;
do not attach 4-ohm speakers or magnetic buzzers directly to the GPIO.
GPIO1 LEDC: 3800Hz, 50% duty, nominal 100ms chirp.

LSM6DS3 address auto-detect: **0x6A (SA0 low) or 0x6B (SA0 high)**;
WHO_AM_I must return **0x69**. CS must be high for I2C. If these pins are exposed,
connect CS to the 3.3V I/O rail and SDO/SA0 to GND for 0x6A; existing breakout
straps can stay because both addresses are checked. INT2 is unused.
Keep the existing SDA/SCL pull-ups to 3V3 for initial bring-up.
Use the exact module's labels, not the old LIS board's physical pad locations.
This driver is for LSM6DS3, not an unverified LSM6DS3TR-C/LSM6DS33/LSM6DSL.

The XIAO has an onboard USB-C charging circuit. Battery +/- connect to the
board's **battery pads only** per Seeed instructions, not GPIO/3V3 or 5V.
Keep the USB/charger wiring arrangement separate from any Qi hardware used
in the different next-gen AI project. Battery-current suitability must be
checked against the particular cell's specification.

## Build

```sh
pio run -e ultimate-xiao-c6
```

Keep the original build intact:

```sh
pio run -e trimagotchi-c6
```

- Compile flag: `ES_PET_ULTIMATE_XIAO` is required on this target.
- Four-colour profile is fixed: **never use GPIO21 as profile selector**,
  because on XIAO it is the BUSY input.
- LED handling is board specific (XIAO GPIO15, not SuperMini GPIO8).
- Existing 200x200 graphics and game/NVS/diary formats retained.
- Classic MPU driver is excluded from the XIAO target, original RC7 unaffected.
- Raw display refresh automatically once at the *first game epoch after boot*,
  and then only after a recognised LSM6DS3 double tap. Initial time-setup
  and destructive factory-reset warnings can still refresh independently.
- Background radio scans now target 15-minute intervals initially. This
  is a starting point for consumption tests, NOT a measured optimum.
- Full four-colour JD79660 refresh can take many seconds: a double tap
  does not make the physics of a full pigment refresh instantaneous.
- A short piezo chirp replaces the classic motor gate.

## Functional boundary (important)

**The LSM6DS3 hardware pedometer is explicitly enabled.** Its 16-bit counter
continues while the XIAO light-sleeps. Software polls it at most every minute
and handles counter rollover; awake reads, sleep reads and the game snapshot
share one counter baseline so steps are credited once. Only sensor startup
resets the volatile hardware count; lifetime steps, XP and NVS formats do not
change. Implausible counter jumps are rebased without awarding phantom steps.
Hardware detection and step accuracy still need the physical bench test.

The accelerometer stays at **416 Hz** for tap recognition; the gyroscope stays
OFF. Pedometer and double-tap bits coexist and are checked by register readback.
Only double tap is routed to INT1: ordinary steps do not cause a wake interrupt
or screen refresh. Four new hardware steps observed at a timer poll can still
trigger the existing WALKIES early radio scan. Precise moving-duration and
15-second cadence cannot be recovered from a batched hardware counter; they
are not fabricated. The existing extended PERCHED policy remains disabled on
this bench target. No measured runtime claim is made.

**Restore**: preserve Pet ID, XP, friend books, challenges, badges, HOME,
journeys, mission state, lifetime steps and diary. Backup first and restore
only the data partitions, not the source device's bootloader/app/partition
table, when a validated migration method is ready.

Current partition layout from `partitions.csv`:
- NVS: 0x009000 length 0x006000.
- patch: 0x310000 length 0x002000.
- diary: 0x312000 length 0x0EE000.

*Do not copy these blindly or factory-reset the original.* Confirm the
source pet uses the expected RC7 partition map and that its NVS data can
be restored on the new board before performing any write.

## Bring-up gates

1. Build both environments (original and Ultimate), fix compile errors.
2. USB power only: detect LSM6DS3 WHO=0x69 and verify GPIO2/INT1 idle/wake.
3. Confirm four-colour panel refresh, BUSY polarity and sleep retention.
4. Bench-test 3.8kHz 100ms passive piezo chirp.
5. Confirm double-tap refresh once, no screen refresh on ordinary scans.
6. Check motion detection and walking against original on same route.
7. Measure sleep + active scan + full refresh battery currents; then tune.
8. Backup original pet, verify restoration on a spare state and switch over.
9. Only retire original when personal Mini has matching Pet ID/progression.

Sources: Seeed XIAO ESP32-C6 official pin map; ST LSM6DS3 DS026899 Rev 10;
ST platform driver at https://github.com/STMicroelectronics/lsm6ds3-pid
(commit 1b0caf4bf522c7ae33b7f00b23fad531e58d5e90, BSD-3-Clause);
ES-PET-Mini RC7 existing 4-colour display driver.

## Host regression checks

`python3 tests/run_lsm6ds3_host_tests.py` checks the actual ST driver against a
register-map bus: pedometer/tap coexistence, gyro off, INT1 routing, register
bank restoration, error propagation, exactly-once step delivery and rollover.
These checks and both PlatformIO environments also run in GitHub CI. They do
not substitute for a physical walking, double-tap and sleep-wake test.
