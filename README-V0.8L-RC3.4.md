# Trimagotchi V0.8L RC3.4 — MINI FINAL (Lloyd 1.54")

This is the V0.8 large-screen game/mission release amalgamated into the proven
Lloyd Mini hardware branch.

## Hardware retained from V0.7L.4

- ESP32-C6 SuperMini, 4 MB flash
- WeAct 1.54-inch 200x200 monochrome e-paper
- display wiring unchanged: CLK4, MOSI6, CS7, DC3, RST2, BUSY5
- MPU-compatible IMU at I2C 0x68, observed WHO_AM_I 0x70
- SDA20 / SCL19 / INT14
- calibrated 90% software-step credit from V0.7L.4
- four-second MPU FIFO light-sleep step watcher
- provisional TODAY steps survive clock-unsynced operation and are merged after HOME sync

No hardware rewiring is required.

## V0.8 features brought across

- novelty XP: Wi-Fi 4/2/0, Bluetooth 2/1/0 for new-ever / first-today / repeat-today
- PERCHED after five stationary away SCANs
- missions remain visible while HOME, WALKIES, ADVENTURE, SNOOZING and PERCHED
- 15 daily mission types
- date-driven walking and adventure streaks
- 3/5/7/14/30-day walking awards
- 3/5/7-day adventure streak awards
- 5-in-7, weekend pair, perfect week, 15/25-day month challenges
- challenge XP + diary events
- hidden factory reset: BOOT x10 within 8s, release, then hold BOOT 3s
- RC3 anime/chibi bitmap scene set and proper raster fonts/icons

## Mini graphics adaptation

The square 200x200 UI is deliberately redrawn rather than shrinking the 128x296
portrait layout. The 108x80 character art remains at native resolution.

Because Lloyd's panel is monochrome, the large unit's red accent bitmap plane is
converted into a 50% checker dither. This keeps cheeks/scarves/shading/event
accents without turning them into solid black patches.

Layout:

- top: pet scene left, readable state/metrics card right
- middle: full-width mission + streak card
- bottom: larger FRIENDS / BADGES / XP icons and text
- event screens use centred native artwork + full-width result card

## MINI PERCHED note

The fitted 0x70 MPU-compatible clone has no trusted hardware wake-on-motion path.
PERCHED therefore keeps Wi-Fi/BLE off but wakes the C6 in four-second light-sleep
slices to poll the IMU FIFO. It still exits immediately on confirmed walking,
but will not reach the same stationary current as the LSM6DSOX large version.

## Upgrade

This build keeps the existing 4 MB BIG BRAIN partition layout, NVS keys, Pet ID,
HOME profile, friend books, lifetime counts and diary.

Normal update is app-only at `0x010000`. Do not erase flash.

```bash
cd ~/trimagotchi-v0.8L-rc3.4-mini-final
rm -rf .pio sdkconfig sdkconfig.old
pio run
```

After a clean build, flash app-only with the same C6 no-reset esptool method used
for V0.7L.4.
