#pragma once

// -----------------------------------------------------------------------------
// Trimagotchi V0.7 Lloyd Mini
// ESP32-C6 SuperMini + WeAct Studio 1.54" 200x200 BLACK/WHITE/RED/YELLOW e-paper.
//
// Wiring intentionally mirrors the main Trimagotchi where practical:
//   EPD SCLK -> GPIO4
//   EPD MOSI -> GPIO6
//   EPD CS   -> GPIO7
//   EPD DC   -> GPIO3
//   EPD RST  -> GPIO2
//   EPD BUSY -> GPIO5
//
// MPU-6050:
//   SDA -> GPIO20
//   SCL -> GPIO19
//   INT -> GPIO14
// -----------------------------------------------------------------------------

#if defined(ES_PET_ULTIMATE_XIAO)
// Seeed XIAO ESP32-C6: D8=19, D10=18, D7=17, D6=16, D9=20, D3=21.
// GPIO2/D2 is reserved for the LIS2DW12 INT1 (deep-sleep capable).
#define TRIM_EPD_PIN_SCLK 19
#define TRIM_EPD_PIN_MOSI 18
#define TRIM_EPD_PIN_CS   17
#define TRIM_EPD_PIN_DC   16
#define TRIM_EPD_PIN_RST  20
#define TRIM_EPD_PIN_BUSY 21
#else
#define TRIM_EPD_PIN_SCLK   4
#define TRIM_EPD_PIN_MOSI   6
#define TRIM_EPD_PIN_CS     7
#define TRIM_EPD_PIN_DC     3
#define TRIM_EPD_PIN_RST    2
#define TRIM_EPD_PIN_BUSY   5
#endif

#define TRIM_EPD_PHYS_WIDTH   200
#define TRIM_EPD_PHYS_HEIGHT  200

#define TRIM_SCREEN_WIDTH     200
#define TRIM_SCREEN_HEIGHT    200

// Square panel, so rotation is a mounting preference rather than a geometry
// requirement. Flip this to 1 if the finished case mounts the panel upside down.
#define TRIM_EPD_ROTATE_180   0
