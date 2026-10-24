#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "clock_sync_policy.h"

int main() {
  constexpr int64_t MINUTE = 60LL * 1000000LL;
  constexpr int64_t HOUR = 60LL * MINUTE;
  std::array<uint8_t, 32> ap{};
  std::memcpy(ap.data(), "HomeNet-2G", sizeof("HomeNet-2G") - 1);
  const bool visible = clock_sync::matches_saved_ssid(
      ap.data(), ap.size(), "HomeNet-2G");
  assert(visible);

  // An unclocked reboot with a weak ambient fingerprint must recover when
  // the saved home AP is seen; the old fingerprint-only path refuses.
  assert(!clock_sync::due(true, false, false, false, 0, -1, -1));
  assert(clock_sync::due(true, false, visible, false, 0, -1, -1));
  assert(!clock_sync::due(false, true, visible, false, 0, -1, -1));

  // A failed connection must not spend radio energy on every tap/scan.
  assert(!clock_sync::due(true, false, visible, false, MINUTE, -1, 0));
  assert(!clock_sync::due(true, true, visible, false, 30 * MINUTE - 1, -1, 0));
  assert(clock_sync::due(true, false, visible, false, 30 * MINUTE, -1, 0));
  assert(!clock_sync::due(true, false, false, false, 30 * MINUTE, -1, 0));

  // Successful sync keeps the six-hour cadence. Confirm classic HOME still
  // works, while no recognised or saved HOME means no outgoing connection.
  const int64_t synced = 30 * MINUTE;
  assert(!clock_sync::due(true, false, visible, true,
                          synced + 6 * HOUR - 1, synced, synced));
  assert(clock_sync::due(true, false, visible, true,
                         synced + 6 * HOUR, synced, synced));
  assert(!clock_sync::due(true, false, false, true,
                          synced + 6 * HOUR, synced, synced));
  assert(clock_sync::due(true, true, false, true,
                         synced + 6 * HOUR, synced, synced));

  // Exact matching rejects similar names, case changes and hidden APs.
  assert(!clock_sync::matches_saved_ssid(ap.data(), ap.size(), "HomeNet"));
  assert(!clock_sync::matches_saved_ssid(ap.data(), ap.size(), "HomeNet-2G-Guest"));
  assert(!clock_sync::matches_saved_ssid(ap.data(), ap.size(), "homenet-2g"));
  assert(!clock_sync::matches_saved_ssid(ap.data(), ap.size(), ""));
  assert(!clock_sync::matches_saved_ssid(ap.data(), ap.size(), nullptr));
  assert(!clock_sync::matches_saved_ssid(nullptr, 0, "HomeNet-2G"));
  ap.fill(0);
  assert(!clock_sync::matches_saved_ssid(ap.data(), ap.size(), "HomeNet-2G"));
  ap.fill('X');
  assert(clock_sync::matches_saved_ssid(ap.data(), ap.size(),
                                        "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"));
  assert(!clock_sync::matches_saved_ssid(ap.data(), ap.size(),
                                         "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"));

  std::puts("Clock recovery, retry budget and SSID regression checks passed");
}
