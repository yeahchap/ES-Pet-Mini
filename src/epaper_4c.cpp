#include "epaper.h"
#include "config.h"

#include <cstdlib>
#include <cstring>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "trim_epaper";

namespace {

inline void delay_ms(uint32_t ms) {
  if (ms == 0) return;

  // RC4: this project runs FreeRTOS at 100 Hz (10 ms ticks).  Therefore
  // pdMS_TO_TICKS(1) and pdMS_TO_TICKS(2) are ZERO, which meant our JD79660
  // 1 ms BUSY grace and, critically, the 2 ms RESET-low pulse never actually
  // happened.  Use a real microsecond delay for sub-tick hardware timing.
  if (ms < portTICK_PERIOD_MS) {
    esp_rom_delay_us(ms * 1000U);
    return;
  }

  vTaskDelay(pdMS_TO_TICKS(ms));
}

inline spi_device_handle_t as_spi(void *p) {
  return reinterpret_cast<spi_device_handle_t>(p);
}

}  // namespace

Epaper154FourColor::Epaper154FourColor() = default;

Epaper154FourColor::~Epaper154FourColor() {
  if (buffer_) free(buffer_);
}

esp_err_t Epaper154FourColor::begin() {
  buffer_ = static_cast<uint8_t *>(malloc(BUFFER_BYTES));
  if (!buffer_) {
    ESP_LOGE(TAG, "Unable to allocate 1.54in four-colour e-paper buffer");
    return ESP_ERR_NO_MEM;
  }

  gpio_config_t out_cfg{};
  out_cfg.intr_type = GPIO_INTR_DISABLE;
  out_cfg.mode = GPIO_MODE_OUTPUT;
  out_cfg.pin_bit_mask =
      (1ULL << TRIM_EPD_PIN_DC) |
      (1ULL << TRIM_EPD_PIN_RST);
  out_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  out_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&out_cfg));

  gpio_config_t busy_cfg{};
  busy_cfg.intr_type = GPIO_INTR_DISABLE;
  busy_cfg.mode = GPIO_MODE_INPUT;
  busy_cfg.pin_bit_mask = (1ULL << TRIM_EPD_PIN_BUSY);
  busy_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  busy_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
  ESP_ERROR_CHECK(gpio_config(&busy_cfg));

  spi_bus_config_t buscfg{};
  buscfg.mosi_io_num = TRIM_EPD_PIN_MOSI;
  buscfg.miso_io_num = -1;
  buscfg.sclk_io_num = TRIM_EPD_PIN_SCLK;
  buscfg.quadwp_io_num = -1;
  buscfg.quadhd_io_num = -1;
  buscfg.max_transfer_sz = static_cast<int>(BUFFER_BYTES);

  esp_err_t err = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
    return err;
  }

  spi_device_interface_config_t devcfg{};
  // The controller supports substantially faster SPI, but 10 MHz is a
  // deliberately conservative first-build value for hand-wired Mini stacks.
  devcfg.clock_speed_hz = 10 * 1000 * 1000;
  devcfg.mode = 0;
  devcfg.spics_io_num = TRIM_EPD_PIN_CS;
  devcfg.queue_size = 1;
  devcfg.flags = SPI_DEVICE_HALFDUPLEX;

  spi_device_handle_t dev = nullptr;
  err = spi_bus_add_device(SPI2_HOST, &devcfg, &dev);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
    return err;
  }
  spi_dev_ = reinterpret_cast<void *>(dev);

  clear(InkColor::White);

  ESP_LOGI(TAG,
           "E-paper: WeAct 1.54in FOUR-COLOUR 200x200, "
           "GDEM0154F51H / JD79660 profile");
  ESP_LOGI(TAG,
           "Palette: BLACK WHITE YELLOW RED; framebuffer=%u bytes",
           static_cast<unsigned>(BUFFER_BYTES));
  ESP_LOGI(TAG, "Pins SCLK=%d MOSI=%d CS=%d DC=%d RST=%d BUSY=%d",
           TRIM_EPD_PIN_SCLK, TRIM_EPD_PIN_MOSI, TRIM_EPD_PIN_CS,
           TRIM_EPD_PIN_DC, TRIM_EPD_PIN_RST, TRIM_EPD_PIN_BUSY);

  ESP_ERROR_CHECK(hardware_reset_());
  return initialise_();
}

void Epaper154FourColor::logical_to_physical_(int x, int y,
                                               int &px, int &py) const {
#if TRIM_EPD_ROTATE_180
  px = (PHYS_WIDTH - 1) - x;
  py = (PHYS_HEIGHT - 1) - y;
#else
  px = x;
  py = y;
#endif
}

uint8_t Epaper154FourColor::colour_code_(InkColor color) {
  // Native JD79660 two-bit colour encoding used by GDEM0154F51H:
  // 00 black, 01 white, 10 yellow, 11 red.
  switch (color) {
    case InkColor::Black:  return 0x00;
    case InkColor::White:  return 0x01;
    case InkColor::Yellow: return 0x02;
    case InkColor::Red:    return 0x03;
  }
  return 0x01;
}

