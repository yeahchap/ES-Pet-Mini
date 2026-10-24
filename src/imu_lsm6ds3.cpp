// Ultimate XIAO C6 only. Classic RC7 keeps its MPU-compatible driver.
// LSM6DS3 register configuration uses ST's BSD-licensed platform driver.
#ifdef ES_PET_ULTIMATE_XIAO
#include "imu.h"
#include "lsm6ds3_support.h"
#include <algorithm>
#include <cmath>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {
constexpr gpio_num_t SDA_PIN = GPIO_NUM_22, SCL_PIN = GPIO_NUM_23;
constexpr gpio_num_t INT_PIN = GPIO_NUM_2;
const char *TAG = "espet_lsm6ds3";
i2c_master_dev_handle_t device(void *p) {
  return static_cast<i2c_master_dev_handle_t>(p);
}
SemaphoreHandle_t mutex(void *p) { return static_cast<SemaphoreHandle_t>(p); }
int32_t platform_read(void *p, uint8_t reg, uint8_t *data, uint16_t len) {
  return i2c_master_transmit_receive(device(p), &reg, 1, data, len, 100);
}
int32_t platform_write(void *p, uint8_t reg, const uint8_t *data, uint16_t len) {
  // ST configuration calls write at most one register at a time here.
  if (len > 16) return ESP_ERR_INVALID_SIZE;
  uint8_t bytes[17] = {reg};
  std::copy(data, data + len, bytes + 1);
  return i2c_master_transmit(device(p), bytes, len + 1, 100);
}
stmdev_ctx_t context(void *p) {
  stmdev_ctx_t ctx{};
  ctx.read_reg = platform_read;
  ctx.write_reg = platform_write;
  ctx.handle = p;
  return ctx;
}
}

esp_err_t ImuSense::write_reg_(uint8_t reg, uint8_t value) {
  return static_cast<esp_err_t>(platform_write(dev_, reg, &value, 1));
}
esp_err_t ImuSense::read_regs_(uint8_t reg, uint8_t *data, size_t len) {
  return static_cast<esp_err_t>(platform_read(dev_, reg, data, len));
}
esp_err_t ImuSense::read_reg_(uint8_t reg, uint8_t &value) {
  return read_regs_(reg, &value, 1);
}
esp_err_t ImuSense::read_sample_(ImuSample &sample) {
  auto ctx = context(dev_);
  int16_t raw[3]{};
  const auto err = lsm6ds3_acceleration_raw_get(&ctx, raw);
  if (err != 0) return static_cast<esp_err_t>(err);
  sample = {};
  sample.ax_mg = lsm6ds3_from_fs2g_to_mg(raw[0]);
  sample.ay_mg = lsm6ds3_from_fs2g_to_mg(raw[1]);
  sample.az_mg = lsm6ds3_from_fs2g_to_mg(raw[2]);
  const float magnitude = std::sqrt(sample.ax_mg * sample.ax_mg +
                                    sample.ay_mg * sample.ay_mg +
                                    sample.az_mg * sample.az_mg);
  sample.accel_dynamic_mg = std::fabs(magnitude - 1000.0f);
  return ESP_OK;
}

esp_err_t ImuSense::read_lsm_counter_(uint16_t &raw) {
  // Avoid a torn 16-bit value if the low byte rolls over between I2C reads.
  for (int attempt = 0; attempt < 3; ++attempt) {
    uint8_t high_before = 0, low = 0, high_after = 0;
    esp_err_t err = read_reg_(LSM6DS3_STEP_COUNTER_H, high_before);
    if (err != ESP_OK) return err;
    err = read_reg_(LSM6DS3_STEP_COUNTER_L, low);
    if (err != ESP_OK) return err;
    err = read_reg_(LSM6DS3_STEP_COUNTER_H, high_after);
    if (err != ESP_OK) return err;
    if (high_before == high_after) {
      raw = static_cast<uint16_t>((high_after << 8) | low);
      return ESP_OK;
    }
  }
  return ESP_ERR_INVALID_RESPONSE;
}

