# 04 — Ultimate Personal ES-PET Mini (XIAO ESP32-C6)

**Status: firmware port committed for BENCH TEST; not yet built, flashed or validated.**
The production/known-good RC7 remains on \`main\`. Do not replace the working
personal pet until we have a verified backup and restoration procedure.

## Hardware: proposed pinout for tomorrow's loom

Only **one** 1.54-inch **200x200 FOUR-COLOUR** e-paper (JD79660/GDEM0154F51H).
This is not the Nano OLED and is not the larger next-gen ES-PET AI display.

| XIAO silk | C6 GPIO | Connection |
|---|---:|---|
| D0 | 0 | Battery sense input from 470k/220k divider, if fitted |
| D1 | 1 | 12mm **passive** piezo disc via 120 ohm series resistor to GND |
| D2 | 2 | LIS2DW12 INT1 (active high, latched; wake source) |
| D3 | 21 | e-paper BUSY |
| D4 | 22 | LIS2DW12 SDA |
| D5 | 23 | LIS2DW12 SCL |
| D6 | 16 | e-paper DC |
| D7 | 17 | e-paper CS |
| D8 | 19 | e-paper SCLK |
| D9 | 20 | e-paper RST |
| D10 | 18 | e-paper DIN/MOSI |
| 3V3 | — | e-paper VCC and LIS breakout VCC **only if rated for 3.3V** |
| GND | — | common display, IMU, piezo and battery-divider ground |

Piezo: GPIO1 -> 120 ohm -> ceramic disc -> GND. **Only passive ceramic piezo**;
do not attach 4-ohm speakers or magnetic buzzers directly to the GPIO.
GPIO1 LEDC: 3800Hz, 50% duty, nominal 100ms chirp.

LIS2DW12 address auto-detect: 0x19 (SA0 high) or 0x18 (SA0 low);
WHO_AM_I must return 0x44. The *exact breakout* must be checked for pull-ups,
supply and whether its INT1 pad is available. If not, do not permanently solder.

The XIAO has an onboard USB-C charging circuit. Battery +/- connect to the
board's **battery pads only** per Seeed instructions, not GPIO/3V3 or 5V.
Keep the USB/charger wiring arrangement separate from any Qi hardware used
in the different next-gen AI project. Battery-current suitability must be
checked against the particular cell's specification.

## Build

\`\`\`sh
pio run -e ultimate-xiao-c6
\`\`\`

Keep the original build intact:

\`\`\`sh
pio run -e trimagotchi-c6
\`\`\`

- Compile flag: \`ES_PET_ULTIMATE_XIAO\` is required on this target.
- Four-colour profile is fixed: **never use GPIO21 as profile selector**,
  because on XIAO it is the BUSY input.
- LED handling is board specific (XIAO GPIO15, not SuperMini GPIO8).
- Existing 200x200 graphics and game/NVS/diary formats retained.
- Classic MPU driver is excluded from the XIAO target, original RC7 unaffected.
- Raw display refresh automatically once at the *first game epoch after boot*,
  and then only after a recognised LIS2DW12 double tap. Initial time-setup
  and destructive factory-reset warnings can still refresh independently.
- Background radio scans now target 15-minute intervals initially. This
  is a starting point for consumption tests, NOT a measured optimum.
- Full four-colour JD79660 refresh can take many seconds: a double tap
  does not make the physics of a full pigment refresh instantaneous.
- A short piezo chirp replaces the classic motor gate.

## Functional boundary (important)

**The LIS2DW12 has no hardware all-day pedometer.** The preliminary driver
samples acceleration for candidate steps while the MCU is awake. Long
light-sleep intervals will **miss steps**. We deliberately do not fabricate
step counts from a motion interrupt or incomplete FIFO history. Tomorrow's
walking/step-counting comparison is a gate before calling this an RC7-equivalent
replacement. A more aggressive motion-active sampling window / sleep policy
will be needed to preserve the walking gameplay while saving battery.

**Power**: double-tap hardware recognition uses 400 Hz high-performance
accelerometer operation. This costs more than LIS low-power motion-only
detection. Radio scanning, active sampling and refresh current need real
measurements. No multi-day runtime estimate is yet justified.

**Restore**: preserve Pet ID, XP, friend books, challenges, badges, HOME,
journeys, mission state, lifetime steps and diary. Backup first and restore
only the data partitions, not the source device's bootloader/app/partition
table, when a validated migration method is ready.

Current partition layout from \`partitions.csv\`:
- NVS: 0x009000 length 0x006000.
- patch: 0x310000 length 0x002000.
- diary: 0x312000 length 0x0EE000.

*Do not copy these blindly or factory-reset the original.* Confirm the
source pet uses the expected RC7 partition map and that its NVS data can
be restored on the new board before performing any write.

## Bring-up gates

1. Build both environments (original and Ultimate), fix compile errors.
2. USB power only: detect LIS2DW12 0x44 and verify GPIO2/INT1 idle/wake.
3. Confirm four-colour panel refresh, BUSY polarity and sleep retention.
4. Bench-test 3.8kHz 100ms passive piezo chirp.
5. Confirm double-tap refresh once, no screen refresh on ordinary scans.
6. Check motion detection and walking against original on same route.
7. Measure sleep + active scan + full refresh battery currents; then tune.
8. Backup original pet, verify restoration on a spare state and switch over.
9. Only retire original when personal Mini has matching Pet ID/progression.

Sources: Seeed XIAO ESP32-C6 official pin map; ST LIS2DW12 datasheet and
ST DT0101 (single/double tap); ES-PET-Mini RC7 existing 4-colour display driver.