void Epaper154FourColor::clear(InkColor color) {
  if (!buffer_) return;
  const uint8_t c = colour_code_(color) & 0x03;
  const uint8_t packed = static_cast<uint8_t>(
      (c << 6) | (c << 4) | (c << 2) | c);
  memset(buffer_, packed, BUFFER_BYTES);
}

void Epaper154FourColor::set_pixel(int x, int y, InkColor color) {
  if (!buffer_) return;
  if (x < 0 || y < 0 || x >= WIDTH || y >= HEIGHT) return;

  int px = 0, py = 0;
  logical_to_physical_(x, y, px, py);
  if (px < 0 || py < 0 || px >= PHYS_WIDTH || py >= PHYS_HEIGHT) return;

  const size_t pos =
      static_cast<size_t>(py) * (PHYS_WIDTH / 4) +
      static_cast<size_t>(px / 4);
  const unsigned shift = static_cast<unsigned>((3 - (px & 3)) * 2);
  const uint8_t mask = static_cast<uint8_t>(0x03u << shift);
  const uint8_t value = static_cast<uint8_t>(colour_code_(color) << shift);

  buffer_[pos] = static_cast<uint8_t>((buffer_[pos] & ~mask) | value);
}

esp_err_t Epaper154FourColor::send_command_(uint8_t cmd) {
  gpio_set_level(static_cast<gpio_num_t>(TRIM_EPD_PIN_DC), 0);

  spi_transaction_t t{};
  t.length = 8;
  t.tx_buffer = &cmd;
  return spi_device_polling_transmit(as_spi(spi_dev_), &t);
}

esp_err_t Epaper154FourColor::send_data_(const uint8_t *data, size_t len) {
  if (!len) return ESP_OK;

  gpio_set_level(static_cast<gpio_num_t>(TRIM_EPD_PIN_DC), 1);

  spi_transaction_t t{};
  t.length = static_cast<uint32_t>(len * 8);
  t.tx_buffer = data;
  return spi_device_polling_transmit(as_spi(spi_dev_), &t);
}

esp_err_t Epaper154FourColor::command_data_(uint8_t cmd,
                                            const uint8_t *data,
                                            size_t len) {
  esp_err_t err = send_command_(cmd);
  if (err != ESP_OK) return err;
  return send_data_(data, len);
}

esp_err_t Epaper154FourColor::wait_idle_(uint32_t timeout_ms) {
  // GxEPD2 gives JD79660 a short grace period before sampling BUSY.  This is
  // important not only for refresh, but also after POWER ON (0x04) and POWER
  // OFF (0x02): BUSY does not necessarily assert synchronously with SPI.
  delay_ms(1);

  const int64_t start = esp_timer_get_time();

  // JD79660 / GDEM0154F51H BUSY is active LOW.
  while (gpio_get_level(static_cast<gpio_num_t>(TRIM_EPD_PIN_BUSY)) == 0) {
    if ((esp_timer_get_time() - start) / 1000 > timeout_ms) {
      ESP_LOGE(TAG, "Timed out waiting for four-colour BUSY to release");
      return ESP_ERR_TIMEOUT;
    }
    delay_ms(10);
  }
  return ESP_OK;
}

esp_err_t Epaper154FourColor::hardware_reset_() {
  gpio_set_level(static_cast<gpio_num_t>(TRIM_EPD_PIN_RST), 1);
  delay_ms(20);
  gpio_set_level(static_cast<gpio_num_t>(TRIM_EPD_PIN_RST), 0);
  delay_ms(2);
  gpio_set_level(static_cast<gpio_num_t>(TRIM_EPD_PIN_RST), 1);
  delay_ms(20);
  return wait_idle_(2000);
}

esp_err_t Epaper154FourColor::initialise_() {
  // Conservative full-refresh setup for GDEM0154F51H / JD79660. The command
  // profile follows the public Good Display / GxEPD2 panel implementation,
  // but ES-PET talks to it directly through ESP-IDF SPI.
  esp_err_t err = ESP_OK;

  const uint8_t reg_4d[] = {0x78};
  err = command_data_(0x4D, reg_4d, sizeof(reg_4d));
  if (err != ESP_OK) return err;

  const uint8_t psr[] = {0x0F, 0x29};
  err = command_data_(0x00, psr, sizeof(psr));
  if (err != ESP_OK) return err;

  const uint8_t booster[] = {0x0D, 0x12, 0x30, 0x20, 0x19, 0x2A, 0x22};
  err = command_data_(0x06, booster, sizeof(booster));
  if (err != ESP_OK) return err;

  const uint8_t cdi[] = {0x37};
  err = command_data_(0x50, cdi, sizeof(cdi));
  if (err != ESP_OK) return err;

  const uint8_t resolution[] = {0x00, 0xC8, 0x00, 0xC8};
  err = command_data_(0x61, resolution, sizeof(resolution));
  if (err != ESP_OK) return err;

  const uint8_t e9[] = {0x01};
  err = command_data_(0xE9, e9, sizeof(e9));
  if (err != ESP_OK) return err;

  const uint8_t pll[] = {0x08};
  err = command_data_(0x30, pll, sizeof(pll));
  if (err != ESP_OK) return err;

  err = send_command_(0x04);  // power on
  if (err != ESP_OK) return err;
  return wait_idle_(2000);
}