esp_err_t ImuSense::begin() {
  i2c_master_bus_config_t cfg{};
  cfg.i2c_port = I2C_NUM_0;
  cfg.sda_io_num = SDA_PIN;
  cfg.scl_io_num = SCL_PIN;
  cfg.clk_source = I2C_CLK_SRC_DEFAULT;
  cfg.glitch_ignore_cnt = 7;
  cfg.flags.enable_internal_pullup = true;
  i2c_master_bus_handle_t bus = nullptr;
  esp_err_t err = i2c_new_master_bus(&cfg, &bus);
  if (err != ESP_OK) return err;
  bus_ = bus;
  auto fail = [&](esp_err_t error) {
    if (dev_) { (void)i2c_master_bus_rm_device(device(dev_)); dev_ = nullptr; }
    if (mutex_) { vSemaphoreDelete(mutex(mutex_)); mutex_ = nullptr; }
    (void)i2c_del_master_bus(bus);
    bus_ = nullptr;
    present_ = pedometer_ready_ = false;
    return error;
  };
  uint8_t address = 0;
  esp_err_t discovery_error = ESP_ERR_NOT_FOUND;
  ESP_LOGI(TAG, "ULTIMATE LSM6DS3: SDA=D4/GPIO22 SCL=D5/GPIO23 INT1=D2/GPIO2");
  for (uint8_t candidate : {0x6a, 0x6b}) {
    err = i2c_master_probe(bus, candidate, 100);
    ESP_LOGI(TAG, "Probe 0x%02X: %s", candidate, esp_err_to_name(err));
    if (err != ESP_OK) {
      if (err != ESP_ERR_NOT_FOUND) discovery_error = err;
      continue;
    }
    i2c_device_config_t dc{};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address = candidate;
    dc.scl_speed_hz = 100000;
    i2c_master_dev_handle_t d = nullptr;
    err = i2c_master_bus_add_device(bus, &dc, &d);
    if (err != ESP_OK) return fail(err);
    dev_ = d;
    uint8_t who = 0;
    err = read_reg_(LSM6DS3_WHO_AM_I, who);
    ESP_LOGI(TAG, "Address 0x%02X WHO_AM_I=0x%02X read=%s (expected 0x69)",
             candidate, who, esp_err_to_name(err));
    if (err == ESP_OK && who == LSM6DS3_ID) { address = candidate; break; }
    discovery_error = err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
    (void)i2c_master_bus_rm_device(d);
    dev_ = nullptr;
  }
  if (!address) {
    ESP_LOGE(TAG, "No verified LSM6DS3. This build expects WHO_AM_I 0x69; "
                  "other LSM6 variants need their own driver.");
    return fail(discovery_error);
  }
  auto ctx = context(dev_);
  err = static_cast<esp_err_t>(lsm6ds3_reset_set(&ctx, 1));
  if (err != ESP_OK) return fail(err);
  bool reset_done = false;
  for (int i = 0; i < 20; ++i) {
    vTaskDelay(pdMS_TO_TICKS(10));
    uint8_t reset = 1;
    err = static_cast<esp_err_t>(lsm6ds3_reset_get(&ctx, &reset));
    if (err != ESP_OK) return fail(err);
    if (!reset) { reset_done = true; break; }
  }
  if (!reset_done) return fail(ESP_ERR_TIMEOUT);
  err = static_cast<esp_err_t>(espet_lsm6ds3_configure(&ctx));
  if (err != ESP_OK) return fail(err);
  // Reset ONLY the volatile sensor count at initialisation, never life/XP data
  // and never when entering/exiting sleep. Future steps are banked once.
  err = static_cast<esp_err_t>(lsm6ds3_pedo_step_reset_set(&ctx, 1));
  if (err != ESP_OK) return fail(err);
  err = static_cast<esp_err_t>(lsm6ds3_pedo_step_reset_set(&ctx, 0));
  if (err != ESP_OK) return fail(err);
  struct Check { uint8_t reg, mask, want; };
  const Check checks[] = {
      {LSM6DS3_CTRL1_XL, 0xfc, 0x60}, // 416 Hz, +/-2g
      {LSM6DS3_CTRL2_G, 0xf0, 0x00},  // gyroscope off
      {LSM6DS3_CTRL3_C, 0x74, 0x44},  // BDU, auto-increment, active-high push-pull
      {LSM6DS3_CTRL10_C, 0x06, 0x04}, // embedded engine on; reset released
      {LSM6DS3_TAP_CFG, 0x4f, 0x4f},  // pedometer + all tap axes + latched IRQ
      {LSM6DS3_WAKE_UP_THS, 0x80, 0x80},
      {LSM6DS3_MD1_CFG, 0xff, 0x08},  // only double tap routed to INT1
      {LSM6DS3_INT1_CTRL, 0xff, 0x00}, // no per-step/data-ready wake
  };
  for (const auto &check : checks) {
    uint8_t value = 0;
    err = read_reg_(check.reg, value);
    if (err != ESP_OK) return fail(err);
    if ((value & check.mask) != check.want) {
      ESP_LOGE(TAG, "Configuration readback failed reg=0x%02X value=0x%02X",
               check.reg, value);
      return fail(ESP_ERR_INVALID_RESPONSE);
    }
  }
  uint16_t raw = 0;
  err = read_lsm_counter_(raw);
  if (err != ESP_OK) return fail(err);
  lsm_steps_.baseline(raw);
  last_counter_sample_us_ = esp_timer_get_time();
  gpio_config_t irq{};
  irq.pin_bit_mask = 1ULL << INT_PIN;
  irq.mode = GPIO_MODE_INPUT;
  irq.pull_down_en = GPIO_PULLDOWN_ENABLE;
  err = gpio_config(&irq);
  if (err != ESP_OK) return fail(err);
  lsm6ds3_tap_src_t stale{};
  err = static_cast<esp_err_t>(lsm6ds3_tap_src_get(&ctx, &stale));
  if (err != ESP_OK) return fail(err);
  mutex_ = xSemaphoreCreateMutex();
  if (!mutex_) return fail(ESP_ERR_NO_MEM);
  present_ = pedometer_ready_ = true;
  ESP_LOGI(TAG, "LSM6DS3 0x%02X WHO=0x69; hardware step counter ENABLED; baseline=%u", address, raw);
  ESP_LOGI(TAG, "Double tap ENABLED: latched active-high INT1/GPIO2; gyro OFF; accel 416Hz");
  return ESP_OK;
}

