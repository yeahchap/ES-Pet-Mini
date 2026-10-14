#pragma once

using esp_err_t = int;

constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_INVALID_ARG = 0x102;
constexpr esp_err_t ESP_ERR_NVS_NOT_FOUND = 0x1102;

inline const char *esp_err_to_name(esp_err_t) {
  return "test-error";
}
