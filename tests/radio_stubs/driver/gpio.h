#pragma once
#include <cstdint>
#include "esp_err.h"

enum gpio_num_t { GPIO_NUM_3 = 3, GPIO_NUM_14 = 14 };
enum gpio_mode_t { GPIO_MODE_OUTPUT = 2 };
enum gpio_pullup_t { GPIO_PULLUP_DISABLE = 0 };
enum gpio_pulldown_t { GPIO_PULLDOWN_DISABLE = 0 };
struct gpio_config_t {
  uint64_t pin_bit_mask = 0;
  gpio_mode_t mode{};
  gpio_pullup_t pull_up_en{};
  gpio_pulldown_t pull_down_en{};
};
esp_err_t gpio_config(const gpio_config_t *pins);
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level);