esp_err_t ImuSense::service_lsm_locked_(uint32_t &new_steps) {
  new_steps = 0;
  uint16_t raw = 0;
  esp_err_t count_error = read_lsm_counter_(raw);
  const int64_t now = esp_timer_get_time();
  if (count_error == ESP_OK) {
    // Generous sanity ceiling: ten steps/second plus 64 buffered detections.
    // A sensor reset must not look like an entire 16-bit rollover of new XP.
    const uint32_t limit = static_cast<uint32_t>(std::min<int64_t>(65535,
        64 + std::max<int64_t>(0, now - last_counter_sample_us_) / 100000));
    const uint16_t delta = static_cast<uint16_t>(raw - lsm_steps_.raw());
    new_steps = lsm_steps_.observe(raw, limit);
    last_counter_sample_us_ = now;
    if (delta > limit) {
      ESP_LOGW(TAG, "Implausible hardware counter jump (%u); rebasing without step credits", delta);
      count_error = ESP_ERR_INVALID_RESPONSE;
    }
    if (new_steps) {
      last_hw_step_us_ = now;
      moved_since_take_ = walkies_since_take_ = true;
      ESP_LOGI(TAG, "Hardware steps +%lu; raw=%u; since boot=%lu%s",
               static_cast<unsigned long>(new_steps), raw,
               static_cast<unsigned long>(lsm_steps_.total()), snoozing_ ? " (snooze)" : "");
    }
  }
  walking_now_ = last_hw_step_us_ != 0 && now - last_hw_step_us_ < 15000000LL;
  auto ctx = context(dev_);
  lsm6ds3_tap_src_t tap{};
  const auto tap_error = lsm6ds3_tap_src_get(&ctx, &tap);
  if (tap_error == 0 && lsm_taps_.observe(tap.tap_ia, tap.double_tap, now)) {
    ESP_LOGI(TAG, "LSM6DS3 double tap; display request latched");
  }
  return count_error != ESP_OK ? count_error : static_cast<esp_err_t>(tap_error);
}

