#pragma once

#include <cstdint>

#include "esp_err.h"
#include "encounter_memory.h"
#include "memory.h"

enum class AchievementType : uint8_t {
  None = 0,
  FirstSteps = 1,
  TenKClub = 2,
  WifiWanderer = 3,
  SocialButterfly = 4,
  BigDayOut = 5,
  HomeSweetHome = 6,
  AnotherOne = 7,
  LevelFive = 8,

  // RC5 LONG GAME / prestige tier. Keep values append-only because the
  // NVS badge book stores one persistent bit per numeric achievement ID.
  CenturyClub = 9,
  Seriously = 10,
  TinyLegsBigMiles = 11,
  SeasonedExplorer = 12,
  WellTravelled = 13,
  OldHand = 14,
  SignalHunter = 15,
  RadioMagpie = 16,
  TheCollector = 17,
  ThePack = 18,
  SocialCircle = 19,
  PopularLittleThing = 20,
  Veteran = 21,
  Legend = 22,
  CreatureOfHabit = 23,
  Unstoppable = 24,
};

struct AchievementUpdate {
  uint8_t new_count = 0;
  AchievementType latest = AchievementType::None;
  uint32_t xp_awarded = 0;
};

class AchievementEngine {
 public:
  // If this is the first firmware with badges, achievements already earned by
  // an existing pet are seeded silently rather than dumping retroactive XP.
  esp_err_t begin(const LifeState &life,
                  const TrimPeerBook &peers,
                  const DailyStats &day,
                  uint16_t best_walk_streak);

  AchievementUpdate evaluate(const LifeState &life,
                             const TrimPeerBook &peers,
                             const DailyStats &day,
                             uint16_t best_walk_streak);

  uint8_t unlocked_count() const;

  static const char *name(AchievementType type);

 private:
  esp_err_t load_();
  esp_err_t save_();
  uint32_t qualifying_bits_(const LifeState &life,
                            const TrimPeerBook &peers,
                            const DailyStats &day,
                            uint16_t best_walk_streak) const;
  uint32_t bit_(AchievementType type) const;

  uint32_t unlocked_bits_ = 0;
  bool initialised_ = false;
  uint8_t schema_version_ = 0;
};
