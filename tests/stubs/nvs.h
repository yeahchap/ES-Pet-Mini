#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

using nvs_handle_t = uint32_t;

enum nvs_open_mode_t {
  NVS_READONLY = 0,
  NVS_READWRITE = 1,
};

esp_err_t nvs_open(const char *name,
                   nvs_open_mode_t mode,
                   nvs_handle_t *handle);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_commit(nvs_handle_t handle);

esp_err_t nvs_get_u8(nvs_handle_t handle,
                     const char *key,
                     uint8_t *value);
esp_err_t nvs_get_u32(nvs_handle_t handle,
                      const char *key,
                      uint32_t *value);
esp_err_t nvs_get_blob(nvs_handle_t handle,
                       const char *key,
                       void *value,
                       size_t *length);

esp_err_t nvs_set_u8(nvs_handle_t handle,
                     const char *key,
                     uint8_t value);
esp_err_t nvs_set_u32(nvs_handle_t handle,
                      const char *key,
                      uint32_t value);
esp_err_t nvs_set_blob(nvs_handle_t handle,
                       const char *key,
                       const void *value,
                       size_t length);

void fake_nvs_reset();