esp_err_t ImuSense::start() {
  if (!present_) return ESP_ERR_INVALID_STATE;
  if (started_) return ESP_OK;
  TaskHandle_t task = nullptr;
  if (xTaskCreate(&ImuSense::task_entry_, "lsm-active", 4096, this, 4, &task) != pdPASS)
    return ESP_ERR_NO_MEM;
  task_handle_ = task;
  started_ = true;
  return ESP_OK;
}
void ImuSense::task_entry_(void *p) { static_cast<ImuSense *>(p)->task_loop_(); }
void ImuSense::task_loop_() {
  unsigned failures = 0;
  for (;;) {
    xSemaphoreTake(mutex(mutex_), portMAX_DELAY);
    if (!snoozing_) {
      uint32_t steps = 0;
      const auto err = service_lsm_locked_(steps);
      if (err != ESP_OK) {
        if (failures++ % 25 == 0) ESP_LOGW(TAG, "IMU read failed: %s", esp_err_to_name(err));
      } else failures = 0;
      ImuSample sample{};
      if (read_sample_(sample) == ESP_OK) {
        latest_ = sample;
        const int64_t now = esp_timer_get_time();
        moving_now_ = sample.accel_dynamic_mg > 100 || walking_now_;
        if (moving_now_) {
          moved_since_take_ = true;
          // Measure active observation time only; no invented sleep motion time.
          const uint32_t dt = last_motion_sample_us_ == 0 ? 0 :
              static_cast<uint32_t>(std::min<int64_t>((now - last_motion_sample_us_) / 1000, 1000));
          moving_ms_since_take_ += dt;
          total_moving_ms_ += dt;
        }
        last_motion_sample_us_ = now;
      }
    }
    xSemaphoreGive(mutex(mutex_));
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

ImuMotionSnapshot ImuSense::take_snapshot() {
  ImuMotionSnapshot snapshot{};
  if (!present_) return snapshot;
  xSemaphoreTake(mutex(mutex_), portMAX_DELAY);
  uint32_t extra = 0;
  (void)service_lsm_locked_(extra);
  snapshot.present = present_;
  snapshot.pedometer_ready = pedometer_ready_;
  snapshot.latest = latest_;
  snapshot.moving_now = moving_now_ || walking_now_;
  snapshot.moved_since_last_take = moved_since_take_;
  snapshot.moving_seconds_since_last_take = moving_ms_since_take_ / 1000;
  snapshot.total_moving_seconds = total_moving_ms_ / 1000;
  snapshot.walking_now = walking_now_;
  snapshot.walkies_since_last_take = walkies_since_take_;
  snapshot.steps_since_last_take = lsm_steps_.take_pending();
  snapshot.steps_since_boot = lsm_steps_.total();
  snapshot.hardware_step_counter = lsm_steps_.raw();
  // Batched hardware counts have no individual step timestamps. Do not invent
  // a 15-second cadence history from a minute's accumulated sleep steps.
  snapshot.recent_steps_15s = 0;
  moved_since_take_ = walkies_since_take_ = false;
  moving_ms_since_take_ = 0;
  xSemaphoreGive(mutex(mutex_));
  return snapshot;
}

esp_err_t ImuSense::enter_snooze() {
  if (!present_) return ESP_ERR_INVALID_STATE;
  xSemaphoreTake(mutex(mutex_), portMAX_DELAY);
  // Same sensor configuration in both modes: pedometer and tap engine keep
  // running. The shared mutex finishes any active read before sleeping.
  snoozing_ = true;
  last_motion_sample_us_ = 0;
  xSemaphoreGive(mutex(mutex_));
  return ESP_OK;
}
ImuSnoozePoll ImuSense::poll_snooze() {
  ImuSnoozePoll poll{};
  if (!present_) return poll;
  xSemaphoreTake(mutex(mutex_), portMAX_DELAY);
  poll.ok = service_lsm_locked_(poll.new_steps) == ESP_OK;
  poll.wake_event = lsm_taps_.pending();
  poll.wake_source = poll.wake_event ? 0x10 : 0;
  poll.steps_since_boot = lsm_steps_.total();
  poll.hardware_step_counter = lsm_steps_.raw();
  xSemaphoreGive(mutex(mutex_));
  return poll;
}
void ImuSense::latch_walkies_from_snooze() {
  if (!present_) return;
  xSemaphoreTake(mutex(mutex_), portMAX_DELAY);
  walkies_since_take_ = true;
  xSemaphoreGive(mutex(mutex_));
}
esp_err_t ImuSense::exit_snooze() {
  if (!present_) return ESP_ERR_INVALID_STATE;
  xSemaphoreTake(mutex(mutex_), portMAX_DELAY);
  uint32_t extra = 0;
  const auto err = service_lsm_locked_(extra);
  snoozing_ = false;
  last_motion_sample_us_ = 0;
  xSemaphoreGive(mutex(mutex_));
  return err;
}
bool ImuSense::take_redraw_request() {
  if (!mutex_) return false;
  xSemaphoreTake(mutex(mutex_), portMAX_DELAY);
  const bool value = lsm_taps_.take();
  xSemaphoreGive(mutex(mutex_));
  return value;
}
bool ImuSense::redraw_pending() {
  if (!mutex_) return false;
  xSemaphoreTake(mutex(mutex_), portMAX_DELAY);
  const bool value = lsm_taps_.pending();
  xSemaphoreGive(mutex(mutex_));
  return value;
}
void ImuSense::begin_display_refresh() {
  if (!mutex_) return;
  xSemaphoreTake(mutex(mutex_), portMAX_DELAY);
  lsm_taps_.begin_refresh();
  xSemaphoreGive(mutex(mutex_));
}
void ImuSense::finish_display_refresh() {
  if (!mutex_) return;
  xSemaphoreTake(mutex(mutex_), portMAX_DELAY);
  // Acknowledge a tap/impact latched while the panel was busy. It must not
  // become a second display request as soon as we re-enter light sleep.
  auto ctx = context(dev_);
  lsm6ds3_tap_src_t stale{};
  const auto err = lsm6ds3_tap_src_get(&ctx, &stale);
  if (err != 0) ESP_LOGW(TAG, "Post-refresh tap clear failed: %s",
                         esp_err_to_name(static_cast<esp_err_t>(err)));
  lsm_taps_.finish_refresh(esp_timer_get_time());
  xSemaphoreGive(mutex(mutex_));
}
void ImuSense::begin_feedback() {
  if (!mutex_) return;
  xSemaphoreTake(mutex(mutex_), portMAX_DELAY);
  lsm_taps_.begin_feedback();
  xSemaphoreGive(mutex(mutex_));
}
void ImuSense::finish_feedback() {
  if (!mutex_) return;
  xSemaphoreTake(mutex(mutex_), portMAX_DELAY);
  auto ctx = context(dev_);
  lsm6ds3_tap_src_t stale{};
  (void)lsm6ds3_tap_src_get(&ctx, &stale);
  // Retain an already accepted user request; only feedback-generated taps
  // are suppressed. Step-counter observations are never reset or drained.
  lsm_taps_.finish_feedback(esp_timer_get_time());
  xSemaphoreGive(mutex(mutex_));
}
#endif
