#include <array>
#include <cassert>
#include <cstdio>
#include "lsm6ds3_steps.h"
#include "lsm6ds3_taps.h"
#include "lsm6ds3_support.h"

struct Registers {
  std::array<uint8_t, 256> user{}, embedded{};
  bool bank = false;
  int fail_register = -1;
};
static int32_t read(void *p, uint8_t reg, uint8_t *data, uint16_t size) {
  auto &r = *static_cast<Registers *>(p);
  if (reg == r.fail_register) return -77;
  for (unsigned i = 0; i < size; ++i) {
    data[i] = reg + i == 1 ? static_cast<uint8_t>(r.bank ? 0x80 : 0) :
        (r.bank ? r.embedded : r.user)[reg + i];
  }
  return 0;
}
static int32_t write(void *p, uint8_t reg, const uint8_t *data, uint16_t size) {
  auto &r = *static_cast<Registers *>(p);
  if (reg == r.fail_register) return -77;
  for (unsigned i = 0; i < size; ++i) {
    if (reg + i == 1) r.bank = (data[i] & 0x80) != 0;
    else (r.bank ? r.embedded : r.user)[reg + i] = data[i];
  }
  return 0;
}
static void config_test() {
  Registers r;
  // Nonzero unrelated bits must survive the ST driver's read-modify-writes.
  r.user[LSM6DS3_CTRL3_C] = 0x04;
  r.user[LSM6DS3_TAP_CFG] = 0x80;
  r.user[LSM6DS3_CTRL10_C] = 0x38;
  r.user[LSM6DS3_STEP_COUNTER_L] = 123;
  stmdev_ctx_t ctx{};
  ctx.handle = &r; ctx.read_reg = read; ctx.write_reg = write;
  assert(espet_lsm6ds3_configure(&ctx) == 0);
  assert(!r.bank);
  assert((r.user[LSM6DS3_CTRL1_XL] & 0xfc) == 0x60);
  assert((r.user[LSM6DS3_CTRL2_G] & 0xf0) == 0);
  assert((r.user[LSM6DS3_CTRL3_C] & 0x74) == 0x44);
  assert((r.user[LSM6DS3_CTRL10_C] & 0x04) == 0x04);
  assert((r.user[LSM6DS3_CTRL10_C] & 0x38) == 0x38);
  assert((r.user[LSM6DS3_TAP_CFG] & 0x4f) == 0x4f);
  assert((r.user[LSM6DS3_TAP_CFG] & 0x80) == 0x80);
  assert((r.user[LSM6DS3_WAKE_UP_THS] & 0x80) != 0);
  assert(r.user[LSM6DS3_MD1_CFG] == 0x08); // double tap only
  assert(r.user[LSM6DS3_INT1_CTRL] == 0); // no per-step/data-ready wake
  assert((r.embedded[LSM6DS3_PEDO_THS_REG] & 0x9f) == 0x10);
  assert((r.embedded[LSM6DS3_PEDO_DEB_REG] & 7) == 6);
  assert(r.user[LSM6DS3_STEP_COUNTER_L] == 123); // no reset on reconfiguration
  r.fail_register = LSM6DS3_TAP_THS_6D;
  assert(espet_lsm6ds3_configure(&ctx) != 0); // transport failures propagate
}
static void accounting_test() {
  Lsm6ds3Steps steps;
  steps.baseline(41);                       // existing raw value is not new XP
  assert(steps.observe(45) == 4);            // active observations
  assert(steps.observe(45) == 0);            // repeated poll is not another step
  assert(steps.observe(125) == 80);          // steps accumulated during sleep
  assert(steps.observe(130) == 5);           // exit sleep before game snapshot
  assert(steps.take_pending() == 89);        // one delivery across all observers
  assert(steps.take_pending() == 0);
  assert(steps.total() == 89);
  assert(steps.raw() == 130);
  steps.baseline(65533);
  assert(steps.observe(2) == 5);             // natural 16-bit hardware wrap
  assert(steps.take_pending() == 5);
  assert(steps.observe(2) == 0);
  assert(steps.total() == 5);
  steps.baseline(0);
  assert(steps.observe(100, 200) == 100);
  assert(steps.observe(0, 64) == 0);          // unexpected sensor reset
  assert(steps.total() == 100);              // no ~65k phantom steps
  assert(steps.take_pending() == 100);        // existing earned credits survive
  assert(steps.observe(4, 64) == 4);          // continue from new raw baseline
  assert(steps.take_pending() == 4);
}
static void tap_cycle_test() {
  Lsm6ds3Taps taps;
  Lsm6ds3Steps steps;
  steps.baseline(0);

  // An event-type bit by itself is not a new latched interrupt.
  assert(!taps.observe(false, true, 0));
  assert(!taps.observe(true, false, 0));
  assert(taps.observe(true, true, 0));
  assert(taps.pending());

  // The acknowledgement chirp must retain the original pending request,
  // suppress its own vibration, and continue banking real steps.
  taps.begin_feedback();
  assert(!taps.observe(true, true, 100000));
  assert(steps.observe(12) == 12);
  taps.finish_feedback(100000);
  assert(taps.pending());
  assert(taps.take());
  assert(!taps.take());

  taps.begin_refresh();
  for (int64_t t = 200000; t <= 20000000; t += 200000) {
    assert(!taps.observe(true, true, t));
  }
  assert(steps.observe(32) == 20);
  taps.finish_refresh(20000000);
  assert(!taps.pending());
  // Setting the case down just after completion must not redraw again.
  assert(!taps.observe(true, true, 20100000));
  assert(!taps.observe(true, true, 21499999));
  assert(taps.observe(true, true, 21500000));
  assert(taps.take());
  taps.finish_refresh(42000000);
  assert(steps.take_pending() == 32);
  assert(steps.take_pending() == 0);

  // An unrelated game chirp also cannot synthesize a display request.
  taps.begin_feedback();
  taps.begin_feedback();
  assert(!taps.observe(true, true, 50000000));
  taps.finish_feedback(50100000);
  assert(!taps.observe(true, true, 52000000));
  taps.finish_feedback(52000000);
  assert(!taps.observe(true, true, 53499999));
  assert(!taps.observe(false, true, 53500000));
  assert(taps.observe(true, true, 53500000));
}
int main() {
  config_test();
  accounting_test();
  tap_cycle_test();
  std::puts("PASS: LSM6DS3 configuration, step delivery/rollover and single-refresh tap cycles");
}
