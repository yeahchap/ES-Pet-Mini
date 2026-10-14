#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

struct ImuSample {
  float ax_mg = 0.0f, ay_mg = 0.0f, az_mg = 0.0f;
  float gx_dps = 0.0f, gy_dps = 0.0f, gz_dps = 0.0f;
  float accel_dynamic_mg = 0.0f;
  float gyro_magnitude_dps = 0.0f;
};

struct ImuMotionSnapshot {
  bool present = false;

  // In the Lloyd Mini this means the software step estimator + snooze
  // confirmation path is available. There is no MPU-6050 hardware pedometer.
  bool pedometer_ready = false;

  bool moving_now = false;
  bool moved_since_last_take = false;
  uint32_t moving_seconds_since_last_take = 0;
  uint32_t total_moving_seconds = 0;

  bool walking_now = false;
  bool walkies_since_last_take = false;
  uint32_t steps_since_last_take = 0;
  uint32_t steps_since_boot = 0;

  // Kept for compatibility with the main Trimagotchi telemetry. On this
  // build it is the low 16 bits of the software count, not an MPU register.
  uint16_t hardware_step_counter = 0;
  uint16_t recent_steps_15s = 0;

  ImuSample latest;
};

struct ImuSnoozePoll {
  bool ok = false;
  bool wake_event = false;
  uint8_t wake_source = 0;
  uint32_t new_steps = 0;
  uint32_t steps_since_boot = 0;
  uint16_t hardware_step_counter = 0;
};

class ImuSense {
 public:
  esp_err_t begin();
  bool present() const { return present_; }
  bool pedometer_ready() const { return pedometer_ready_; }

  esp_err_t start();
  ImuMotionSnapshot take_snapshot();

  // Lloyd Mini V0.7L.2 SNOOZE:
  // - fitted IMU answers at 0x68 and reports WHO_AM_I 0x70.
  // - accel samples are collected at 20 Hz in the IMU FIFO while the C6
  //   light-sleeps in short timer windows.
  // - software cadence detection turns FIFO samples into rough steps.
  // - GPIO14 INT stays wired/reserved for a later low-power WOM refinement.
  esp_err_t enter_snooze();
  ImuSnoozePoll poll_snooze();
  void latch_walkies_from_snooze();
  esp_err_t exit_snooze();

  static constexpr int wake_gpio() { return 14; }

 private:
  static constexpr uint32_t STEP_MIN_INTERVAL_US = 260000;
  static constexpr uint32_t STEP_MAX_INTERVAL_US = 1250000;
  static constexpr float STEP_HIGH_MG = 145.0f;
  static constexpr float STEP_RELEASE_MG = 70.0f;

  // V0.7L.4 field calibration:
  // Lloyd's first same-route comparison read about 11% high versus the
  // reference Trimagotchi. Preserve cadence detection, but only bank 90 of
  // every 100 detected step credits.
  static constexpr uint16_t STEP_CREDIT_NUM = 90;
  static constexpr uint16_t STEP_CREDIT_DEN = 100;

  static void task_entry_(void *arg);
  void task_loop_();

  esp_err_t pause_sampler_();
  void resume_sampler_();

  esp_err_t read_reg_(uint8_t reg, uint8_t &value);
  esp_err_t write_reg_(uint8_t reg, uint8_t value);
  esp_err_t read_regs_(uint8_t start_reg, uint8_t *data, size_t len);
  esp_err_t read_sample_(ImuSample &sample);

  esp_err_t configure_active_();
  esp_err_t configure_motion_cycle_();
  esp_err_t start_confirmation_fifo_();
  esp_err_t stop_fifo_();
  esp_err_t count_confirmation_fifo_(uint32_t &steps);
  esp_err_t clear_interrupt_(uint8_t &status);

  void process_active_step_locked_(float dynamic_mg, int64_t now_us);
  void note_step_locked_(int64_t when_us);
  uint16_t recent_steps_locked_(int64_t now_us, int64_t window_us) const;

  void *bus_ = nullptr;
  void *dev_ = nullptr;
  void *mutex_ = nullptr;
  void *task_handle_ = nullptr;

  bool present_ = false;
  bool started_ = false;
  bool pedometer_ready_ = false;
  bool snoozing_ = false;
  bool confirmation_fifo_active_ = false;

  volatile bool pause_requested_ = false;
  volatile bool task_paused_ = false;

  ImuSample latest_;

  bool moving_now_ = false;
  bool moved_since_take_ = false;
  uint32_t moving_ms_since_take_ = 0;
  uint32_t total_moving_ms_ = 0;
  int motion_score_ = 0;

  // Software cadence/step state.
  bool peak_high_ = false;
  int64_t last_peak_us_ = 0;
  uint8_t cadence_hits_ = 0;

  uint32_t steps_since_take_ = 0;
  uint32_t steps_since_boot_ = 0;
  uint16_t hardware_step_counter_ = 0;

  // Fractional calibration remainder. This changes only the credited step
  // count; raw cadence/peak detection remains exactly as before.
  uint16_t step_credit_accum_ = 0;
  bool walking_now_ = false;
  bool walkies_since_take_ = false;

  static constexpr size_t STEP_TIME_CAPACITY = 64;
  int64_t step_times_us_[STEP_TIME_CAPACITY] = {};
  size_t step_time_head_ = 0;
  size_t step_time_count_ = 0;
};
