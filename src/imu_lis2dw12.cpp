// ES-PET Ultimate XIAO C6 bench port. Kept separate from proven MPU driver.
// ST LIS2DW12: WHO_AM_I 0x44; register values from ST DT0101 application note.
// This first port deliberately does NOT pretend a 32-sample FIFO can count steps
// during long sleep. Active sampling counts candidate steps; sleep handles taps.
// Calibrate tap thresholds, walking sensitivity and current before personal clone.
#ifdef ES_PET_ULTIMATE_XIAO
#include "imu.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {
constexpr gpio_num_t SDA_PIN=GPIO_NUM_22, SCL_PIN=GPIO_NUM_23, INT_PIN=GPIO_NUM_2;
constexpr uint8_t WHO_REG=0x0f, WHO_VAL=0x44, CTRL1=0x20, CTRL2=0x21,
    CTRL3=0x22, CTRL4=0x23, CTRL6=0x25, DATA=0x28, TAP_SRC=0x39,
    TAP_X=0x30, TAP_Y=0x31, TAP_Z=0x32, DUR=0x33, WAKE_THS=0x34,
    CTRL7=0x3f;
constexpr float MG_PER_LSB=0.061f; // +/-2g, 14-bit left-aligned
constexpr int64_t MIN_STEP_US=260000, MAX_STEP_US=1250000;
const char *TAG="espet_lis2dw12";
inline i2c_master_dev_handle_t dev(void *h) {
    return static_cast<i2c_master_dev_handle_t>(h);
}
inline SemaphoreHandle_t lock(void *m) {
    return static_cast<SemaphoreHandle_t>(m);
}
inline int16_t axis(const uint8_t *p) {
    return static_cast<int16_t>(static_cast<uint16_t>(p[0]) |
                                (static_cast<uint16_t>(p[1])<<8));
}
}

