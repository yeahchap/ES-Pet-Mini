#pragma once

#include <cstdint>

namespace es_pet::economy {

// RC7 progression balance. Radio discovery remains part of the pet's
// personality, but deliberate missions are now the main source of XP.
constexpr uint32_t kDailyRadioXpCap = 100;
constexpr uint32_t kPerfectMissionDayBonusXp = 200;
constexpr uint32_t kNewPeerXp = 100;

constexpr uint32_t capped_radio_award(uint32_t awarded_today,
                                      uint32_t requested) {
  return awarded_today >= kDailyRadioXpCap
             ? 0
             : (requested > (kDailyRadioXpCap - awarded_today)
                    ? (kDailyRadioXpCap - awarded_today)
                    : requested);
}

constexpr uint32_t perfect_day_bonus(uint8_t completed_now,
                                     uint8_t completed_after) {
  return completed_now > 0 && completed_after == 3
             ? kPerfectMissionDayBonusXp
             : 0;
}

}  // namespace es_pet::economy
