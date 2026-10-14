#include "battery.h"

#include <algorithm>

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

static const char *TAG = "trim_battery";

constexpr gpio_num_t BAT_GPIO = GPIO_NUM_0;
constexpr adc_unit_t BAT_UNIT = ADC_UNIT_1;
constexpr adc_channel_t BAT_CHANNEL = ADC_CHANNEL_0;

// Hardware divider:
// LiPo+ --- 470k --- ADC --- 220k --- GND
//
// Vadc = Vbat * 220 / (470 + 220)
// Vbat = Vadc * 690 / 220
constexpr int R_TOP_K = 470;
constexpr int R_BOTTOM_K = 220;

// Our divider produces roughly 1.05-1.34V over the useful LiPo range.
// 6dB gives plenty of headroom with better resolution than maximum attenuation.
constexpr adc_atten_t BAT_ATTEN = ADC_ATTEN_DB_12;

adc_oneshot_unit_handle_t s_adc = nullptr;
adc_cali_handle_t s_cali = nullptr;

bool s_adc_ready = false;
bool s_calibrated = false;
bool s_fitted = false;


// Provisional single-cell LiPo curve.
// We'll tune this against the real ES-PET batteries later.
int voltage_to_percent(int mv) {
  struct Point {
    int mv;
    int pct;
  };

  static constexpr Point curve[] = {
      {3300,   0},
      {3400,   5},
      {3550,  10},
      {3650,  20},
      {3700,  30},
      {3750,  40},
      {3800,  50},
      {3850,  60},
      {3920,  70},
      {4000,  80},
      {4100,  90},
      {4200, 100},
  };

  if (mv <= curve[0].mv) return curve[0].pct;

  const int last = static_cast<int>(sizeof(curve) / sizeof(curve[0])) - 1;
  if (mv >= curve[last].mv) return curve[last].pct;

  for (int i = 1; i <= last; ++i) {
    if (mv <= curve[i].mv) {
      const int x0 = curve[i - 1].mv;
      const int y0 = curve[i - 1].pct;
      const int x1 = curve[i].mv;
      const int y1 = curve[i].pct;

      return y0 + ((mv - x0) * (y1 - y0)) / (x1 - x0);
    }
  }

  return 0;
}


int read_adc_mv() {
  if (!s_adc_ready) return 0;

  constexpr int SAMPLE_COUNT = 1;

  int64_t total_raw = 0;

  for (int i = 0; i < SAMPLE_COUNT; ++i) {
    int raw = 0;

    if (adc_oneshot_read(s_adc, BAT_CHANNEL, &raw) != ESP_OK) {
      return 0;
    }

    total_raw += raw;
  }

  const int raw_avg = static_cast<int>(total_raw / SAMPLE_COUNT);

  ESP_LOGI(TAG, "ADC DEBUG: raw_avg=%d", raw_avg);
  ESP_LOGI(TAG, "ADC DEBUG: raw_avg=%d", raw_avg);

  if (s_calibrated) {
    int mv = 0;

    if (adc_cali_raw_to_voltage(s_cali, raw_avg, &mv) == ESP_OK) {
      return mv;
    }
  }

  // Belt-and-braces fallback if calibration is unavailable.
  // Approximate 6dB full-scale range for ESP32-C6.
  return (raw_avg * 1900) / 4095;
}

}  // namespace


esp_err_t battery_init() {
  // ----------------------------------------------------------
  // Presence detection
  //
  // Briefly discharge GPIO0 before handing it to the ADC.
  //
  // With our 470k/220k + 100nF sensor attached, the node then
  // charges back toward ~1.1-1.35V.
  //
  // With nothing attached, GPIO0 should remain near zero rather
  // than producing a random floating-pin "battery".
  // ----------------------------------------------------------

  // gpio_reset_pin(BAT_GPIO);
  // gpio_set_direction(BAT_GPIO, GPIO_MODE_OUTPUT);
  // gpio_set_level(BAT_GPIO, 0);

  // vTaskDelay(pdMS_TO_TICKS(20));

  // gpio_set_direction(BAT_GPIO, GPIO_MODE_INPUT);
  // gpio_set_pull_mode(BAT_GPIO, GPIO_FLOATING);

  // Divider Thévenin resistance is ~150k.
  // With 100nF, tau is ~15ms; 100ms is comfortably settled.
  vTaskDelay(pdMS_TO_TICKS(100));


  adc_oneshot_unit_init_cfg_t unit_cfg{};
  unit_cfg.unit_id = BAT_UNIT;

  esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc);

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "ADC unavailable: %s", esp_err_to_name(err));
    return err;
  }


  adc_oneshot_chan_cfg_t chan_cfg{};
  chan_cfg.atten = BAT_ATTEN;
  chan_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;

  err = adc_oneshot_config_channel(s_adc, BAT_CHANNEL, &chan_cfg);

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "ADC channel configuration failed: %s",
             esp_err_to_name(err));
    return err;
  }

  s_adc_ready = true;


#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
  adc_cali_curve_fitting_config_t cal_cfg{};
  cal_cfg.unit_id = BAT_UNIT;
  cal_cfg.chan = BAT_CHANNEL;
  cal_cfg.atten = BAT_ATTEN;
  cal_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;

  if (adc_cali_create_scheme_curve_fitting(&cal_cfg, &s_cali) == ESP_OK) {
    s_calibrated = true;
    ESP_LOGI(TAG, "ADC curve-fitting calibration ready");
  } else {
    ESP_LOGW(TAG,
             "ADC calibration unavailable; using raw-voltage fallback");
  }
#endif


  const int adc_mv = read_adc_mv();

  // A real ES-PET divider should be around 1.0-1.35V.
  // Keep the acceptance window deliberately broad.
  s_fitted = (adc_mv >= 700 && adc_mv <= 1550);

  if (!s_fitted) {
    ESP_LOGI(TAG,
             "Battery monitor not fitted (ADC=%dmV); continuing normally",
             adc_mv);
    return ESP_OK;
  }

  const int battery_mv =
      (adc_mv * (R_TOP_K + R_BOTTOM_K)) / R_BOTTOM_K;

  ESP_LOGI(TAG,
           "Battery monitor detected: ADC=%dmV BAT=%dmV SOC~%d%%",
           adc_mv,
           battery_mv,
           voltage_to_percent(battery_mv));

  return ESP_OK;
}


BatteryReading battery_read() {
  BatteryReading result{};

  if (!s_adc_ready || !s_fitted) {
    return result;
  }

  const int adc_mv = read_adc_mv();

  if (adc_mv <= 0) {
    return result;
  }

  const int battery_mv =
      (adc_mv * (R_TOP_K + R_BOTTOM_K)) / R_BOTTOM_K;

  result.fitted = true;
  result.adc_millivolts = adc_mv;
  result.millivolts = battery_mv;
  result.percent = std::clamp(voltage_to_percent(battery_mv), 0, 100);

  return result;
}


bool battery_available() {
  return s_adc_ready && s_fitted;
}
