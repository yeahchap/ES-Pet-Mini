# ES-PET Mini 4C RC3.4 Test 2

Test 2 fixes JD79660 refresh timing.

The GDEM0154F51H / JD79660 BUSY line is active LOW, but it does not assert
immediately after command 0x12. Test 1 sampled BUSY too soon, could see HIGH,
and then power the panel off before the four-colour refresh waveform started.

Test 2 follows the proven GxEPD2 sequence: send 0x12 + 0x00, wait 1 ms, then
wait for BUSY to return HIGH. Serial now logs `Refresh waveform started; BUSY=...`.
On a healthy panel BUSY should normally be 0 at that point and the refresh
should take many seconds rather than completing in the same log timestamp.

No pet logic, radio logic, MPU logic, UI layout, GPIO assignments, NVS layout,
or artwork has been changed from Test 1.
