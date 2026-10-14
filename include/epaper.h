#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

// Common ES-PET palette.
// On the monochrome panel, all non-white colours render as black.
enum class InkColor : uint8_t {
  White  = 0,
  Black  = 1,
  Red    = 2,
  Yellow = 3,
};


// ============================================================
// Original Mini monochrome backend
// WeAct 1.54" 200x200, SSD1681 / GDEH0154D67
// ============================================================

class Epaper154Mono {
 public:
  static constexpr int PHYS_WIDTH  = 200;
  static constexpr int PHYS_HEIGHT = 200;
  static constexpr int WIDTH       = 200;
  static constexpr int HEIGHT      = 200;

  Epaper154Mono();
  ~Epaper154Mono();

  esp_err_t begin();

  void clear(InkColor color = InkColor::White);
  void set_pixel(int x, int y, InkColor color);

  esp_err_t refresh();
  esp_err_t sleep();

 private:
  static constexpr size_t BUFFER_BYTES =
      (PHYS_WIDTH * PHYS_HEIGHT) / 8;

  uint8_t *buffer_ = nullptr;
  void *spi_dev_ = nullptr;

  void logical_to_physical_(int x, int y, int &px, int &py) const;

  esp_err_t hardware_reset_();
  esp_err_t initialise_();
  esp_err_t wait_idle_(uint32_t timeout_ms);

  esp_err_t send_command_(uint8_t cmd);
  esp_err_t send_data_(const uint8_t *data, size_t len);
  esp_err_t command_data_(uint8_t cmd, const uint8_t *data, size_t len);

  esp_err_t set_window_();
  esp_err_t write_buffer_();
  esp_err_t update_display_();
  esp_err_t power_off_();
};


// ============================================================
// Current Mini four-colour backend
// WeAct / Good Display 1.54" 200x200, JD79660 / GDEM0154F51H
// ============================================================

class Epaper154FourColor {
 public:
  static constexpr int PHYS_WIDTH  = 200;
  static constexpr int PHYS_HEIGHT = 200;
  static constexpr int WIDTH       = 200;
  static constexpr int HEIGHT      = 200;

  Epaper154FourColor();
  ~Epaper154FourColor();

  esp_err_t begin();

  void clear(InkColor color = InkColor::White);
  void set_pixel(int x, int y, InkColor color);

  esp_err_t refresh();
  esp_err_t sleep();

 private:
  static constexpr size_t BUFFER_BYTES =
      (PHYS_WIDTH * PHYS_HEIGHT) / 4;

  uint8_t *buffer_ = nullptr;
  void *spi_dev_ = nullptr;

  void logical_to_physical_(int x, int y, int &px, int &py) const;
  static uint8_t colour_code_(InkColor color);

  esp_err_t hardware_reset_();
  esp_err_t initialise_();
  esp_err_t wait_idle_(uint32_t timeout_ms);

  esp_err_t send_command_(uint8_t cmd);
  esp_err_t send_data_(const uint8_t *data, size_t len);
  esp_err_t command_data_(uint8_t cmd, const uint8_t *data, size_t len);

  esp_err_t write_buffer_();
  esp_err_t update_display_();
  esp_err_t power_off_();
};


// ============================================================
// RC7 universal display facade
//
// GPIO1 OPEN/HIGH = four-colour Mini (default)
// GPIO1 LOW/GND   = original B&W Mini
// ============================================================

class Epaper29Tri {
 public:
  static constexpr int PHYS_WIDTH  = 200;
  static constexpr int PHYS_HEIGHT = 200;
  static constexpr int WIDTH       = 200;
  static constexpr int HEIGHT      = 200;

  Epaper29Tri() = default;

  esp_err_t begin();

  void clear(InkColor color = InkColor::White);
  void set_pixel(int x, int y, InkColor color);

  esp_err_t refresh();
  esp_err_t sleep();

  bool is_four_colour() const { return four_colour_; }

 private:
  bool four_colour_ = true;

  Epaper154FourColor four_;
  Epaper154Mono mono_;
};
