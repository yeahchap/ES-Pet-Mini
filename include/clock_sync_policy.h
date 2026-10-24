#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace clock_sync {

// Scan records may fill their entire SSID array without a terminating NUL.
// Matching is exact and case-sensitive; hidden SSIDs are empty.
inline bool matches_saved_ssid(const uint8_t *observed, size_t capacity,
                               const char *saved) {
  if (!observed || !saved || saved[0] == '\0') return false;
  const size_t saved_length = std::strlen(saved);
  size_t observed_length = 0;
  while (observed_length < capacity && observed[observed_length] != 0) {
    ++observed_length;
  }
  return observed_length == saved_length &&
         std::memcmp(observed, saved, saved_length) == 0;
}

inline bool due(bool configured, bool home_confident, bool saved_home_visible,
                bool clock_valid, int64_t now_us, int64_t last_sync_us,
                int64_t last_attempt_us) {
  if (!configured || (!home_confident && !saved_home_visible)) return false;
  // Keep the existing radio budget, including when the clock is invalid.
  constexpr int64_t RETRY_BACKOFF_US = 30LL * 60LL * 1000000LL;
  if (last_attempt_us >= 0 && now_us - last_attempt_us < RETRY_BACKOFF_US) {
    return false;
  }
  if (!clock_valid || last_sync_us < 0) return true;
  constexpr int64_t SIX_HOURS_US = 6LL * 60LL * 60LL * 1000000LL;
  return now_us - last_sync_us >= SIX_HOURS_US;
}

}  // namespace clock_sync
