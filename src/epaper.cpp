#include "epaper.h"

#include "driver/gpio.h"
#include "esp_log.h"

namespace {

static const char *TAG = "trim_display";

constexpr gpio_num_t DISPLAY_SELECT_GPIO = GPIO_NUM_21;

}  // namespace


esp_err_t Epaper29Tri::begin() {
#if defined(ES_PET_ULTIMATE_XIAO)
  // XIAO GPIO21 is the display BUSY input, not the SuperMini profile switch.
  // Never configure it as a selector: this device only uses the 4-colour panel.
  four_colour_ = true;
  ESP_LOGI(TAG, "ULTIMATE XIAO: fixed JD79660 4-colour display");
  return four_.begin();
#else
  // GPIO21 is deliberately pulled HIGH:
  //   OPEN/HIGH = current four-colour Mini
  //   LOW/GND   = original monochrome Mini
  //
  // This keeps existing 4-colour hardware modification-free.
  gpio_config_t cfg{};
  cfg.intr_type = GPIO_INTR_DISABLE;
  cfg.mode = GPIO_MODE_INPUT;
  cfg.pin_bit_mask = (1ULL << DISPLAY_SELECT_GPIO);
  cfg.pull_up_en = GPIO_PULLUP_ENABLE;
  cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;

  const esp_err_t cfg_err = gpio_config(&cfg);

  if (cfg_err != ESP_OK) {
    // Optional/profile hardware must NEVER prevent ES-PET booting.
    // If selector configuration somehow fails, choose the current
    // four-colour hardware as the safe/default RC7 profile.
    four_colour_ = true;

    ESP_LOGW(TAG,
             "DISPLAY selector GPIO21 configuration failed (%s); "
             "defaulting to FOUR-COLOUR",
             esp_err_to_name(cfg_err));
  } else {
    four_colour_ = gpio_get_level(DISPLAY_SELECT_GPIO) != 0;
  }

  if (four_colour_) {
    ESP_LOGI(TAG,
             "RC7 DISPLAY PROFILE: FOUR-COLOUR "
             "(JD79660 / GDEM0154F51H) [GPIO21=HIGH]");
    return four_.begin();
  }

  ESP_LOGI(TAG,
           "RC7 DISPLAY PROFILE: B&W "
           "(SSD1681 / GDEH0154D67) [GPIO21=LOW]");
  return mono_.begin();
}

#endif
}


void Epaper29Tri::clear(InkColor color) {
  if (four_colour_) {
    four_.clear(color);
  } else {
    mono_.clear(color);
  }
}


void Epaper29Tri::set_pixel(int x, int y, InkColor color) {
  if (four_colour_) {
    four_.set_pixel(x, y, color);
  } else {
    // The proven monochrome driver renders any non-white ink as black.
    mono_.set_pixel(x, y, color);
  }
}


esp_err_t Epaper29Tri::refresh() {
  return four_colour_ ? four_.refresh() : mono_.refresh();
}


esp_err_t Epaper29Tri::sleep() {
  return four_colour_ ? four_.sleep() : mono_.sleep();
}
