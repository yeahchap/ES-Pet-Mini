#include "onboard_led.h"

#include <cstdint>

#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

static const char *TAG = "trim_led";

static constexpr gpio_num_t RGB_GPIO = GPIO_NUM_8;
static constexpr uint32_t RMT_RESOLUTION_HZ = 10000000;  // 10 MHz = 0.1 us/tick

esp_err_t onboard_rgb_off() {
#if defined(ES_PET_ULTIMATE_XIAO)
  // XIAO does NOT have the SuperMini RGB LED on GPIO8.
  // Its user LED is on GPIO15; avoid configuring nonexistent RGB hardware.
  ESP_ERROR_CHECK(gpio_reset_pin(GPIO_NUM_15));
  ESP_ERROR_CHECK(gpio_set_direction(GPIO_NUM_15, GPIO_MODE_OUTPUT));
  ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_15, 1)); // active-low user LED
  return ESP_OK;
#else
  ESP_LOGI(TAG, "Extinguishing onboard RGB LED on GPIO8");

  rmt_channel_handle_t channel = nullptr;

  rmt_tx_channel_config_t channel_cfg{};
  channel_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
  channel_cfg.gpio_num = RGB_GPIO;
  channel_cfg.mem_block_symbols = 64;
  channel_cfg.resolution_hz = RMT_RESOLUTION_HZ;
  channel_cfg.trans_queue_depth = 1;

  esp_err_t err = rmt_new_tx_channel(&channel_cfg, &channel);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Could not create RMT channel: %s", esp_err_to_name(err));
    return err;
  }

  rmt_bytes_encoder_config_t encoder_cfg{};
  encoder_cfg.bit0.level0 = 1;
  encoder_cfg.bit0.duration0 = 3;  // 0.3 us
  encoder_cfg.bit0.level1 = 0;
  encoder_cfg.bit0.duration1 = 9;  // 0.9 us

  encoder_cfg.bit1.level0 = 1;
  encoder_cfg.bit1.duration0 = 9;  // 0.9 us
  encoder_cfg.bit1.level1 = 0;
  encoder_cfg.bit1.duration1 = 3;  // 0.3 us
  encoder_cfg.flags.msb_first = 1;

  rmt_encoder_handle_t encoder = nullptr;
  err = rmt_new_bytes_encoder(&encoder_cfg, &encoder);
  if (err != ESP_OK) {
    rmt_del_channel(channel);
    return err;
  }

  err = rmt_enable(channel);
  if (err != ESP_OK) {
    rmt_del_encoder(encoder);
    rmt_del_channel(channel);
    return err;
  }

  static const uint8_t black[3] = {0x00, 0x00, 0x00};

  rmt_transmit_config_t tx_cfg{};
  tx_cfg.loop_count = 0;

  err = rmt_transmit(channel, encoder, black, sizeof(black), &tx_cfg);
  if (err == ESP_OK) {
    err = rmt_tx_wait_all_done(channel, 100);
  }

  // Latch the zero frame, then keep the data line low permanently.
  esp_rom_delay_us(100);

  rmt_disable(channel);
  rmt_del_encoder(encoder);
  rmt_del_channel(channel);

  gpio_reset_pin(RGB_GPIO);
  gpio_set_direction(RGB_GPIO, GPIO_MODE_OUTPUT);
  gpio_set_level(RGB_GPIO, 0);
  esp_rom_delay_us(100);

  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Onboard RGB LED OFF");
  } else {
    ESP_LOGW(TAG, "RGB off transmission returned: %s", esp_err_to_name(err));
  }

  return err;
#endif
}
