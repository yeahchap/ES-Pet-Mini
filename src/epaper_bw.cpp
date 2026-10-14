#include "epaper.h"
#include "config.h"

#include <cstdlib>
#include <cstring>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "trim_epaper";

namespace {

inline void delay_ms(uint32_t ms) {
  vTaskDelay(pdMS_TO_TICKS(ms));
}

inline spi_device_handle_t as_spi(void *p) {
  return reinterpret_cast<spi_device_handle_t>(p);
}

}  // namespace

Epaper154Mono::Epaper154Mono() = default;

Epaper154Mono::~Epaper154Mono() {
  if (buffer_) free(buffer_);
}

esp_err_t Epaper154Mono::begin() {
  buffer_ = static_cast<uint8_t *>(malloc(BUFFER_BYTES));
  if (!buffer_) {
    ESP_LOGE(TAG, "Unable to allocate 1.54in e-paper buffer");
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
  busy_cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
  busy_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
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
           "E-paper: WeAct 1.54in B/W 200x200, SSD1681 / GDEH0154D67 profile");
  ESP_LOGI(TAG, "Pins SCLK=%d MOSI=%d CS=%d DC=%d RST=%d BUSY=%d",
           TRIM_EPD_PIN_SCLK, TRIM_EPD_PIN_MOSI, TRIM_EPD_PIN_CS,
           TRIM_EPD_PIN_DC, TRIM_EPD_PIN_RST, TRIM_EPD_PIN_BUSY);

  ESP_ERROR_CHECK(hardware_reset_());
  return initialise_();
}

void Epaper154Mono::logical_to_physical_(int x, int y,
                                         int &px, int &py) const {
#if TRIM_EPD_ROTATE_180
  px = (PHYS_WIDTH - 1) - x;
  py = (PHYS_HEIGHT - 1) - y;
#else
  px = x;
  py = y;
#endif
}

void Epaper154Mono::clear(InkColor color) {
  if (!buffer_) return;
  // SSD1681 B/W RAM: 1 = white, 0 = black.
  memset(buffer_, color == InkColor::White ? 0xFF : 0x00, BUFFER_BYTES);
}

void Epaper154Mono::set_pixel(int x, int y, InkColor color) {
  if (!buffer_) return;
  if (x < 0 || y < 0 || x >= WIDTH || y >= HEIGHT) return;

  int px = 0, py = 0;
  logical_to_physical_(x, y, px, py);

  if (px < 0 || py < 0 || px >= PHYS_WIDTH || py >= PHYS_HEIGHT) return;

  const size_t pos =
      static_cast<size_t>(py) * (PHYS_WIDTH / 8) +
      static_cast<size_t>(px / 8);
  const uint8_t bit = static_cast<uint8_t>(0x80u >> (px & 7));

  if (color == InkColor::White) {
    buffer_[pos] |= bit;
  } else {
    // "Red" from the shared V0.7 UI becomes black on the mono panel.
    buffer_[pos] &= static_cast<uint8_t>(~bit);
  }
}

esp_err_t Epaper154Mono::send_command_(uint8_t cmd) {
  gpio_set_level(static_cast<gpio_num_t>(TRIM_EPD_PIN_DC), 0);

  spi_transaction_t t{};
  t.length = 8;
  t.tx_buffer = &cmd;
  return spi_device_polling_transmit(as_spi(spi_dev_), &t);
}

esp_err_t Epaper154Mono::send_data_(const uint8_t *data, size_t len) {
  if (!len) return ESP_OK;

  gpio_set_level(static_cast<gpio_num_t>(TRIM_EPD_PIN_DC), 1);

  spi_transaction_t t{};
  t.length = static_cast<uint32_t>(len * 8);
  t.tx_buffer = data;
  return spi_device_polling_transmit(as_spi(spi_dev_), &t);
}

esp_err_t Epaper154Mono::command_data_(uint8_t cmd,
                                      const uint8_t *data,
                                      size_t len) {
  esp_err_t err = send_command_(cmd);
  if (err != ESP_OK) return err;
  return send_data_(data, len);
}

esp_err_t Epaper154Mono::wait_idle_(uint32_t timeout_ms) {
  const int64_t start = esp_timer_get_time();

  // GDEH0154D67 / SSD1681 BUSY is active high.
  while (gpio_get_level(static_cast<gpio_num_t>(TRIM_EPD_PIN_BUSY)) != 0) {
    if ((esp_timer_get_time() - start) / 1000 > timeout_ms) {
      ESP_LOGE(TAG, "Timed out waiting for BUSY to go low");
      return ESP_ERR_TIMEOUT;
    }
    delay_ms(10);
  }
  return ESP_OK;
}

