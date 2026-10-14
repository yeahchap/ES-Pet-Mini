# ES-PET Mini — 1.54-inch Four-Colour Test 1

This is a **display-only hardware port** of the existing Mini RC3.4 release for the
WeAct / Good Display **GDEM0154F51H 200x200 four-colour panel (JD79660)**.

## Deliberately unchanged

- ESP32-C6 target and 4 MB partition layout
- Mini UI geometry and 200x200 layout
- HOME / clock / Explorer / missions / badges / diary behaviour
- MPU-compatible motion code, including acceptance of WHO_AM_I `0x68` and `0x70`
- Display wiring:
  - SCL / CLK -> GPIO4
  - SDA / MOSI -> GPIO6
  - CS -> GPIO7
  - D/C -> GPIO3
  - RES / RST -> GPIO2
  - BUSY -> GPIO5
- MPU wiring:
  - SDA -> GPIO20
  - SCL -> GPIO19
  - INT -> GPIO14
  - AD0 -> GND (address 0x68)

## Changed

- PlatformIO Espressif32 is restored to **7.0.1 / ESP-IDF 6.0.1**, matching the now-proven C6 HOME Wi-Fi runtime. The safe project folder name contains no `-L` token.
- E-paper backend is now native **2 bits/pixel** for the JD79660.
- Native pigment codes are BLACK, WHITE, YELLOW and RED.
- BUSY handling is changed to the JD79660 active-low behaviour.
- Full refresh uses the GDEM0154F51H controller profile and allows up to 30 seconds.
- As an immediate colour sanity check, the existing black header keeps its exact
  geometry but renders **ES-PET in yellow** and the **version in red**.
- Existing pet artwork remains the proven monochrome artwork for this first panel
  smoke test. Colourised pet PNG masters can be dropped in once the panel itself
  has been proven on the real hardware.

## First upload recommendation

This is a **new spare C6**, so use the normal new-board upload route after building.
Do not use this test build on the finished Large unit.

The controller command profile was implemented from the public GDEM0154F51H /
JD79660 support in the GxEPD2 project and the WeAct example selection for the same
panel. ES-PET continues to use its native ESP-IDF SPI layer; it does not add Arduino
or GxEPD2 as a dependency.
