#pragma once

template <typename... Args>
inline void esp_test_log(const char *, const char *, Args...) {}

#define ESP_LOGI(tag, format, ...) \
  esp_test_log(tag, format, ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) \
  esp_test_log(tag, format, ##__VA_ARGS__)
#define ESP_LOGE(tag, format, ...) \
  esp_test_log(tag, format, ##__VA_ARGS__)