esp_err_t Epaper154Mono::hardware_reset_() {
  gpio_set_level(static_cast<gpio_num_t>(TRIM_EPD_PIN_RST), 1);
  delay_ms(20);
  gpio_set_level(static_cast<gpio_num_t>(TRIM_EPD_PIN_RST), 0);
  delay_ms(20);
  gpio_set_level(static_cast<gpio_num_t>(TRIM_EPD_PIN_RST), 1);
  delay_ms(20);
  return wait_idle_(5000);
}

esp_err_t Epaper154Mono::initialise_() {
  // Profile follows the GDEH0154D67 / SSD1681 init sequence used by the
  // WeAct-recommended GxEPD2 driver.
  ESP_ERROR_CHECK(send_command_(0x12));  // soft reset
  delay_ms(10);
  ESP_ERROR_CHECK(wait_idle_(5000));

  // Driver output control: 200 gate lines -> 199 = 0x00C7.
  const uint8_t drv[] = {0xC7, 0x00, 0x00};
  ESP_ERROR_CHECK(command_data_(0x01, drv, sizeof(drv)));

  const uint8_t border[] = {0x05};
  ESP_ERROR_CHECK(command_data_(0x3C, border, sizeof(border)));

  const uint8_t temp[] = {0x80};
  ESP_ERROR_CHECK(command_data_(0x18, temp, sizeof(temp)));

  return set_window_();
}

esp_err_t Epaper154Mono::set_window_() {
  const uint8_t entry[] = {0x03};  // X increment, Y increment
  ESP_ERROR_CHECK(command_data_(0x11, entry, sizeof(entry)));

  const uint8_t x_range[] = {
      0x00,
      static_cast<uint8_t>((PHYS_WIDTH - 1) / 8)  // 24 for 200px
  };
  ESP_ERROR_CHECK(command_data_(0x44, x_range, sizeof(x_range)));

  const uint16_t y_end = PHYS_HEIGHT - 1;  // 199
  const uint8_t y_range[] = {
      0x00, 0x00,
      static_cast<uint8_t>(y_end & 0xFF),
      static_cast<uint8_t>((y_end >> 8) & 0xFF)
  };
  ESP_ERROR_CHECK(command_data_(0x45, y_range, sizeof(y_range)));

  const uint8_t x_counter[] = {0x00};
  ESP_ERROR_CHECK(command_data_(0x4E, x_counter, sizeof(x_counter)));

  const uint8_t y_counter[] = {0x00, 0x00};
  ESP_ERROR_CHECK(command_data_(0x4F, y_counter, sizeof(y_counter)));

  return ESP_OK;
}

esp_err_t Epaper154Mono::write_buffer_() {
  // For a clean full refresh, seed both the controller's previous and current
  // RAM with our full frame. This is the same pattern used by GxEPD2's
  // "writeImageForFullRefresh" path for this panel.
  ESP_ERROR_CHECK(set_window_());
  ESP_ERROR_CHECK(send_command_(0x26));
  ESP_ERROR_CHECK(send_data_(buffer_, BUFFER_BYTES));

  ESP_ERROR_CHECK(set_window_());
  ESP_ERROR_CHECK(send_command_(0x24));
  ESP_ERROR_CHECK(send_data_(buffer_, BUFFER_BYTES));

  return ESP_OK;
}

esp_err_t Epaper154Mono::update_display_() {
  const uint8_t ctl[] = {0xF7};  // full update
  ESP_ERROR_CHECK(command_data_(0x22, ctl, sizeof(ctl)));
  ESP_ERROR_CHECK(send_command_(0x20));
  return wait_idle_(10000);
}

esp_err_t Epaper154Mono::power_off_() {
  const uint8_t ctl[] = {0x83};
  ESP_ERROR_CHECK(command_data_(0x22, ctl, sizeof(ctl)));
  ESP_ERROR_CHECK(send_command_(0x20));
  return wait_idle_(5000);
}

esp_err_t Epaper154Mono::refresh() {
  ESP_LOGI(TAG, "Refreshing 200x200 display");

  // Panel is deep-slept after each update, so every refresh starts by waking
  // it with RST and repeating the short SSD1681 init sequence.
  ESP_ERROR_CHECK(hardware_reset_());
  ESP_ERROR_CHECK(initialise_());
  ESP_ERROR_CHECK(write_buffer_());
  ESP_ERROR_CHECK(update_display_());
  ESP_ERROR_CHECK(power_off_());

  ESP_LOGI(TAG, "Display refresh complete");
  return sleep();
}

esp_err_t Epaper154Mono::sleep() {
  const uint8_t key[] = {0x01};
  esp_err_t err = command_data_(0x10, key, sizeof(key));
  if (err == ESP_OK) ESP_LOGI(TAG, "E-paper sleeping");
  return err;
}
