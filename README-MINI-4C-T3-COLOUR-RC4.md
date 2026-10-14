# ES-PET Mini 4C T3 Colour RC4

RC4 is a surgical display-timing/reliability update from RC3. Visuals, game logic, MPU/step logic, WALKIES, Wi-Fi, BLE, NVS and diary behaviour are unchanged.

## Root cause fixed

This ESP-IDF build uses `CONFIG_FREERTOS_HZ=100`, so one FreeRTOS tick is 10 ms. The old display helper used `vTaskDelay(pdMS_TO_TICKS(ms))` even for 1 ms and 2 ms hardware timings. Both converted to **zero ticks**.

That meant:

- the intended 2 ms JD79660 RESET-low pulse could be effectively zero-length, so a panel already in deep sleep did not reliably wake;
- the intended 1 ms BUSY grace period could also be zero;
- RC2/RC3 then correctly noticed that refresh had not started, but treated the timeout as fatal and rebooted ES-PET.

RC4 uses `esp_rom_delay_us()` for sub-tick hardware delays, so the 1 ms/2 ms timings really happen.

## Reliability changes

- Real 2 ms reset-low pulse after panel deep sleep.
- Real 1 ms BUSY grace timing.
- 100 ms BUSY assertion window after refresh command.
- One full reset/init/rewrite recovery retry remains.
- A final display refresh failure is non-fatal: ES-PET continues running and retries a redraw later instead of rebooting.
- Successful updates still power off and deep-sleep the e-paper for battery use.

## Expected healthy refresh

```
Refreshing 200x200 FOUR-COLOUR display
Refresh waveform started; BUSY=0 after ... ms
... about 20 seconds ...
Four-colour display refresh complete
Four-colour e-paper sleeping
```

The important test is several consecutive redraws after sleep/WALKIES. There should be no crash/reboot loop.
