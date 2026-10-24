#pragma once

#include <algorithm>
#include <cstdint>

// One fresh hardware tap interrupt owns one complete display cycle. Keep
// counting steps independently while chirp/refresh handling consumes taps.
class Lsm6ds3Taps {
 public:
  static constexpr int64_t kSettleUs = 1500000;

  bool observe(bool interrupt_active, bool double_tap, int64_t now_us) {
    if (!interrupt_active || !double_tap || cycle_active_ || feedback_depth_ ||
        now_us < rearm_after_us_) return false;
    pending_ = cycle_active_ = true;
    return true;
  }
  bool pending() const { return pending_; }
  bool take() {
    const bool value = pending_;
    pending_ = false;
    return value;
  }
  void begin_refresh() { cycle_active_ = true; }
  void finish_refresh(int64_t now_us) {
    pending_ = cycle_active_ = false;
    settle_(now_us);
  }
  void begin_feedback() { ++feedback_depth_; }
  void finish_feedback(int64_t now_us) {
    if (feedback_depth_) --feedback_depth_;
    settle_(now_us);
  }

 private:
  void settle_(int64_t now_us) {
    rearm_after_us_ = std::max(rearm_after_us_, now_us + kSettleUs);
  }
  bool pending_ = false;
  bool cycle_active_ = false;
  unsigned feedback_depth_ = 0;
  int64_t rearm_after_us_ = 0;
};
