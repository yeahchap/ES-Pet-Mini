#pragma once

#include <cstdint>

#include "esp_err.h"
#include "memory.h"

struct DailyStats {
  uint32_t date_key = 0;      // YYYYMMDD
  uint32_t steps = 0;
  uint32_t wifi_unique = 0;
  uint32_t ble_unique = 0;
  uint32_t xp = 0;
  uint16_t adventures = 0;
  uint16_t peer_meetings = 0;
};

enum class DiaryEventType : uint8_t {
  AdventureStart = 1,
  AdventureEnd = 2,
  PeerFirstMeeting = 3,
  LevelUp = 4,
  MissionComplete = 5,
  AchievementUnlocked = 6,
  ChallengeComplete = 7,
};

class EncounterMemory {
 public:
  // Initialises the raw DIARY layout. On the very first V0.6.1 boot it seeds
  // the lifetime filters from the exact 256-entry legacy friend books.
  esp_err_t begin(const WifiFriendBook &wifi_book,
                  const BleFriendBook &ble_book,
                  LifeState &life);

  bool observe_lifetime_wifi(uint64_t id);
  bool observe_lifetime_ble(uint64_t id);

  // Returns true when a new local date was started.
  bool ensure_day(uint32_t date_key);
  bool observe_daily_wifi(uint64_t id);
  bool observe_daily_ble(uint64_t id);

  void add_daily_steps(uint32_t steps) { daily_.steps += steps; }
  void add_daily_wifi(uint32_t count) { daily_.wifi_unique += count; }
  void add_daily_ble(uint32_t count) { daily_.ble_unique += count; }
  void add_daily_xp(uint32_t xp) { daily_.xp += xp; }
  void add_daily_adventure() { if (daily_.adventures < 0xFFFF) ++daily_.adventures; }
  void add_daily_peer_meeting() { if (daily_.peer_meetings < 0xFFFF) ++daily_.peer_meetings; }

  const DailyStats &daily() const { return daily_; }
  esp_err_t save_daily();

  esp_err_t append_event(DiaryEventType type,
                         int64_t unix_time,
                         uint64_t subject_id = 0,
                         uint32_t value1 = 0,
                         uint32_t value2 = 0);

 private:
  bool bloom_test_and_add_(uint32_t base,
                           uint32_t size,
                           uint64_t id,
                           uint8_t hashes);
  esp_err_t reset_daily_filters_();
  esp_err_t append_daily_summary_();

  const void *partition_ = nullptr;
  DailyStats daily_{};
  uint32_t summary_index_ = 0;
  uint32_t event_index_ = 0;
};
