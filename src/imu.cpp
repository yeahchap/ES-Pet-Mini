#include "imu.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "trim_imu_mpu6x";

namespace {

constexpr gpio_num_t SCL_PIN = GPIO_NUM_19;
constexpr gpio_num_t SDA_PIN = GPIO_NUM_20;
constexpr gpio_num_t INT_PIN = GPIO_NUM_14;

constexpr uint8_t ADDR = 0x68;

constexpr uint8_t REG_SMPLRT_DIV      = 0x19;
constexpr uint8_t REG_CONFIG          = 0x1A;
constexpr uint8_t REG_GYRO_CONFIG     = 0x1B;
constexpr uint8_t REG_ACCEL_CONFIG    = 0x1C;
constexpr uint8_t REG_MOT_THR         = 0x1F;
constexpr uint8_t REG_MOT_DUR         = 0x20;
constexpr uint8_t REG_FIFO_EN         = 0x23;
constexpr uint8_t REG_INT_PIN_CFG     = 0x37;
constexpr uint8_t REG_INT_ENABLE      = 0x38;
constexpr uint8_t REG_INT_STATUS      = 0x3A;
constexpr uint8_t REG_ACCEL_XOUT_H    = 0x3B;
constexpr uint8_t REG_MOT_DETECT_CTRL = 0x69;
constexpr uint8_t REG_USER_CTRL       = 0x6A;
constexpr uint8_t REG_PWR_MGMT_1      = 0x6B;
constexpr uint8_t REG_PWR_MGMT_2      = 0x6C;
constexpr uint8_t REG_FIFO_COUNTH     = 0x72;
constexpr uint8_t REG_FIFO_R_W        = 0x74;
constexpr uint8_t REG_WHO_AM_I        = 0x75;

constexpr uint8_t WHO_MPU6050 = 0x68;
constexpr uint8_t WHO_MPU6X_COMPAT = 0x70;
constexpr uint8_t INT_MOTION = 0x40;

// 20 Hz accelerometer stream: with DLPF enabled, base sample rate is 1 kHz;
// divider 49 gives 20 Hz.
constexpr uint8_t SAMPLE_DIV_20HZ = 49;

// +/-2g = 16384 LSB/g.
constexpr float ACCEL_MG_PER_LSB = 1000.0f / 16384.0f;

int16_t be16(const uint8_t *p) {
  return static_cast<int16_t>(
      (static_cast<uint16_t>(p[0]) << 8) |
       static_cast<uint16_t>(p[1]));
}

float accel_dynamic_mg_from_raw(const uint8_t *raw) {
  const float ax = be16(&raw[0]) * ACCEL_MG_PER_LSB;
  const float ay = be16(&raw[2]) * ACCEL_MG_PER_LSB;
  const float az = be16(&raw[4]) * ACCEL_MG_PER_LSB;

  const float mag = std::sqrt(ax * ax + ay * ay + az * az);
  return std::fabs(mag - 1000.0f);
}

}  // namespace

