#include "factory_reset.h"

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

namespace {
const char *TAG = "trim_reset";

constexpr int64_t TAP_WINDOW_US = 8LL * 1000LL * 1000LL;
constexpr int64_t TAP_DEBOUNCE_US = 80LL * 1000LL;
constexpr int64_t CONFIRM_HOLD_US = 3LL * 1000LL * 1000LL;
constexpr int64_t ARM_TIMEOUT_US = 20LL * 1000LL * 1000LL;
constexpr uint8_t REQUIRED_TAPS = 10;

esp_err_t erase_namespace(const char *name) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(name, NVS_READWRITE, &h);
  if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
  if (err != ESP_OK) return err;

  err = nvs_erase_all(h);
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  return err;
}
}  // namespace

esp_err_t FactoryResetGesture::begin() {
  gpio_config_t cfg{};
  cfg.pin_bit_mask = (1ULL << static_cast<unsigned>(kButtonGpio));
  cfg.mode = GPIO_MODE_INPUT;
  cfg.pull_up_en = GPIO_PULLUP_ENABLE;
  cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  cfg.intr_type = GPIO_INTR_DISABLE;

  esp_err_t err = gpio_config(&cfg);
  if (err != ESP_OK) return err;

  last_level_ = gpio_get_level(kButtonGpio);

  BaseType_t ok = xTaskCreate(
      &FactoryResetGesture::task_entry_,
      "trim-reset-gesture",
      3072,
      this,
      4,
      nullptr);

  if (ok != pdPASS) return ESP_ERR_NO_MEM;

  ESP_LOGI(TAG,
           "Hidden factory reset armed: 10 BOOT taps/8s + 3s confirm hold");
  return ESP_OK;
}

void FactoryResetGesture::acknowledge_prompt() {
  confirmation_enabled_ = true;
  armed_since_us_ = esp_timer_get_time();
}

void FactoryResetGesture::note_wake_press() {
  wake_press_pending_ = true;
}

void FactoryResetGesture::task_entry_(void *arg) {
  static_cast<FactoryResetGesture *>(arg)->task_loop_();
}

void FactoryResetGesture::register_press_(int64_t now_us) {
  if (armed_) return;
  if (last_press_us_ != 0 &&
      (now_us - last_press_us_) < TAP_DEBOUNCE_US) {
    return;
  }

  if (tap_count_ == 0 ||
      window_started_us_ == 0 ||
      (now_us - window_started_us_) > TAP_WINDOW_US) {
    tap_count_ = 0;
    window_started_us_ = now_us;
  }

  last_press_us_ = now_us;
  ++tap_count_;

  ESP_LOGI(TAG, "BOOT service tap %u/%u",
           static_cast<unsigned>(tap_count_),
           static_cast<unsigned>(REQUIRED_TAPS));

  if (tap_count_ >= REQUIRED_TAPS &&
      (now_us - window_started_us_) <= TAP_WINDOW_US) {
    armed_ = true;
    armed_since_us_ = now_us;
    confirmation_needs_release_ = true;
    confirmation_enabled_ = false;
    hold_started_us_ = 0;
    ESP_LOGW(TAG, "FACTORY RESET ARMED - release, then hold BOOT 3 seconds");
  }
}

void FactoryResetGesture::disarm_() {
  armed_ = false;
  reset_requested_ = false;
  tap_count_ = 0;
  window_started_us_ = 0;
  armed_since_us_ = 0;
  hold_started_us_ = 0;
  confirmation_needs_release_ = false;
  confirmation_enabled_ = false;
}

void FactoryResetGesture::task_loop_() {
  while (true) {
    const int64_t now_us = esp_timer_get_time();
    const int level = gpio_get_level(kButtonGpio);

    const bool synthetic_wake_press = wake_press_pending_;
    if (synthetic_wake_press) {
      wake_press_pending_ = false;
      if (!armed_) register_press_(now_us);
      // Prevent the same still-held physical press being counted twice.
      last_level_ = 0;
    }

    if (!armed_) {
      if (!synthetic_wake_press && last_level_ == 1 && level == 0) {
        register_press_(now_us);
      }

      if (tap_count_ > 0 && window_started_us_ != 0 &&
          (now_us - window_started_us_) > TAP_WINDOW_US) {
        tap_count_ = 0;
        window_started_us_ = 0;
      }
    } else if (!reset_requested_) {
      if (!confirmation_enabled_) {
        // Wait for the e-paper warning screen to finish refreshing.
        hold_started_us_ = 0;
      } else if ((now_us - armed_since_us_) > ARM_TIMEOUT_US) {
        ESP_LOGI(TAG, "Factory reset confirmation timed out");
        disarm_();
      } else if (confirmation_needs_release_) {
        if (level == 1) {
          confirmation_needs_release_ = false;
          hold_started_us_ = 0;
        }
      } else if (level == 0) {
        if (hold_started_us_ == 0) hold_started_us_ = now_us;
        if ((now_us - hold_started_us_) >= CONFIRM_HOLD_US) {
          reset_requested_ = true;
          ESP_LOGW(TAG, "Factory reset CONFIRMED");
        }
      } else {
        hold_started_us_ = 0;
      }
    }

    last_level_ = level;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

esp_err_t erase_trimagotchi_user_data() {
  // Keep this list deliberately narrow. These are the namespaces owned by
  // Trimagotchi itself; no generic Wi-Fi/PHY/system NVS is erased.
  constexpr const char *NAMESPACES[] = {
      "trimlife",   // XP, identity, HOME fingerprint, friend caches
      "trimclock",  // first-run HOME Wi-Fi/name/timezone settings
      "trimgame",   // missions + achievements
      "trimdiary",  // daily counters + raw-diary indices
      "trimecon",   // RC7 daily radio-XP allowance
  };

  for (const char *name : NAMESPACES) {
    esp_err_t err = erase_namespace(name);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Could not erase NVS namespace '%s': %s",
               name, esp_err_to_name(err));
      return err;
    }
  }

  const esp_partition_t *diary = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA,
      ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
      "diary");

  if (!diary) {
    ESP_LOGE(TAG, "DIARY partition not found during factory reset");
    return ESP_ERR_NOT_FOUND;
  }

  esp_err_t err = esp_partition_erase_range(diary, 0, diary->size);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Could not erase DIARY partition: %s",
             esp_err_to_name(err));
    return err;
  }

  ESP_LOGW(TAG, "Trimagotchi user data erased; firmware preserved");
  return ESP_OK;
}