esp_err_t ImuSense::write_reg_(uint8_t r, uint8_t v) {
  uint8_t d[]={r,v};
  return i2c_master_transmit(dev(dev_),d,sizeof(d),100);
}
esp_err_t ImuSense::read_regs_(uint8_t r,uint8_t *p,size_t n) {
  return i2c_master_transmit_receive(dev(dev_),&r,1,p,n,100);
}
esp_err_t ImuSense::read_reg_(uint8_t r,uint8_t &v) {
  return read_regs_(r,&v,1);
}
esp_err_t ImuSense::read_sample_(ImuSample &s) {
  uint8_t b[6]={};
  esp_err_t e=read_regs_(DATA,b,6);
  if(e!=ESP_OK) return e;
  s.ax_mg=axis(b)*MG_PER_LSB; s.ay_mg=axis(b+2)*MG_PER_LSB;
  s.az_mg=axis(b+4)*MG_PER_LSB;
  const float m=std::sqrt(s.ax_mg*s.ax_mg+s.ay_mg*s.ay_mg+s.az_mg*s.az_mg);
  s.accel_dynamic_mg=std::fabs(m-1000.0f);
  return ESP_OK;
}
esp_err_t ImuSense::begin() {
  i2c_master_bus_config_t cfg{};
  cfg.clk_source=I2C_CLK_SRC_DEFAULT;
  cfg.i2c_port=I2C_NUM_0;
  cfg.sda_io_num=SDA_PIN; cfg.scl_io_num=SCL_PIN;
  cfg.glitch_ignore_cnt=7;
  cfg.flags.enable_internal_pullup=true;
  i2c_master_bus_handle_t bus=nullptr;
  esp_err_t e=i2c_new_master_bus(&cfg,&bus);
  if(e!=ESP_OK) return e;
  bus_=bus;
  // SA0 is breakout-dependent; 0x19 and 0x18 are both valid.
  uint8_t address=0;
  for(uint8_t candidate:{0x19,0x18}) {
    if(i2c_master_probe(bus,candidate,100)==ESP_OK) { address=candidate; break; }
  }
  if(!address) return ESP_ERR_NOT_FOUND;
  i2c_device_config_t dc{};
  dc.dev_addr_length=I2C_ADDR_BIT_LEN_7;
  dc.device_address=address; dc.scl_speed_hz=400000;
  i2c_master_dev_handle_t device=nullptr;
  ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus,&dc,&device),TAG,"I2C add");
  dev_=device;
  uint8_t who=0;
  ESP_RETURN_ON_ERROR(read_reg_(WHO_REG,who),TAG,"WHO_AM_I");
  if(who!=WHO_VAL) { ESP_LOGE(TAG,"Wrong IMU ID %02x",who);return ESP_ERR_INVALID_RESPONSE; }
  ESP_RETURN_ON_ERROR(write_reg_(CTRL2,0x40),TAG,"reset");
  vTaskDelay(pdMS_TO_TICKS(10));
  ESP_RETURN_ON_ERROR(write_reg_(CTRL2,0x0c),TAG,"BDU/autoincrement");
  ESP_RETURN_ON_ERROR(write_reg_(CTRL6,0x04),TAG,"low noise +/-2g");
  ESP_RETURN_ON_ERROR(write_reg_(CTRL3,0x10),TAG,"latched interrupts");
  // Configure double tap (ST DT0101): 400 Hz high performance, all axes.
  ESP_RETURN_ON_ERROR(write_reg_(CTRL4,0x08),TAG,"INT1 double tap");
  ESP_RETURN_ON_ERROR(write_reg_(TAP_X,0x0c),TAG,"tap X threshold");
  ESP_RETURN_ON_ERROR(write_reg_(TAP_Y,0x0c),TAG,"tap Y threshold");
  ESP_RETURN_ON_ERROR(write_reg_(TAP_Z,0xec),TAG,"tap Z / axes");
  ESP_RETURN_ON_ERROR(write_reg_(DUR,0x62),TAG,"double tap window");
  ESP_RETURN_ON_ERROR(write_reg_(WAKE_THS,0x80),TAG,"double tap select");
  ESP_RETURN_ON_ERROR(write_reg_(CTRL1,0x74),TAG,"400Hz HP");
  vTaskDelay(pdMS_TO_TICKS(10));
  ESP_RETURN_ON_ERROR(write_reg_(CTRL7,0x20),TAG,"enable interrupts");
  gpio_config_t irq{};
  irq.pin_bit_mask=1ULL<<INT_PIN;
  irq.mode=GPIO_MODE_INPUT;
  irq.pull_down_en=GPIO_PULLDOWN_ENABLE;
  ESP_RETURN_ON_ERROR(gpio_config(&irq),TAG,"INT1 GPIO");
  uint8_t stale=0; (void)read_reg_(TAP_SRC,stale);
  mutex_=xSemaphoreCreateMutex();
  if(!mutex_) return ESP_ERR_NO_MEM;
  present_=true; pedometer_ready_=true;
  ESP_LOGI(TAG,"LIS2DW12 0x%02X on SDA22 SCL23; INT1 GPIO2; double tap enabled",address);
  ESP_LOGW(TAG,"Walking counts while asleep are NOT yet validated");
  return ESP_OK;
}
esp_err_t ImuSense::start() {
  if(!present_) return ESP_ERR_INVALID_STATE;
  if(started_) return ESP_OK;
  if(xTaskCreate(&ImuSense::task_entry_,"lis-active",4096,this,4,nullptr)!=pdPASS)
    return ESP_ERR_NO_MEM;
  started_=true;
  return ESP_OK;
}
void ImuSense::task_entry_(void *p) { static_cast<ImuSense*>(p)->task_loop_(); }
void ImuSense::task_loop_() {
  for(;;) {
    if(snoozing_) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
    ImuSample s{};
    if(read_sample_(s)==ESP_OK) {
      const int64_t now=esp_timer_get_time();
      xSemaphoreTake(lock(mutex_),portMAX_DELAY);
      latest_=s;
      const bool moving=s.accel_dynamic_mg>100;
      moving_now_=moving;
      if(moving) { moved_since_take_=true; moving_ms_since_take_+=50; total_moving_ms_+=50; }
      // Very conservative candidate step estimator; calibrate to user's walk.
      if(s.accel_dynamic_mg<70) peak_high_=false;
      if(!peak_high_ && s.accel_dynamic_mg>145) {
        peak_high_=true;
        const int64_t gap=now-last_peak_us_;
        if(last_peak_us_ && gap>=MIN_STEP_US && gap<=MAX_STEP_US) {
          if(cadence_hits_<4) ++cadence_hits_;
          if(cadence_hits_>=3) {
            ++steps_since_take_; ++steps_since_boot_;
            walkies_since_take_=true; walking_now_=true;
          }
        } else if(gap>MAX_STEP_US) { cadence_hits_=0; walking_now_=false; }
        last_peak_us_=now;
      }
      xSemaphoreGive(lock(mutex_));
    }
    // Tap IRQ is latched and may arrive during active Wi-Fi scans.
    if(gpio_get_level(INT_PIN)) {
      uint8_t src=0;
      if(read_reg_(TAP_SRC,src)==ESP_OK && (src&0x10)) {
        xSemaphoreTake(lock(mutex_),portMAX_DELAY);
        redraw_request_=true;
        xSemaphoreGive(lock(mutex_));
        ESP_LOGI(TAG,"Double tap detected; display request latched");
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
ImuMotionSnapshot ImuSense::take_snapshot() {
  ImuMotionSnapshot s{};
  if(!present_) return s;
  xSemaphoreTake(lock(mutex_),portMAX_DELAY);
  s.present=present_; s.pedometer_ready=pedometer_ready_;
  s.latest=latest_;
  s.moving_now=moving_now_; s.moved_since_last_take=moved_since_take_;
  s.moving_seconds_since_last_take=moving_ms_since_take_/1000;
  s.total_moving_seconds=total_moving_ms_/1000;
  s.walking_now=walking_now_;
  s.walkies_since_last_take=walkies_since_take_;
  s.steps_since_last_take=steps_since_take_;
  s.steps_since_boot=steps_since_boot_;
  s.hardware_step_counter=static_cast<uint16_t>(steps_since_boot_);
  s.recent_steps_15s=0; // not fabricated when active sampling cadence is unknown
  moved_since_take_=false;moving_ms_since_take_=0;
  walkies_since_take_=false;steps_since_take_=0;
  xSemaphoreGive(lock(mutex_));
  return s;
}
esp_err_t ImuSense::enter_snooze() {
  if(!present_) return ESP_ERR_INVALID_STATE;
  snoozing_=true; // sensor tap engine remains running; FreeRTOS task pauses
  return ESP_OK;
}
ImuSnoozePoll ImuSense::poll_snooze() {
  ImuSnoozePoll p{}; p.ok=present_;
  if(!present_) return p;
  uint8_t src=0;
  if(read_reg_(TAP_SRC,src)==ESP_OK && (src&0x10)) {
    xSemaphoreTake(lock(mutex_),portMAX_DELAY);
    redraw_request_=true;
    xSemaphoreGive(lock(mutex_));
    p.wake_event=true;
  }
  // NO false steps: we did not collect complete FIFO data during light sleep.
  p.new_steps=0;
  p.steps_since_boot=steps_since_boot_;
  return p;
}
void ImuSense::latch_walkies_from_snooze() { /* no inferred steps */ }
esp_err_t ImuSense::exit_snooze() { snoozing_=false; return ESP_OK; }
bool ImuSense::take_redraw_request() {
  if(!mutex_) return false;
  xSemaphoreTake(lock(mutex_),portMAX_DELAY);
  bool flag=redraw_request_; redraw_request_=false;
  xSemaphoreGive(lock(mutex_));
  return flag;
}
#endif