esp_err_t ImuSense::begin() {
  ESP_LOGI(TAG,
           "Initialising MPU-6x compatible IMU: "
           "SDA=GPIO20 SCL=GPIO19 INT=GPIO14 addr=0x68");

  i2c_master_bus_config_t bus_cfg{};
  bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
  bus_cfg.i2c_port = I2C_NUM_0;
  bus_cfg.scl_io_num = SCL_PIN;
  bus_cfg.sda_io_num = SDA_PIN;
  bus_cfg.glitch_ignore_cnt = 7;
  bus_cfg.flags.enable_internal_pullup = true;

  i2c_master_bus_handle_t bus = nullptr;
  esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "I2C bus creation failed: %s", esp_err_to_name(err));
    return err;
  }
  bus_ = bus;

  i2c_device_config_t dev_cfg{};
  dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dev_cfg.device_address = ADDR;
  dev_cfg.scl_speed_hz = 400000;

  i2c_master_dev_handle_t dev = nullptr;
  err = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Could not add MPU-compatible IMU at 0x68: %s",
             esp_err_to_name(err));
    return err;
  }
  dev_ = dev;

  uint8_t who = 0;
  err = read_reg_(REG_WHO_AM_I, who);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "No response from MPU-compatible IMU at 0x68: %s",
             esp_err_to_name(err));
    return err;
  }

  ESP_LOGI(TAG, "WHO_AM_I = 0x%02X", who);

  // The fitted Lloyd board was sold as MPU-6050 hardware, but the dedicated
  // probe measured a stable WHO_AM_I 0x70 at the normal 0x68 I2C address and
  // proved that live accelerometer data is valid. Accept both identities.
  if (who != WHO_MPU6050 && who != WHO_MPU6X_COMPAT) {
    ESP_LOGE(TAG,
             "Unsupported MPU-compatible identity 0x%02X "
             "(accepted: 0x68 or 0x70)",
             who);
    return ESP_ERR_INVALID_RESPONSE;
  }

  ESP_LOGI(TAG,
           "IMU identity accepted: %s",
           who == WHO_MPU6050 ? "0x68 classic" : "0x70 compatible");

  // Device reset, then wake on internal oscillator. The gyro remains in
  // standby in this build: software steps only need the accelerometer.
  ESP_ERROR_CHECK(write_reg_(REG_PWR_MGMT_1, 0x80));
  vTaskDelay(pdMS_TO_TICKS(100));

  ESP_ERROR_CHECK(configure_active_());

  gpio_config_t int_cfg{};
  int_cfg.pin_bit_mask = 1ULL << INT_PIN;
  int_cfg.mode = GPIO_MODE_INPUT;
  int_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
  int_cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
  int_cfg.intr_type = GPIO_INTR_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&int_cfg));

  mutex_ = xSemaphoreCreateMutex();
  if (!mutex_) return ESP_ERR_NO_MEM;

  uint8_t stale = 0;
  (void)clear_interrupt_(stale);

  vTaskDelay(pdMS_TO_TICKS(60));

  ImuSample first;
  ESP_ERROR_CHECK(read_sample_(first));
  latest_ = first;

  present_ = true;
  pedometer_ready_ = true;

  ESP_LOGI(TAG,
           "MPU ALIVE: A[%.0f %.0f %.0f]mg dyn=%.0fmg",
           first.ax_mg, first.ay_mg, first.az_mg,
           first.accel_dynamic_mg);
  ESP_LOGI(TAG,
           "Software pedometer READY: 20Hz accel + cadence filter; "
           "SNOOZE uses short FIFO/timer windows");
  ESP_LOGI(TAG,
           "Step calibration: %u%% credited (field factor %.2f)",
           static_cast<unsigned>(STEP_CREDIT_NUM),
           static_cast<double>(STEP_CREDIT_NUM) /
               static_cast<double>(STEP_CREDIT_DEN));

  return ESP_OK;
}

esp_err_t ImuSense::configure_active_() {
  ESP_ERROR_CHECK(stop_fifo_());

  // Awake, internal oscillator.
  ESP_ERROR_CHECK(write_reg_(REG_PWR_MGMT_1, 0x00));

  // LP_WAKE bits clear; gyro axes in standby, accelerometer axes enabled.
  ESP_ERROR_CHECK(write_reg_(REG_PWR_MGMT_2, 0x07));

  // DLPF around 44Hz, 20Hz output, +/-2g.
  ESP_ERROR_CHECK(write_reg_(REG_CONFIG, 0x03));
  ESP_ERROR_CHECK(write_reg_(REG_SMPLRT_DIV, SAMPLE_DIV_20HZ));
  ESP_ERROR_CHECK(write_reg_(REG_GYRO_CONFIG, 0x00));
  ESP_ERROR_CHECK(write_reg_(REG_ACCEL_CONFIG, 0x00));

  // Interrupt disabled while the host is awake.
  ESP_ERROR_CHECK(write_reg_(REG_INT_ENABLE, 0x00));
  ESP_ERROR_CHECK(write_reg_(REG_INT_PIN_CFG, 0x20));  // latched until status read

  return ESP_OK;
}

