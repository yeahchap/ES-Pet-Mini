#pragma once
#include "vendor/lsm6ds3/lsm6ds3_reg.h"

// Call after software reset has completed. Does not reset the hardware counter.
// Returns ST transport status (0 = success).
int32_t espet_lsm6ds3_configure(const stmdev_ctx_t *ctx);
