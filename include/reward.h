#pragma once
#include <cstdint>

struct EpochReward {
  uint32_t xp = 0;

  // Wi-Fi novelty classes for this SCAN.
  uint32_t new_friends = 0;          // never seen before
  uint32_t known_today = 0;          // known lifetime, first sighting today
  uint32_t repeat_today = 0;         // already seen today
  uint32_t familiar_friends = 0;     // known_today + repeat_today (compat/UI)
  uint32_t new_channels = 0;
};

// V0.8 novelty economy:
//   Wi-Fi new-ever          = +4 XP
//   Wi-Fi known/first-today = +2 XP
//   Wi-Fi repeat-today      = +0 XP
//   New lifetime channel    = +2 XP
EpochReward calculate_epoch_reward(uint32_t new_friends,
                                   uint32_t known_today,
                                   uint32_t repeat_today,
                                   uint32_t new_channels);

// BLE mirrors the same 2:1 novelty ratio:
//   BLE new-ever            = +2 XP
//   BLE known/first-today   = +1 XP
//   BLE repeat-today        = +0 XP
uint32_t calculate_ble_reward(uint32_t new_ever,
                              uint32_t known_today);
uint32_t level_start_xp(uint32_t level);
uint32_t level_from_xp(uint32_t xp);
uint32_t next_level_xp(uint32_t level);