esp_err_t ImuSense::configure_motion_cycle_() {
  ESP_ERROR_CHECK(stop_fifo_());

  // Motion detector uses a small high-pass component in the accel path.
  ESP_ERROR_CHECK(write_reg_(REG_ACCEL_CONFIG, 0x01));

  // MPU-6050 motion threshold is intentionally conservative here. This is
  // merely a wake trigger; WALKIES still needs a cadence in the FIFO.
  ESP_ERROR_CHECK(write_reg_(REG_MOT_THR, 40));
  ESP_ERROR_CHECK(write_reg_(REG_MOT_DUR, 20));
  ESP_ERROR_CHECK(write_reg_(REG_MOT_DETECT_CTRL, 0x15));
  ESP_ERROR_CHECK(write_reg_(REG_INT_PIN_CFG, 0x20));
  ESP_ERROR_CHECK(write_reg_(REG_INT_ENABLE, INT_MOTION));

  // CYCLE=1. LP_WAKE_CTRL=2 selects the useful mid-rate low-power accel
  // wake cadence on MPU-6050; gyro axes remain in standby.
  ESP_ERROR_CHECK(write_reg_(REG_PWR_MGMT_2, 0x87));
  ESP_ERROR_CHECK(write_reg_(REG_PWR_MGMT_1, 0x20));

  return ESP_OK;
}

esp_err_t ImuSense::start() {
  if (!present_) return ESP_ERR_INVALID_STATE;
  if (started_) return ESP_OK;

  TaskHandle_t handle = nullptr;
  if (xTaskCreate(task_entry_, "trim_mpu", 4096, this, 4, &handle) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }

  task_handle_ = handle;
  started_ = true;
  ESP_LOGI(TAG, "20Hz MPU software-step sampler started");
  return ESP_OK;
}

void ImuSense::task_entry_(void *arg) {
  static_cast<ImuSense *>(arg)->task_loop_();
}

void ImuSense::task_loop_() {
  TickType_t last = xTaskGetTickCount();
  uint32_t log_div = 0;

  while (true) {
    if (pause_requested_) {
      task_paused_ = true;
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      task_paused_ = false;
      last = xTaskGetTickCount();
      log_div = 0;
      continue;
    }

    ImuSample s;
    if (read_sample_(s) == ESP_OK) {
      const bool active = s.accel_dynamic_mg >= 80.0f;

      auto m = static_cast<SemaphoreHandle_t>(mutex_);
      if (xSemaphoreTake(m, pdMS_TO_TICKS(20)) == pdTRUE) {
        latest_ = s;

        motion_score_ = active ? std::min(40, motion_score_ + 3)
                               : std::max(0, motion_score_ - 1);

        if (!moving_now_ && motion_score_ >= 7) moving_now_ = true;
        if (moving_now_ && motion_score_ <= 2) moving_now_ = false;

        if (active || moving_now_) {
          moved_since_take_ = true;
          moving_ms_since_take_ += 50;
          total_moving_ms_ += 50;
        }

        const int64_t now_us = esp_timer_get_time();
        process_active_step_locked_(s.accel_dynamic_mg, now_us);

        // Stop displaying WALKING after cadence has been absent for a while.
        if (last_peak_us_ == 0 ||
            (now_us - last_peak_us_) > 2500000LL) {
          walking_now_ = false;
        }

        xSemaphoreGive(m);
      }

      if (++log_div >= 100) {
        log_div = 0;

        uint32_t boot_steps = 0;
        uint16_t recent = 0;
        bool walking = false;

        auto m2 = static_cast<SemaphoreHandle_t>(mutex_);
        if (xSemaphoreTake(m2, pdMS_TO_TICKS(20)) == pdTRUE) {
          boot_steps = steps_since_boot_;
          recent = recent_steps_locked_(
              esp_timer_get_time(), 15000000LL);
          walking = walking_now_;
          xSemaphoreGive(m2);
        }

        ESP_LOGI(TAG,
                 "MPU RAW=%s WALK=%s steps_boot=%lu recent15=%u "
                 "A[%.0f %.0f %.0f]mg dyn=%.0fmg INT=%d",
                 moving_now_ ? "MOVING" : "STILL",
                 walking ? "YES" : "NO",
                 static_cast<unsigned long>(boot_steps),
                 static_cast<unsigned>(recent),
                 s.ax_mg, s.ay_mg, s.az_mg,
                 s.accel_dynamic_mg,
                 gpio_get_level(INT_PIN));
      }
    }

    vTaskDelayUntil(&last, pdMS_TO_TICKS(50));
  }
}

