#pragma once

#include <cstdint>

#include "economy_rules.h"
#include "esp_err.h"

// Persists only the minimum state required to enforce RC7's daily radio-XP
// ceiling across reboots. It is not a diagnostic history or telemetry log.
class EconomyTracker {
 public:
  esp_err_t begin();

  // Reset the allowance when a trustworthy local date changes. Radio XP
  // collected before the first time sync is conservatively carried into the
  // first dated bucket of that boot.
  bool ensure_day(uint32_t date_key);

  // Returns the portion of requested XP still available within today's cap.
  uint32_t award_radio(uint32_t requested);

  uint32_t date_key() const { return date_key_; }
  uint32_t radio_xp_today() const { return radio_xp_today_; }
  uint32_t radio_xp_remaining() const {
    return radio_xp_today_ >= es_pet::economy::kDailyRadioXpCap
               ? 0
               : es_pet::economy::kDailyRadioXpCap - radio_xp_today_;
  }

  bool dirty() const { return dirty_; }
  esp_err_t save();

 private:
  void initialise_undated_();

  uint32_t date_key_ = 0;
  uint32_t radio_xp_today_ = 0;
  bool initialised_ = false;
  bool dirty_ = false;
};
