#pragma once

#include "esp_err.h"

struct BatteryReading {
  bool fitted = false;
  int millivolts = 0;      // Estimated LiPo voltage
  int percent = 0;         // Provisional LiPo SOC estimate
  int adc_millivolts = 0;  // Voltage actually measured at GPIO0
};

esp_err_t battery_init();
BatteryReading battery_read();
bool battery_available();