void ImuSense::process_active_step_locked_(float dynamic_mg,
                                           int64_t now_us) {
  if (peak_high_) {
    if (dynamic_mg <= STEP_RELEASE_MG) peak_high_ = false;
    return;
  }

  if (dynamic_mg < STEP_HIGH_MG) return;
  peak_high_ = true;

  if (last_peak_us_ == 0) {
    last_peak_us_ = now_us;
    cadence_hits_ = 1;
    return;
  }

  const int64_t dt = now_us - last_peak_us_;

  if (dt < static_cast<int64_t>(STEP_MIN_INTERVAL_US)) {
    return;
  }

  if (dt > static_cast<int64_t>(STEP_MAX_INTERVAL_US)) {
    last_peak_us_ = now_us;
    cadence_hits_ = 1;
    return;
  }

  // Two cadence-consistent impacts are required before the first steps are
  // committed. A single pick-up/bump therefore does not become a "step".
  if (cadence_hits_ == 1) {
    note_step_locked_(last_peak_us_);
    note_step_locked_(now_us);
    cadence_hits_ = 2;
  } else {
    note_step_locked_(now_us);
    if (cadence_hits_ < 250) ++cadence_hits_;
  }

  last_peak_us_ = now_us;

  const uint16_t recent6 =
      recent_steps_locked_(now_us, 6000000LL);

  if (recent6 >= 4) {
    walking_now_ = true;
    walkies_since_take_ = true;
  }
}

void ImuSense::note_step_locked_(int64_t when_us) {
  // Keep the detector itself untouched and calibrate only the step credits.
  // 10 detected impacts therefore become 9 banked steps. The remainder is
  // carried so short walks do not suffer repeated integer truncation.
  step_credit_accum_ =
      static_cast<uint16_t>(step_credit_accum_ + STEP_CREDIT_NUM);

  if (step_credit_accum_ < STEP_CREDIT_DEN) {
    return;
  }

  step_credit_accum_ =
      static_cast<uint16_t>(step_credit_accum_ - STEP_CREDIT_DEN);

  ++steps_since_take_;
  ++steps_since_boot_;
  hardware_step_counter_ =
      static_cast<uint16_t>(steps_since_boot_ & 0xFFFFu);

  step_times_us_[step_time_head_] = when_us;
  step_time_head_ = (step_time_head_ + 1) % STEP_TIME_CAPACITY;
  if (step_time_count_ < STEP_TIME_CAPACITY) ++step_time_count_;
}

uint16_t ImuSense::recent_steps_locked_(int64_t now_us,
                                        int64_t window_us) const {
  uint16_t count = 0;

  for (size_t i = 0; i < step_time_count_; ++i) {
    const size_t idx =
        (step_time_head_ + STEP_TIME_CAPACITY - 1 - i) %
        STEP_TIME_CAPACITY;

    const int64_t t = step_times_us_[idx];
    if (t <= 0) break;
    if ((now_us - t) > window_us) break;
    ++count;
  }

  return count;
}

