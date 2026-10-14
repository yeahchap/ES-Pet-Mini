# RC6 to RC7 — safe app-only upgrade

Use this process separately for each pet. Replace `PORT` with the device port
and give every backup a unique name.

## 1. Record and back up

Photograph the current status screen, including XP, level, friends and badges.
Then make a complete 4 MB recovery image:

```bash
esptool.py --chip esp32c6 --port PORT read_flash \
  0x000000 0x400000 es-pet-before-rc7-DEVICE.bin
sha256sum es-pet-before-rc7-DEVICE.bin
```

Keep this backup until the pet has run RC7 successfully for at least a day.

## 2. Build and verify

```bash
rm -rf .pio sdkconfig sdkconfig.old
pio run -e trimagotchi-c6
tests/run_host_tests.sh
sha256sum .pio/build/trimagotchi-c6/firmware.bin
```

Confirm that `firmware.bin` is no larger than `0x300000` bytes.

## 3. Flash the application only

Put the C6 into its usual download mode, then write exactly one image at the
app offset:

```bash
esptool.py --chip esp32c6 --port PORT --baud 460800 \
  write_flash 0x010000 .pio/build/trimagotchi-c6/firmware.bin
```

Do **not** run `erase_flash`. Do **not** include bootloader, partition-table,
NVS, patch or diary images in this RC6-to-RC7 command.

## 4. Acceptance check

After reboot, verify all of the following before updating the next pet:

- header says `V0.8L RC7`;
- the same Pet ID, HOME and known-friend totals remain;
- old XP is present (it may only rise through normal RC7 rewards);
- badge, journey and lifetime-step totals remain;
- the correct display profile appears and the full screen refreshes cleanly;
- the IMU adds steps when walked;
- Wi-Fi and Bluetooth scans still complete;
- today's missions retain their choices/progress; incomplete ones show the
  new RC7 reward when completed;
- no boot loop, repeated factory-reset prompt or NVS error appears.

Recommended order: spare/sacrificial C6, your pet, then your friend's pet.
Keep each device under observation for several scan cycles before moving on.

## Recovery

If a device cannot pass the acceptance check, stop and do not flash the other
pet. Restore that device's own 4 MB backup only:

```bash
esptool.py --chip esp32c6 --port PORT --baud 460800 \
  write_flash 0x000000 es-pet-before-rc7-DEVICE.bin
```

Never restore one pet's full backup onto another: it contains that pet's
identity, HOME data, diary and progress.

## Fresh blank C6

A new/sacrificial board has no life data to preserve, so it may receive the
complete PlatformIO image set on first installation. Once provisioned, use the
same app-only rule for later updates.
