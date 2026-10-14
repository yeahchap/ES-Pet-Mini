#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

enum class MissionType : uint8_t {
  None = 0,
  StretchLegs = 1,
  SignalSafari = 2,
  SocialSpark = 3,
  EarlyBird = 4,
  LunchWalk = 5,
  EveningMixer = 6,
  BigDayOut = 7,
  GoExploring = 8,

  // Expanded daily pool retained by RC7.
  NewHorizons = 9,
  BluetoothTrail = 10,
  DoubleAdventure = 11,
  HomeAgain = 12,
  WeekendWander = 13,
  MondayMomentum = 14,
  FridayRoam = 15,
};

enum class ChallengeType : uint8_t {
  None = 0,

  // Consecutive qualifying walking days (>= 1500 steps/day).
  Walk3 = 1,
  Walk5 = 2,
  Walk7 = 3,
  Walk14 = 4,
  Walk30 = 5,

  // Consecutive dates with at least one ADVENTURE started.
  Adventure3 = 6,
  Adventure5 = 7,
  Adventure7 = 8,

  // Calendar-driven, non-consecutive challenges.
  FiveInSeven = 9,
  PerfectWeek = 10,
  WeekendPair = 11,
  Month15 = 12,
  Month25 = 13,
};

struct MissionSlot {
  uint8_t type = 0;
  uint8_t flags = 0;     // bit0 = completed / reward already paid
  uint16_t reserved = 0;
  uint32_t progress = 0;
  uint32_t target = 0;
  uint32_t reward_xp = 0;
};

// Deliberately unchanged from V0.7/V0.8 so an in-progress current-day set is
// preserved through an RC7 app-only update.
struct MissionState {
  uint32_t magic = 0x544D4D53;  // TMMS
  uint16_t version = 1;
  uint16_t count = 3;
  uint32_t date_key = 0;
  MissionSlot slots[3] = {};
};

// Long-term calendar challenge state lives separately from the daily mission
// blob. No fake retroactive streak is reconstructed from older firmware.
struct ChallengeState {
  uint32_t magic = 0x544D4348;  // TMCH
  uint16_t version = 1;
  uint16_t reserved0 = 0;

  uint32_t current_date = 0;

  uint32_t last_walk_date = 0;
  uint16_t walk_streak = 0;
  uint16_t best_walk_streak = 0;

  uint32_t last_adventure_date = 0;
  uint16_t adventure_streak = 0;
  uint16_t best_adventure_streak = 0;

  // Monday-Sunday calendar week.
  uint32_t week_key = 0;
  uint8_t week_walk_mask = 0;       // weekday bits: SUN=0 ... SAT=6
  uint8_t week_reward_flags = 0;    // bit0=5/7, bit1=weekend, bit2=7/7
  uint16_t reserved1 = 0;

  // Calendar month, bit 0 == day 1 ... bit 30 == day 31.
  uint32_t month_key = 0;           // YYYYMM
  uint32_t month_walk_mask = 0;
  uint8_t month_reward_flags = 0;   // bit0=15 days, bit1=25 days
  uint8_t reserved2[3] = {};
};

struct MissionInput {
  uint32_t date_key = 0;
  uint8_t local_hour = 0;

  uint32_t steps_delta = 0;
  uint32_t steps_today = 0;

  // First-seen-today counts (used by the original daily missions).
  uint32_t wifi_new_delta = 0;
  uint32_t bt_new_delta = 0;

  // Truly new-to-this-pet discoveries (V0.8 novelty memory).
  uint32_t wifi_lifetime_new_delta = 0;
  uint32_t bt_lifetime_new_delta = 0;

  uint32_t peer_new_delta = 0;
  bool adventure_started = false;
  bool adventure_ended = false;
};

struct MissionUpdate {
  bool changed = false;
  bool new_day = false;
  uint8_t completed_count = 0;
  MissionType latest_completed = MissionType::None;
  uint32_t xp_awarded = 0;

  bool challenge_changed = false;
  uint8_t challenge_completed_count = 0;
  ChallengeType latest_challenge = ChallengeType::None;
  uint32_t challenge_xp_awarded = 0;
};

class MissionEngine {
 public:
  esp_err_t begin(uint64_t pet_id);

  // Generates a fresh three-mission set when date_key changes and advances
  // the calendar windows used by long-term challenges.
  esp_err_t ensure_day(uint32_t date_key, uint8_t local_hour, bool &new_day);

  MissionUpdate update(const MissionInput &input);

  const MissionState &state() const { return state_; }
  const MissionSlot &slot(size_t index) const { return state_.slots[index]; }
  const ChallengeState &challenges() const { return challenge_; }

  uint8_t completed_count() const;
  int featured_slot() const;

  uint16_t walk_streak() const { return challenge_.walk_streak; }
  uint16_t best_walk_streak() const { return challenge_.best_walk_streak; }
  uint16_t adventure_streak() const { return challenge_.adventure_streak; }
  uint8_t week_walk_days() const;
  uint8_t month_walk_days() const;

  static const char *name(MissionType type);
  static const char *short_name(MissionType type);
  static uint32_t reward_xp(MissionType type);
  static const char *challenge_name(ChallengeType type);
  static const char *challenge_detail(ChallengeType type);

 private:
  esp_err_t load_();
  esp_err_t save_missions_();
  esp_err_t save_challenges_();
  void generate_(uint32_t date_key, uint8_t local_hour);
  void configure_slot_(MissionSlot &slot, MissionType type);
  bool refresh_incomplete_rewards_();

  bool prepare_challenge_date_(uint32_t date_key);
  void record_challenge_(MissionUpdate &out,
                         ChallengeType type,
                         uint32_t reward_xp);
  void qualify_walk_day_(uint32_t date_key, MissionUpdate &out);
  void qualify_adventure_day_(uint32_t date_key, MissionUpdate &out);

  uint64_t pet_id_ = 0;
  MissionState state_{};
  ChallengeState challenge_{};
};