ImuMotionSnapshot ImuSense::take_snapshot() {
  ImuMotionSnapshot out;
  out.present = present_;
  out.pedometer_ready = pedometer_ready_;

  if (!present_ || !mutex_) return out;

  auto m = static_cast<SemaphoreHandle_t>(mutex_);

  if (xSemaphoreTake(m, pdMS_TO_TICKS(100)) == pdTRUE) {
    const int64_t now_us = esp_timer_get_time();

    out.moving_now = moving_now_;
    out.moved_since_last_take = moved_since_take_;
    out.moving_seconds_since_last_take = moving_ms_since_take_ / 1000;
    out.total_moving_seconds = total_moving_ms_ / 1000;

    out.walking_now = walking_now_;
    out.walkies_since_last_take = walkies_since_take_;
    out.steps_since_last_take = steps_since_take_;
    out.steps_since_boot = steps_since_boot_;
    out.hardware_step_counter = hardware_step_counter_;
    out.recent_steps_15s =
        recent_steps_locked_(now_us, 15000000LL);
    out.latest = latest_;

    moved_since_take_ = false;
    moving_ms_since_take_ = 0;
    steps_since_take_ = 0;
    walkies_since_take_ = false;

    xSemaphoreGive(m);
  }

  return out;
}

esp_err_t ImuSense::pause_sampler_() {
  if (!started_ || !task_handle_) return ESP_ERR_INVALID_STATE;

  pause_requested_ = true;

  for (int i = 0; i < 60; ++i) {
    if (task_paused_) return ESP_OK;
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  pause_requested_ = false;
  ESP_LOGE(TAG, "Timed out waiting for MPU sampler to pause");
  return ESP_ERR_TIMEOUT;
}

void ImuSense::resume_sampler_() {
  if (!started_ || !task_handle_) return;
  pause_requested_ = false;
  xTaskNotifyGive(static_cast<TaskHandle_t>(task_handle_));
}

esp_err_t ImuSense::enter_snooze() {
  if (!present_ || !pedometer_ready_) return ESP_ERR_INVALID_STATE;
  if (snoozing_) return ESP_OK;

  esp_err_t err = pause_sampler_();
  if (err != ESP_OK) return err;

  confirmation_fifo_active_ = false;

  // WALK-READY mode:
  // Keep the proven raw accelerometer path at 20 Hz and collect samples in
  // the sensor FIFO while the C6 light-sleeps. This intentionally avoids the
  // old MPU-6050-specific motion-register setup on the observed 0x70 device.
  err = start_confirmation_fifo_();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Could not start SNOOZE accel FIFO: %s",
             esp_err_to_name(err));
    (void)configure_active_();
    resume_sampler_();
    return err;
  }

  snoozing_ = true;

  ESP_LOGI(TAG,
           "SNOOZE MPU: 20Hz accel FIFO armed; short timer polling active");
  return ESP_OK;
}

esp_err_t ImuSense::start_confirmation_fifo_() {
  // Leave low-power cycle mode for a short 20Hz accel-only FIFO capture while
  // the C6 performs its six-second confirmation sleep.
  ESP_ERROR_CHECK(write_reg_(REG_INT_ENABLE, 0x00));
  ESP_ERROR_CHECK(write_reg_(REG_PWR_MGMT_1, 0x00));
  ESP_ERROR_CHECK(write_reg_(REG_PWR_MGMT_2, 0x07));
  ESP_ERROR_CHECK(write_reg_(REG_CONFIG, 0x03));
  ESP_ERROR_CHECK(write_reg_(REG_SMPLRT_DIV, SAMPLE_DIV_20HZ));
  ESP_ERROR_CHECK(write_reg_(REG_ACCEL_CONFIG, 0x00));

  ESP_ERROR_CHECK(stop_fifo_());

  // USER_CTRL FIFO_RESET then FIFO_EN. FIFO_EN register bit3 selects accel.
  ESP_ERROR_CHECK(write_reg_(REG_USER_CTRL, 0x04));
  vTaskDelay(pdMS_TO_TICKS(2));
  ESP_ERROR_CHECK(write_reg_(REG_USER_CTRL, 0x40));
  ESP_ERROR_CHECK(write_reg_(REG_FIFO_EN, 0x08));

  confirmation_fifo_active_ = true;
  return ESP_OK;
}

