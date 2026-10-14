#pragma once

#include <cstdint>

#include "driver/gpio.h"
#include "esp_err.h"

// Hidden recovery gesture for public/repeatable builds:
//   10 BOOT taps within 8 seconds -> armed
//   release, then hold BOOT for 3 seconds -> factory reset
//
// The BOOT button on the supported ESP32-C6 SuperMini is GPIO9. GPIO9 remains
// a normal input after bootstrapping has completed.
class FactoryResetGesture {
 public:
  static constexpr gpio_num_t kButtonGpio = GPIO_NUM_9;

  esp_err_t begin();

  bool armed() const { return armed_; }
  bool reset_requested() const { return reset_requested_; }

  // Called by the UI after the confirmation screen has actually refreshed.
  // The 3-second destructive hold is ignored until this acknowledgement, so
  // the user always sees the warning before reset can occur.
  void acknowledge_prompt();

  // Called when GPIO9 itself wakes light sleep. This makes the wake press
  // count as the first tap even if it was very quick.
  void note_wake_press();

 private:
  static void task_entry_(void *arg);
  void task_loop_();
  void register_press_(int64_t now_us);
  void disarm_();

  volatile bool armed_ = false;
  volatile bool reset_requested_ = false;
  volatile bool wake_press_pending_ = false;
  volatile bool confirmation_enabled_ = false;

  uint8_t tap_count_ = 0;
  int last_level_ = 1;
  bool confirmation_needs_release_ = false;

  int64_t window_started_us_ = 0;
  int64_t last_press_us_ = 0;
  int64_t armed_since_us_ = 0;
  int64_t hold_started_us_ = 0;
};

// Erases Trimagotchi-owned NVS namespaces and the raw DIARY partition only.
// Firmware, bootloader, partition table and PHY partition are untouched.
esp_err_t erase_trimagotchi_user_data();
