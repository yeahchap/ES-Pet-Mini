#include "reward.h"

#include <algorithm>
#include <climits>

EpochReward calculate_epoch_reward(uint32_t new_friends,
                                   uint32_t known_today,
                                   uint32_t repeat_today,
                                   uint32_t new_channels) {
  EpochReward r;
  r.new_friends = new_friends;
  r.known_today = known_today;
  r.repeat_today = repeat_today;
  r.familiar_friends = known_today + repeat_today;
  r.new_channels = new_channels;

  // Novel places should matter more, but a familiar everyday route still
  // deserves some credit once per day. Repeated minute-by-minute scans do not.
  const uint64_t xp =
      static_cast<uint64_t>(new_friends) * 4ULL +
      static_cast<uint64_t>(known_today) * 2ULL +
      static_cast<uint64_t>(new_channels) * 2ULL;

  r.xp = static_cast<uint32_t>(
      std::min<uint64_t>(xp, 250ULL));

  return r;
}

uint32_t calculate_ble_reward(uint32_t new_ever,
                              uint32_t known_today) {
  const uint64_t xp =
      static_cast<uint64_t>(new_ever) * 2ULL +
      static_cast<uint64_t>(known_today);

  return static_cast<uint32_t>(
      std::min<uint64_t>(xp, 120ULL));
}

uint32_t level_start_xp(uint32_t level) {
  if (level <= 1) return 0;
  uint64_t n = static_cast<uint64_t>(level - 1);
  uint64_t threshold = 50ULL * n * (n + 1ULL);
  return threshold > UINT32_MAX ? UINT32_MAX
                                : static_cast<uint32_t>(threshold);
}

uint32_t level_from_xp(uint32_t xp) {
  uint32_t level = 1;
  while (level < 1000) {
    uint32_t next = level_start_xp(level + 1);
    if (next == UINT32_MAX || xp < next) break;
    ++level;
  }
  return level;
}

uint32_t next_level_xp(uint32_t level) {
  return level_start_xp(level + 1);
}