esp_err_t ImuSense::stop_fifo_() {
  if (!dev_) return ESP_ERR_INVALID_STATE;

  esp_err_t err = write_reg_(REG_FIFO_EN, 0x00);
  if (err != ESP_OK) return err;

  err = write_reg_(REG_USER_CTRL, 0x00);
  confirmation_fifo_active_ = false;
  return err;
}

esp_err_t ImuSense::count_confirmation_fifo_(uint32_t &steps) {
  steps = 0;

  uint8_t count_raw[2]{};
  ESP_ERROR_CHECK(read_regs_(REG_FIFO_COUNTH, count_raw, sizeof(count_raw)));

  uint16_t fifo_bytes =
      static_cast<uint16_t>(
          (static_cast<uint16_t>(count_raw[0]) << 8) |
          count_raw[1]);

  // One accel sample = 6 bytes. Ignore a partial tail and cap to the MPU FIFO.
  fifo_bytes = std::min<uint16_t>(fifo_bytes, 1020);
  const uint16_t sample_count = fifo_bytes / 6;

  bool high = false;
  int last_peak_sample = -1000;
  int cadence_hits = 0;

  uint8_t raw[6]{};

  for (uint16_t i = 0; i < sample_count; ++i) {
    esp_err_t err = read_regs_(REG_FIFO_R_W, raw, sizeof(raw));
    if (err != ESP_OK) {
      (void)stop_fifo_();
      return err;
    }

    const float dynamic_mg = accel_dynamic_mg_from_raw(raw);

    if (high) {
      if (dynamic_mg <= STEP_RELEASE_MG) high = false;
      continue;
    }

    if (dynamic_mg < STEP_HIGH_MG) continue;
    high = true;

    const int sample_delta = static_cast<int>(i) - last_peak_sample;

    // 20Hz: 5 samples=250ms, 25 samples=1.25s.
    if (last_peak_sample < 0 || sample_delta > 25) {
      last_peak_sample = static_cast<int>(i);
      cadence_hits = 1;
      continue;
    }

    if (sample_delta < 5) continue;

    if (cadence_hits == 1) {
      steps += 2;
      cadence_hits = 2;
    } else {
      ++steps;
      if (cadence_hits < 250) ++cadence_hits;
    }

    last_peak_sample = static_cast<int>(i);
  }

  ESP_ERROR_CHECK(stop_fifo_());

  if (steps > 0 && mutex_) {
    auto m = static_cast<SemaphoreHandle_t>(mutex_);
    if (xSemaphoreTake(m, pdMS_TO_TICKS(100)) == pdTRUE) {
      const int64_t now_us = esp_timer_get_time();

      // Space the synthetic timestamps through the recent window so the
      // normal WALKIES/recent-step UI stays coherent.
      const int64_t spacing =
          steps > 1 ? std::min<int64_t>(600000, 5000000LL / steps)
                    : 500000;

      const int64_t first =
          now_us - static_cast<int64_t>(steps - 1) * spacing;

      for (uint32_t i = 0; i < steps; ++i) {
        note_step_locked_(first + static_cast<int64_t>(i) * spacing);
      }

      xSemaphoreGive(m);
    }
  }

  return ESP_OK;
}

esp_err_t ImuSense::clear_interrupt_(uint8_t &status) {
  return read_reg_(REG_INT_STATUS, status);
}

