# ES-PET V0.8L RC7 release verification

Verified on 25 September 2026.

## Build

- PlatformIO Core 6.1.19
- Espressif 32 platform 7.0.1
- ESP-IDF 6.0.1
- Target: ESP32-C6
- Result: success
- App size: 1,511,168 bytes reported by the linker; 1,511,936-byte image
- App partition capacity: 3,145,728 bytes (`0x300000`)
- RAM: 45,404 / 327,680 bytes (13.9%)
- Flash: 1,511,168 / 3,145,728 bytes (48.0%)

The sole compiler warning is the pre-existing use of ESP-IDF's deprecated
single-cause sleep query. It is unrelated to the RC7 economy changes.

## Firmware image

- Image type: ESP32-C6
- Declared flash size: 4 MB
- Application version: `0.8L-RC7`
- Image checksum: valid
- Image validation hash: valid
- SHA-256: `3ca34dfe534ab2fd3923052ac4b7ff88118960d2bc746338cf4dc889eff9dae8`

## Behavioural tests

Host tests cover:

- 100-XP radio ceiling and partial final award;
- persistence across reboot;
- local-date rollover;
- carry-forward from pre-time-sync operation;
- all 15 daily mission reward tiers;
- perfect-day bonus transition rules;
- RC6 current-day mission migration: completed reward unchanged, mission type
  and progress unchanged, incomplete reward updated prospectively.

## Compatibility checks

- RC6 and RC7 partition records are identical.
- `LifeState`, encounter/diary and achievement persistence headers are
  byte-for-byte identical to RC6.
- Mission and challenge state structs retain the same fields and versions.
- The only new namespace is `trimecon`, containing version, local date and
  radio XP used today.
- Release binary contains the RC7 labels.
- Disabled detailed XP-source trace strings are absent from the binary.
