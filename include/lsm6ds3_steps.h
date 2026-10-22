#pragma once
#include <cstdint>

// One consumer (the game snapshot), multiple observations (awake and snoozing).
// Sensor observations never drain the pending game credits.
class Lsm6ds3Steps {
 public:
  void baseline(uint16_t raw) { raw_ = raw; total_ = pending_ = 0; }
  uint16_t observe(uint16_t raw, uint32_t max_delta = 65535) {
    const uint16_t delta = static_cast<uint16_t>(raw - raw_);
    raw_ = raw;
    // A reset/torn read must not award nearly 65k steps. Rebase the raw count
    // while preserving previously earned totals and undelivered game credits.
    if (delta > max_delta) return 0;
    total_ += delta;
    pending_ += delta;
    return delta;
  }
  uint32_t take_pending() { const auto n = pending_; pending_ = 0; return n; }
  uint32_t total() const { return total_; }
  uint16_t raw() const { return raw_; }
 private:
  uint16_t raw_ = 0;
  uint32_t total_ = 0, pending_ = 0;
};