ImuSnoozePoll ImuSense::poll_snooze() {
  ImuSnoozePoll out;
  if (!snoozing_) return out;

  if (!confirmation_fifo_active_) {
    esp_err_t start_err = start_confirmation_fifo_();
    if (start_err != ESP_OK) {
      ESP_LOGW(TAG, "Could not restart SNOOZE FIFO: %s",
               esp_err_to_name(start_err));
      return out;
    }
  }

  uint32_t steps = 0;
  esp_err_t err = count_confirmation_fifo_(steps);

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "SNOOZE FIFO read failed: %s",
             esp_err_to_name(err));
    (void)start_confirmation_fifo_();
    return out;
  }

  // count_confirmation_fifo_() drains/stops the FIFO. Re-arm immediately so
  // the next light-sleep slice continues collecting step samples.
  err = start_confirmation_fifo_();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Could not re-arm SNOOZE FIFO: %s",
             esp_err_to_name(err));
  }

  out.ok = true;
  out.new_steps = steps;
  out.wake_event = steps > 0;
  out.wake_source = 0;

  if (mutex_) {
    auto m = static_cast<SemaphoreHandle_t>(mutex_);
    if (xSemaphoreTake(m, pdMS_TO_TICKS(50)) == pdTRUE) {
      out.steps_since_boot = steps_since_boot_;
      out.hardware_step_counter = hardware_step_counter_;
      xSemaphoreGive(m);
    }
  }

  ESP_LOGI(TAG,
           "SNOOZE FIFO POLL: +%lu rough steps total_boot=%lu",
           static_cast<unsigned long>(steps),
           static_cast<unsigned long>(out.steps_since_boot));

  return out;
}

void ImuSense::latch_walkies_from_snooze() {
  if (!mutex_) return;

  auto m = static_cast<SemaphoreHandle_t>(mutex_);
  if (xSemaphoreTake(m, pdMS_TO_TICKS(50)) == pdTRUE) {
    walking_now_ = true;
    walkies_since_take_ = true;
    moved_since_take_ = true;
    xSemaphoreGive(m);
  }
}

esp_err_t ImuSense::exit_snooze() {
  if (!snoozing_) return ESP_OK;

  (void)stop_fifo_();

  esp_err_t err = configure_active_();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Could not restore active MPU mode: %s",
             esp_err_to_name(err));
  }

  uint8_t stale = 0;
  (void)clear_interrupt_(stale);

  snoozing_ = false;
  confirmation_fifo_active_ = false;

  resume_sampler_();

  ESP_LOGI(TAG, "SNOOZE MPU exit: active 20Hz accel sampler restored");
  return err;
}

esp_err_t ImuSense::read_reg_(uint8_t reg, uint8_t &value) {
  if (!dev_) return ESP_ERR_INVALID_STATE;

  auto dev = static_cast<i2c_master_dev_handle_t>(dev_);
  return i2c_master_transmit_receive(
      dev, &reg, 1, &value, 1, 100);
}

esp_err_t ImuSense::write_reg_(uint8_t reg, uint8_t value) {
  if (!dev_) return ESP_ERR_INVALID_STATE;

  uint8_t b[2] = {reg, value};
  auto dev = static_cast<i2c_master_dev_handle_t>(dev_);
  return i2c_master_transmit(dev, b, sizeof(b), 100);
}

esp_err_t ImuSense::read_regs_(uint8_t reg, uint8_t *data, size_t len) {
  if (!dev_) return ESP_ERR_INVALID_STATE;

  auto dev = static_cast<i2c_master_dev_handle_t>(dev_);
  return i2c_master_transmit_receive(
      dev, &reg, 1, data, len, 100);
}

esp_err_t ImuSense::read_sample_(ImuSample &s) {
  uint8_t raw[6]{};
  esp_err_t err =
      read_regs_(REG_ACCEL_XOUT_H, raw, sizeof(raw));
  if (err != ESP_OK) return err;

  const int16_t ax = be16(&raw[0]);
  const int16_t ay = be16(&raw[2]);
  const int16_t az = be16(&raw[4]);

  s.ax_mg = ax * ACCEL_MG_PER_LSB;
  s.ay_mg = ay * ACCEL_MG_PER_LSB;
  s.az_mg = az * ACCEL_MG_PER_LSB;

  s.gx_dps = 0.0f;
  s.gy_dps = 0.0f;
  s.gz_dps = 0.0f;
  s.gyro_magnitude_dps = 0.0f;

  const float mag =
      std::sqrt(s.ax_mg * s.ax_mg +
                s.ay_mg * s.ay_mg +
                s.az_mg * s.az_mg);

  s.accel_dynamic_mg = std::fabs(mag - 1000.0f);
  return ESP_OK;
}