esp_err_t Epaper154FourColor::write_buffer_() {
  // Full-screen native 2bpp RAM write: 4 pixels per byte.
  esp_err_t err = send_command_(0x10);
  if (err != ESP_OK) return err;
  return send_data_(buffer_, BUFFER_BYTES);
}

esp_err_t Epaper154FourColor::update_display_() {
  const uint8_t cdi[] = {0x37};
  esp_err_t err = command_data_(0x50, cdi, sizeof(cdi));
  if (err != ESP_OK) return err;

  err = send_command_(0x12);  // display refresh
  if (err != ESP_OK) return err;

  const uint8_t zero[] = {0x00};
  err = send_data_(zero, sizeof(zero));
  if (err != ESP_OK) return err;

  // JD79660 BUSY is active-low. RC4 now has a *real* 1 ms/sub-tick delay,
  // so this assertion window is meaningful instead of a tight zero-delay loop.
  // If it never goes LOW, refresh() performs one full wake/init/rewrite retry.
  constexpr uint32_t kBusyAssertTimeoutMs = 100;
  const int64_t assert_start = esp_timer_get_time();
  while ((esp_timer_get_time() - assert_start) / 1000 <=
         kBusyAssertTimeoutMs) {
    if (gpio_get_level(static_cast<gpio_num_t>(TRIM_EPD_PIN_BUSY)) == 0) {
      const uint32_t asserted_after_ms = static_cast<uint32_t>(
          (esp_timer_get_time() - assert_start) / 1000);
      ESP_LOGI(TAG, "Refresh waveform started; BUSY=0 after %u ms",
               static_cast<unsigned>(asserted_after_ms));
      return wait_idle_(30000);
    }
    delay_ms(1);
  }

  ESP_LOGW(TAG,
           "Refresh BUSY did not assert LOW within %u ms; controller recovery needed",
           static_cast<unsigned>(kBusyAssertTimeoutMs));
  return ESP_ERR_TIMEOUT;
}

esp_err_t Epaper154FourColor::power_off_() {
  esp_err_t err = send_command_(0x02);
  if (err != ESP_OK) return err;

  const uint8_t zero[] = {0x00};
  err = send_data_(zero, sizeof(zero));
  if (err != ESP_OK) return err;

  return wait_idle_(2000);
}

esp_err_t Epaper154FourColor::refresh() {
  ESP_LOGI(TAG, "Refreshing 200x200 FOUR-COLOUR display");

  // The panel is put into deep sleep after a successful update.  Waking a
  // JD79660 therefore requires a genuine RESET pulse before reinitialisation.
  // RC4 fixes the old sub-tick delay bug so the 2 ms RESET-low pulse really
  // occurs, then keeps one complete recovery retry as a safety net.
  esp_err_t last_err = ESP_FAIL;

  for (int attempt = 1; attempt <= 2; ++attempt) {
    last_err = hardware_reset_();
    if (last_err == ESP_OK) last_err = initialise_();
    if (last_err == ESP_OK) last_err = write_buffer_();
    if (last_err == ESP_OK) last_err = update_display_();

    if (last_err == ESP_OK) {
      ESP_LOGI(TAG, "Four-colour display refresh complete");
      last_err = sleep();
      if (last_err == ESP_OK) return ESP_OK;

      ESP_LOGW(TAG, "Display refreshed but panel sleep failed: %s",
               esp_err_to_name(last_err));
    } else {
      ESP_LOGW(TAG, "Four-colour refresh attempt %d failed: %s",
               attempt, esp_err_to_name(last_err));
    }

    if (attempt < 2) {
      ESP_LOGW(TAG,
               "Four-colour controller recovery: reset/init/rewrite retry 1/1");
      delay_ms(50);
    }
  }

  // A display fault must never reboot the pet.  The caller logs the failure
  // and leaves the screen state dirty so the next epoch can try again.
  ESP_LOGE(TAG,
           "Four-colour display update abandoned after recovery; pet continues");
  return last_err;
}

esp_err_t Epaper154FourColor::sleep() {
  esp_err_t err = power_off_();
  if (err != ESP_OK) return err;

  err = send_command_(0x07);  // deep sleep
  if (err != ESP_OK) return err;

  const uint8_t key[] = {0xA5};
  err = send_data_(key, sizeof(key));
  if (err == ESP_OK) ESP_LOGI(TAG, "Four-colour e-paper sleeping");
  return err;
}
