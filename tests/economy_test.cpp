#include <cassert>
#include <cstdint>

#include "economy.h"
#include "economy_rules.h"
#include "nvs.h"

int main() {
  using namespace es_pet::economy;

  static_assert(kDailyRadioXpCap == 100);
  static_assert(kPerfectMissionDayBonusXp == 200);
  static_assert(kNewPeerXp == 100);
  static_assert(capped_radio_award(0, 40) == 40);
  static_assert(capped_radio_award(90, 20) == 10);
  static_assert(capped_radio_award(100, 1) == 0);
  static_assert(perfect_day_bonus(1, 3) == 200);
  static_assert(perfect_day_bonus(3, 3) == 200);
  static_assert(perfect_day_bonus(0, 3) == 0);
  static_assert(perfect_day_bonus(1, 2) == 0);

  fake_nvs_reset();

  EconomyTracker first;
  assert(first.begin() == ESP_OK);
  assert(first.ensure_day(20260925));
  assert(first.award_radio(60) == 60);
  assert(first.award_radio(60) == 40);
  assert(first.award_radio(1) == 0);
  assert(first.radio_xp_today() == 100);
  assert(first.radio_xp_remaining() == 0);
  assert(first.save() == ESP_OK);

  EconomyTracker rebooted;
  assert(rebooted.begin() == ESP_OK);
  assert(!rebooted.ensure_day(20260925));
  assert(rebooted.award_radio(20) == 0);
  assert(rebooted.ensure_day(20260926));
  assert(rebooted.radio_xp_remaining() == 100);
  assert(rebooted.award_radio(25) == 25);

  // XP earned before the first trustworthy date carries into that date; a
  // HOME time sync cannot accidentally provide a second allowance.
  fake_nvs_reset();
  EconomyTracker undated;
  assert(undated.begin() == ESP_OK);
  assert(undated.award_radio(80) == 80);
  assert(undated.save() == ESP_OK);

  EconomyTracker synced_later;
  assert(synced_later.begin() == ESP_OK);
  assert(synced_later.ensure_day(20260925));
  assert(synced_later.award_radio(30) == 20);
  assert(synced_later.radio_xp_today() == 100);

  return 0;
}
