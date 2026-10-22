#ifdef ES_PET_ULTIMATE_XIAO
#include "lsm6ds3_support.h"

int32_t espet_lsm6ds3_configure(const stmdev_ctx_t *ctx) {
  int32_t ret = 0;
#define SET(call) do { ret = (call); if (ret != 0) return ret; } while (0)
  SET(lsm6ds3_block_data_update_set(ctx, 1));
  SET(lsm6ds3_auto_increment_set(ctx, 1));
  SET(lsm6ds3_gy_data_rate_set(ctx, LSM6DS3_GY_ODR_OFF));
  SET(lsm6ds3_xl_full_scale_set(ctx, LSM6DS3_2g));
  // Double-tap recognition uses a fast accelerometer ODR. Do not describe this
  // as 26 Hz low-power pedometer operation: both features stay live in snooze.
  SET(lsm6ds3_xl_power_mode_set(ctx, LSM6DS3_XL_HIGH_PERFORMANCE));
  SET(lsm6ds3_pedo_full_scale_set(ctx, LSM6DS3_PEDO_AT_2g));
  SET(lsm6ds3_pedo_threshold_set(ctx, 0x10));
  SET(lsm6ds3_pedo_debounce_steps_set(ctx, 6));
  SET(lsm6ds3_pedo_sens_set(ctx, 1)); // Sets BOTH FUNC_EN and PEDO_EN.
  // All subsequent TAP_CFG updates are read-modify-write, preserving PEDO_EN.
  SET(lsm6ds3_tap_detection_on_x_set(ctx, 1));
  SET(lsm6ds3_tap_detection_on_y_set(ctx, 1));
  SET(lsm6ds3_tap_detection_on_z_set(ctx, 1));
  SET(lsm6ds3_tap_threshold_set(ctx, 9));
  SET(lsm6ds3_tap_shock_set(ctx, 3));
  SET(lsm6ds3_tap_quiet_set(ctx, 3));
  SET(lsm6ds3_tap_dur_set(ctx, 8));
  // ST's enum name is misleading: value 1 sets SINGLE_DOUBLE_TAP=1 (both
  // events enabled per DS026899). Only DOUBLE_TAP is routed to INT1 below.
  SET(lsm6ds3_tap_mode_set(ctx, LSM6DS3_ONLY_DOUBLE));
  SET(lsm6ds3_int_notification_set(ctx, LSM6DS3_INT_LATCHED));
  SET(lsm6ds3_pin_mode_set(ctx, LSM6DS3_PUSH_PULL));
  SET(lsm6ds3_pin_polarity_set(ctx, LSM6DS3_ACTIVE_HIGH));
  lsm6ds3_int1_route_t route{};
  route.int1_double_tap = 1;
  // No per-step or data-ready IRQ: keep the XIAO asleep between timer polls.
  SET(lsm6ds3_pin_int1_route_set(ctx, &route));
  SET(lsm6ds3_xl_data_rate_set(ctx, LSM6DS3_XL_ODR_416Hz));
#undef SET
  return 0;
}
#endif
