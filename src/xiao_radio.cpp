#include "xiao_radio.h"

#if defined(ES_PET_ULTIMATE_XIAO)
#include "driver/gpio.h"
#include "esp_log.h"
#endif

esp_err_t xiao_radio_begin() {
#if defined(ES_PET_ULTIMATE_XIAO)
  // Seeed XIAO ESP32-C6 schematic: GPIO3 drives the RF-switch power PMOS;
  // LOW enables it. GPIO14 LOW selects the fitted ceramic antenna. These
  // internal board pins are separate from XIAO D3 (GPIO21 / display BUSY).
  gpio_config_t pins{};
  pins.pin_bit_mask = (1ULL << GPIO_NUM_3) | (1ULL << GPIO_NUM_14);
  pins.mode = GPIO_MODE_OUTPUT;
  pins.pull_up_en = GPIO_PULLUP_DISABLE;
  pins.pull_down_en = GPIO_PULLDOWN_DISABLE;
  esp_err_t err = gpio_config(&pins);
  if (err != ESP_OK) return err;
  err = gpio_set_level(GPIO_NUM_14, 0);
  if (err != ESP_OK) return err;
  err = gpio_set_level(GPIO_NUM_3, 0);
  if (err != ESP_OK) return err;
  ESP_LOGI("trim_wifi", "XIAO RF switch enabled: GPIO3=0 GPIO14=0 (onboard antenna)");
#endif
  return ESP_OK;
}
