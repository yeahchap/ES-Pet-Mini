#include <cassert>

#include "mission.h"
#include "nvs.h"

int main() {
  assert(MissionEngine::reward_xp(MissionType::StretchLegs) == 100);
  assert(MissionEngine::reward_xp(MissionType::SignalSafari) == 100);
  assert(MissionEngine::reward_xp(MissionType::SocialSpark) == 100);

  assert(MissionEngine::reward_xp(MissionType::EarlyBird) == 150);
  assert(MissionEngine::reward_xp(MissionType::LunchWalk) == 150);
  assert(MissionEngine::reward_xp(MissionType::EveningMixer) == 150);
  assert(MissionEngine::reward_xp(MissionType::GoExploring) == 150);
  assert(MissionEngine::reward_xp(MissionType::NewHorizons) == 150);
  assert(MissionEngine::reward_xp(MissionType::BluetoothTrail) == 150);
  assert(MissionEngine::reward_xp(MissionType::HomeAgain) == 150);

  assert(MissionEngine::reward_xp(MissionType::WeekendWander) == 200);
  assert(MissionEngine::reward_xp(MissionType::MondayMomentum) == 200);
  assert(MissionEngine::reward_xp(MissionType::FridayRoam) == 200);

  assert(MissionEngine::reward_xp(MissionType::BigDayOut) == 250);
  assert(MissionEngine::reward_xp(MissionType::DoubleAdventure) == 250);
  assert(MissionEngine::reward_xp(MissionType::None) == 0);

  // Simulate an in-progress RC6 mission blob. RC7 must retain mission types,
  // progress, and completed rewards while updating only unpaid rewards.
  fake_nvs_reset();
  MissionState rc6;
  rc6.date_key = 20260925;

  rc6.slots[0].type = static_cast<uint8_t>(MissionType::StretchLegs);
  rc6.slots[0].flags = 0x01;
  rc6.slots[0].progress = 1500;
  rc6.slots[0].target = 1500;
  rc6.slots[0].reward_xp = 25;

  rc6.slots[1].type = static_cast<uint8_t>(MissionType::SignalSafari);
  rc6.slots[1].progress = 7;
  rc6.slots[1].target = 20;
  rc6.slots[1].reward_xp = 25;

  rc6.slots[2].type = static_cast<uint8_t>(MissionType::BigDayOut);
  rc6.slots[2].progress = 1000;
  rc6.slots[2].target = 5000;
  rc6.slots[2].reward_xp = 75;

  nvs_handle_t handle = 0;
  assert(nvs_open("trimgame", NVS_READWRITE, &handle) == ESP_OK);
  assert(nvs_set_blob(handle, "missions", &rc6, sizeof(rc6)) == ESP_OK);
  assert(nvs_commit(handle) == ESP_OK);
  nvs_close(handle);

  MissionEngine upgraded;
  assert(upgraded.begin(0x12345678ULL) == ESP_OK);
  bool new_day = true;
  assert(upgraded.ensure_day(20260925, 9, new_day) == ESP_OK);
  assert(!new_day);

  assert(upgraded.slot(0).flags == 0x01);
  assert(upgraded.slot(0).progress == 1500);
  assert(upgraded.slot(0).reward_xp == 25);
  assert(upgraded.slot(1).progress == 7);
  assert(upgraded.slot(1).reward_xp == 100);
  assert(upgraded.slot(2).progress == 1000);
  assert(upgraded.slot(2).reward_xp == 250);

  MissionInput input;
  input.date_key = 20260925;
  input.local_hour = 9;
  input.wifi_new_delta = 13;
  MissionUpdate update = upgraded.update(input);
  assert(update.completed_count == 1);
  assert(update.latest_completed == MissionType::SignalSafari);
  assert(update.xp_awarded == 100);

  return 0;
}
