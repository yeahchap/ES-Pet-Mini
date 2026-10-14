# ES-PET Mini 4C RC5 LONG GAME

RC5 LONG GAME is deliberately based on the locked, proven Mini 4C RC4 tree.

## What changes

- Keeps the original 8 achievements unchanged.
- Adds 16 long-term / prestige achievements, for 24 total.
- Adds a badge-schema migration key (`badgever`, schema v2).
- Existing RC4 pets silently seed any prestige badges they already qualify for on first RC5 boot: no retroactive XP and no stack of celebration screens.
- Future prestige badges unlock normally and award XP.
- Social prestige uses the existing distinct peer count; THE PACK means this pet + two known peers.
- 30-day and 100-day walking badges reuse the already-persistent best walking streak from the RC3.4 challenge engine.

## What does NOT change

- No artwork or visible layout changes. The on-screen version label remains `V0.8L RC3.4`.
- No e-paper driver/timing changes. RC4 JD79660 reliability code is untouched.
- No MPU/WALKIES/SNOOZE/PERCHED changes.
- No Wi-Fi, BLE scanning, HOME, ADVENTURE or DISCOVERY logic changes.
- No `LifeState`, peer-book, diary or partition layout changes.
- No OTA or patch partition is introduced in RC5.

## Prestige badges

| Badge | Requirement | XP |
|---|---:|---:|
| CENTURY CLUB | 100,000 lifetime steps | 150 |
| SERIOUSLY?! | 250,000 lifetime steps | 250 |
| TINY LEGS BIG MILES | 500,000 lifetime steps | 500 |
| SEASONED EXPLORER | 10 completed adventures | 150 |
| WELL TRAVELLED | 25 completed adventures | 250 |
| OLD HAND | 50 completed adventures | 400 |
| SIGNAL HUNTER | 500 lifetime Wi-Fi identities | 200 |
| RADIO MAGPIE | 500 lifetime BLE identities | 200 |
| THE COLLECTOR | 1,000 combined Wi-Fi + BLE identities | 350 |
| THE PACK | know 2 other ES-PETs (3-pet pack including self) | 250 |
| SOCIAL CIRCLE | know 5 other ES-PETs | 350 |
| POPULAR LITTLE THING | know 10 other ES-PETs | 500 |
| VETERAN | reach level 40 | 300 |
| LEGEND | reach level 50 | 500 |
| CREATURE OF HABIT | best 30-day qualifying walk streak | 300 |
| UNSTOPPABLE | best 100-day qualifying walk streak | 750 |

## First flash safety

Before flashing RC5 to a real pet, take a full 4 MiB backup. Use the normal app-only update at `0x010000`; do not erase flash. This preserves NVS and DIARY.
