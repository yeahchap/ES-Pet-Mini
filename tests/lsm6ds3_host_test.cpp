#include <array>
#include <cassert>
#include <cstdio>
#include "lsm6ds3_steps.h"
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
int main() {
  config_test();
  accounting_test();
  std::puts("PASS: LSM6DS3 pedometer/tap register coexistence, IRQ routing, bank restore, I2C errors, step delivery and rollover");
}
