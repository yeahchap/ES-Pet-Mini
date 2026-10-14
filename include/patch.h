#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

struct PatchInfo {
  bool active = false;
  uint32_t sequence = 0;
  uint32_t payload_size = 0;
};

esp_err_t patch_begin();
const PatchInfo &patch_info();