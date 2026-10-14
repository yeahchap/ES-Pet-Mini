#include "mission.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "trim_mission";

namespace {

constexpr const char *NAMESPACE_NAME = "trimgame";
constexpr const char *MISSION_KEY = "missions";
constexpr const char *CHALLENGE_KEY = "challenges";
constexpr uint32_t MISSION_MAGIC = 0x544D4D53;
constexpr uint16_t MISSION_VERSION = 1;
constexpr uint32_t CHALLENGE_MAGIC = 0x544D4348;
constexpr uint16_t CHALLENGE_VERSION = 1;
constexpr uint8_t FLAG_COMPLETE = 0x01;

constexpr uint32_t WALK_DAY_TARGET = 1500;

uint64_t next_rng(uint64_t &s) {
  // Small deterministic xorshift generator. Missions vary by date and Pet ID,
  // but remain stable across reboot for that same day.
  s ^= s << 13;
  s ^= s >> 7;
  s ^= s << 17;
  return s;
}

bool split_date_key(uint32_t key, int &year, unsigned &month, unsigned &day) {
  if (key < 19000101u) return false;
  year = static_cast<int>(key / 10000u);
  month = static_cast<unsigned>((key / 100u) % 100u);
  day = static_cast<unsigned>(key % 100u);
  return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

// Howard Hinnant's civil-date mapping, adapted to produce a simple day serial.
// It needs no RTC/timezone calls and therefore stays deterministic for a
// YYYYMMDD local-date key.
int64_t days_from_civil(int y, unsigned m, unsigned d) {
  y -= (m <= 2);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned mp = (m > 2) ? (m - 3u) : (m + 9u);
  const unsigned doy = (153u * mp + 2u) / 5u + d - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return static_cast<int64_t>(era) * 146097LL +
         static_cast<int64_t>(doe) - 719468LL;
}

int64_t date_serial(uint32_t key) {
  int y = 0;
  unsigned m = 0, d = 0;
  if (!split_date_key(key, y, m, d)) return -1;
  return days_from_civil(y, m, d);
}

uint8_t weekday_from_key(uint32_t key) {
  // 1970-01-01 was Thursday. Return SUN=0 ... SAT=6.
  const int64_t serial = date_serial(key);
  if (serial < -700000) return 0;
  int w = static_cast<int>((serial + 4LL) % 7LL);
  if (w < 0) w += 7;
  return static_cast<uint8_t>(w);
}

uint32_t monday_week_key(uint32_t key) {
  const int64_t serial = date_serial(key);
  if (serial < 0) return 0;
  const uint8_t weekday = weekday_from_key(key);
  const uint8_t since_monday = static_cast<uint8_t>((weekday + 6u) % 7u);
  return static_cast<uint32_t>(serial - since_monday);
}

uint8_t popcount8(uint8_t v) {
  uint8_t n = 0;
  while (v) {
    n = static_cast<uint8_t>(n + (v & 1u));
    v = static_cast<uint8_t>(v >> 1u);
  }
  return n;
}

uint8_t popcount32(uint32_t v) {
  uint8_t n = 0;
  while (v) {
    n = static_cast<uint8_t>(n + (v & 1u));
    v >>= 1u;
  }
  return n;
}

}  // namespace

esp_err_t MissionEngine::begin(uint64_t pet_id) {
  pet_id_ = pet_id;
  return load_();
}

esp_err_t MissionEngine::load_() {
  state_ = MissionState{};
  challenge_ = ChallengeState{};

  nvs_handle_t h;
  esp_err_t err = nvs_open(NAMESPACE_NAME, NVS_READONLY, &h);
  if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
  if (err != ESP_OK) return err;

  size_t mission_size = sizeof(state_);
  esp_err_t mission_err = nvs_get_blob(h, MISSION_KEY, &state_, &mission_size);
  if (mission_err == ESP_ERR_NVS_NOT_FOUND) {
    state_ = MissionState{};
  } else if (mission_err != ESP_OK ||
             mission_size != sizeof(state_) ||
             state_.magic != MISSION_MAGIC ||
             state_.version != MISSION_VERSION ||
             state_.count != 3) {
    ESP_LOGW(TAG, "Mission state invalid/mismatched; starting fresh");
    state_ = MissionState{};
  }

  size_t challenge_size = sizeof(challenge_);
  esp_err_t challenge_err =
      nvs_get_blob(h, CHALLENGE_KEY, &challenge_, &challenge_size);
  if (challenge_err == ESP_ERR_NVS_NOT_FOUND) {
    challenge_ = ChallengeState{};
  } else if (challenge_err != ESP_OK ||
             challenge_size != sizeof(challenge_) ||
             challenge_.magic != CHALLENGE_MAGIC ||
             challenge_.version != CHALLENGE_VERSION) {
    ESP_LOGW(TAG, "Challenge state invalid/mismatched; starting fresh");
    challenge_ = ChallengeState{};
  }

  nvs_close(h);

  if (state_.date_key != 0) {
    ESP_LOGI(TAG,
             "Loaded missions for %lu: %u/3 complete",
             static_cast<unsigned long>(state_.date_key),
             static_cast<unsigned>(completed_count()));
  }

  if (challenge_.current_date != 0) {
    ESP_LOGI(TAG,
             "Loaded challenges: walk=%u (best=%u) adventure=%u week=%u/7 month=%u",
             static_cast<unsigned>(challenge_.walk_streak),
             static_cast<unsigned>(challenge_.best_walk_streak),
             static_cast<unsigned>(challenge_.adventure_streak),
             static_cast<unsigned>(week_walk_days()),
             static_cast<unsigned>(month_walk_days()));
  }

  return ESP_OK;
}

esp_err_t MissionEngine::save_missions_() {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NAMESPACE_NAME, NVS_READWRITE, &h);
  if (err != ESP_OK) return err;

  err = nvs_set_blob(h, MISSION_KEY, &state_, sizeof(state_));
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  return err;
}

esp_err_t MissionEngine::save_challenges_() {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NAMESPACE_NAME, NVS_READWRITE, &h);
  if (err != ESP_OK) return err;

  err = nvs_set_blob(h, CHALLENGE_KEY, &challenge_, sizeof(challenge_));
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  return err;
}

uint32_t MissionEngine::reward_xp(MissionType type) {
  switch (type) {
    // Easy daily missions.
    case MissionType::StretchLegs:
    case MissionType::SignalSafari:
    case MissionType::SocialSpark:
      return 100;

    // Standard daily missions.
    case MissionType::EarlyBird:
    case MissionType::LunchWalk:
    case MissionType::EveningMixer:
    case MissionType::GoExploring:
    case MissionType::NewHorizons:
    case MissionType::BluetoothTrail:
    case MissionType::HomeAgain:
      return 150;

    // Longer or calendar-limited missions.
    case MissionType::WeekendWander:
    case MissionType::MondayMomentum:
    case MissionType::FridayRoam:
      return 200;

    // The two hardest single-day missions.
    case MissionType::BigDayOut:
    case MissionType::DoubleAdventure:
      return 250;

    default:
      return 0;
  }
}

void MissionEngine::configure_slot_(MissionSlot &slot, MissionType type) {
  slot = MissionSlot{};
  slot.type = static_cast<uint8_t>(type);
  slot.reward_xp = reward_xp(type);

  switch (type) {
    case MissionType::StretchLegs:
      slot.target = 1500;
      break;
    case MissionType::SignalSafari:
      slot.target = 20;
      break;
    case MissionType::SocialSpark:
      slot.target = 25;
      break;
    case MissionType::EarlyBird:
      slot.target = 500;
      break;
    case MissionType::LunchWalk:
      slot.target = 400;
      break;
    case MissionType::EveningMixer:
      slot.target = 15;
      break;
    case MissionType::BigDayOut:
      slot.target = 5000;
      break;
    case MissionType::GoExploring:
      slot.target = 1;
      break;

    case MissionType::NewHorizons:
      slot.target = 8;
      break;
    case MissionType::BluetoothTrail:
      slot.target = 12;
      break;
    case MissionType::DoubleAdventure:
      slot.target = 2;
      break;
    case MissionType::HomeAgain:
      slot.target = 1;
      break;
    case MissionType::WeekendWander:
      slot.target = 2500;
      break;
    case MissionType::MondayMomentum:
      slot.target = 1800;
      break;
    case MissionType::FridayRoam:
      slot.target = 1;
      break;

    default:
      break;
  }
}

bool MissionEngine::refresh_incomplete_rewards_() {
  bool changed = false;

  for (MissionSlot &slot : state_.slots) {
    if (slot.type == static_cast<uint8_t>(MissionType::None) ||
        (slot.flags & FLAG_COMPLETE) != 0) {
      continue;
    }

    const uint32_t expected =
        reward_xp(static_cast<MissionType>(slot.type));
    if (slot.reward_xp != expected) {
      slot.reward_xp = expected;
      changed = true;
    }
  }

  return changed;
}

void MissionEngine::generate_(uint32_t date_key, uint8_t local_hour) {
  state_ = MissionState{};
  state_.date_key = date_key;

  std::array<MissionType, 16> candidates{};
  size_t count = 0;

  // Always-achievable core pool.
  candidates[count++] = MissionType::StretchLegs;
  candidates[count++] = MissionType::SignalSafari;
  candidates[count++] = MissionType::SocialSpark;
  candidates[count++] = MissionType::BigDayOut;
  candidates[count++] = MissionType::GoExploring;
  candidates[count++] = MissionType::NewHorizons;
  candidates[count++] = MissionType::BluetoothTrail;
  candidates[count++] = MissionType::DoubleAdventure;
  candidates[count++] = MissionType::HomeAgain;

  // Time-window missions are only offered if their useful window has not
  // already finished when the daily set is first generated.
  if (local_hour < 10) candidates[count++] = MissionType::EarlyBird;
  if (local_hour < 14) candidates[count++] = MissionType::LunchWalk;
  if (local_hour < 23) candidates[count++] = MissionType::EveningMixer;

  // Calendar-specific missions make different days feel different without
  // needing any cloud service or online calendar.
  const uint8_t weekday = weekday_from_key(date_key);
  if (weekday == 0 || weekday == 6) {
    candidates[count++] = MissionType::WeekendWander;
  }
  if (weekday == 1) candidates[count++] = MissionType::MondayMomentum;
  if (weekday == 5) candidates[count++] = MissionType::FridayRoam;

  uint64_t rng =
      pet_id_ ^
      (static_cast<uint64_t>(date_key) << 17) ^
      0x4D495353494F4E53ULL;

  for (size_t i = count; i > 1; --i) {
    const size_t j = static_cast<size_t>(next_rng(rng) % i);
    std::swap(candidates[i - 1], candidates[j]);
  }

  for (size_t i = 0; i < 3; ++i) {
    configure_slot_(state_.slots[i], candidates[i]);
  }

  ESP_LOGI(TAG,
           "NEW MISSIONS %lu: [%s] [%s] [%s]",
           static_cast<unsigned long>(date_key),
           short_name(static_cast<MissionType>(state_.slots[0].type)),
           short_name(static_cast<MissionType>(state_.slots[1].type)),
           short_name(static_cast<MissionType>(state_.slots[2].type)));
}

bool MissionEngine::prepare_challenge_date_(uint32_t date_key) {
  if (date_key == 0 || challenge_.current_date == date_key) return false;

  bool changed = true;
  const int64_t today = date_serial(date_key);

  if (challenge_.last_walk_date != 0) {
    const int64_t last = date_serial(challenge_.last_walk_date);
    if (last < 0 || today - last > 1) challenge_.walk_streak = 0;
  }

  if (challenge_.last_adventure_date != 0) {
    const int64_t last = date_serial(challenge_.last_adventure_date);
    if (last < 0 || today - last > 1) challenge_.adventure_streak = 0;
  }

  const uint32_t week_key = monday_week_key(date_key);
  if (challenge_.week_key != week_key) {
    challenge_.week_key = week_key;
    challenge_.week_walk_mask = 0;
    challenge_.week_reward_flags = 0;
  }

  const uint32_t month_key = date_key / 100u;
  if (challenge_.month_key != month_key) {
    challenge_.month_key = month_key;
    challenge_.month_walk_mask = 0;
    challenge_.month_reward_flags = 0;
  }

  challenge_.current_date = date_key;
  return changed;
}

esp_err_t MissionEngine::ensure_day(uint32_t date_key,
                                    uint8_t local_hour,
                                    bool &new_day) {
  new_day = false;
  if (date_key == 0) return ESP_ERR_INVALID_ARG;

  const bool mission_new_day = state_.date_key != date_key;
  const bool challenge_changed = prepare_challenge_date_(date_key);

  if (mission_new_day) {
    generate_(date_key, local_hour);
    new_day = true;
    esp_err_t err = save_missions_();
    if (err != ESP_OK) return err;
  } else if (refresh_incomplete_rewards_()) {
    // Preserve the current RC6 mission choices and progress while applying
    // RC7 rewards prospectively to anything not already completed/paid.
    esp_err_t err = save_missions_();
    if (err != ESP_OK) return err;
  }

  if (challenge_changed) {
    esp_err_t err = save_challenges_();
    if (err != ESP_OK) return err;
  }

  return ESP_OK;
}

void MissionEngine::record_challenge_(MissionUpdate &out,
                                      ChallengeType type,
                                      uint32_t reward_xp) {
  ++out.challenge_completed_count;
  out.latest_challenge = type;
  out.challenge_xp_awarded += reward_xp;
  out.challenge_changed = true;

  ESP_LOGI(TAG,
           "CHALLENGE COMPLETE: %s +%lu XP",
           challenge_name(type),
           static_cast<unsigned long>(reward_xp));
}

void MissionEngine::qualify_walk_day_(uint32_t date_key,
                                      MissionUpdate &out) {
  if (challenge_.last_walk_date == date_key) return;

  const int64_t today = date_serial(date_key);
  const int64_t last = date_serial(challenge_.last_walk_date);

  if (challenge_.last_walk_date != 0 && last >= 0 && today - last == 1) {
    if (challenge_.walk_streak < 0xFFFFu) ++challenge_.walk_streak;
  } else {
    challenge_.walk_streak = 1;
  }

  challenge_.last_walk_date = date_key;
  challenge_.best_walk_streak =
      std::max<uint16_t>(challenge_.best_walk_streak, challenge_.walk_streak);
  out.challenge_changed = true;

  switch (challenge_.walk_streak) {
    case 3:  record_challenge_(out, ChallengeType::Walk3, 100); break;
    case 5:  record_challenge_(out, ChallengeType::Walk5, 175); break;
    case 7:  record_challenge_(out, ChallengeType::Walk7, 300); break;
    case 14: record_challenge_(out, ChallengeType::Walk14, 650); break;
    case 30: record_challenge_(out, ChallengeType::Walk30, 1500); break;
    default: break;
  }

  const uint8_t weekday = weekday_from_key(date_key);
  challenge_.week_walk_mask = static_cast<uint8_t>(
      challenge_.week_walk_mask | static_cast<uint8_t>(1u << weekday));

  const uint8_t week_days = popcount8(challenge_.week_walk_mask);
  if (week_days >= 5 && (challenge_.week_reward_flags & 0x01u) == 0) {
    challenge_.week_reward_flags |= 0x01u;
    record_challenge_(out, ChallengeType::FiveInSeven, 250);
  }

  // Saturday + Sunday in the same Monday-Sunday week.
  const uint8_t weekend_bits = static_cast<uint8_t>((1u << 6u) | (1u << 0u));
  if (weekday == 0 &&
      (challenge_.week_walk_mask & weekend_bits) == weekend_bits &&
      (challenge_.week_reward_flags & 0x02u) == 0) {
    challenge_.week_reward_flags |= 0x02u;
    record_challenge_(out, ChallengeType::WeekendPair, 125);
  }

  if (week_days >= 7 && (challenge_.week_reward_flags & 0x04u) == 0) {
    challenge_.week_reward_flags |= 0x04u;
    record_challenge_(out, ChallengeType::PerfectWeek, 400);
  }

  const unsigned dom = static_cast<unsigned>(date_key % 100u);
  if (dom >= 1 && dom <= 31) {
    challenge_.month_walk_mask |= (1u << (dom - 1u));
  }

  const uint8_t month_days = popcount32(challenge_.month_walk_mask);
  if (month_days >= 15 && (challenge_.month_reward_flags & 0x01u) == 0) {
    challenge_.month_reward_flags |= 0x01u;
    record_challenge_(out, ChallengeType::Month15, 600);
  }
  if (month_days >= 25 && (challenge_.month_reward_flags & 0x02u) == 0) {
    challenge_.month_reward_flags |= 0x02u;
    record_challenge_(out, ChallengeType::Month25, 1200);
  }
}

void MissionEngine::qualify_adventure_day_(uint32_t date_key,
                                           MissionUpdate &out) {
  if (challenge_.last_adventure_date == date_key) return;

  const int64_t today = date_serial(date_key);
  const int64_t last = date_serial(challenge_.last_adventure_date);

  if (challenge_.last_adventure_date != 0 &&
      last >= 0 && today - last == 1) {
    if (challenge_.adventure_streak < 0xFFFFu) {
      ++challenge_.adventure_streak;
    }
  } else {
    challenge_.adventure_streak = 1;
  }

  challenge_.last_adventure_date = date_key;
  challenge_.best_adventure_streak =
      std::max<uint16_t>(challenge_.best_adventure_streak,
                         challenge_.adventure_streak);
  out.challenge_changed = true;

  switch (challenge_.adventure_streak) {
    case 3: record_challenge_(out, ChallengeType::Adventure3, 120); break;
    case 5: record_challenge_(out, ChallengeType::Adventure5, 200); break;
    case 7: record_challenge_(out, ChallengeType::Adventure7, 325); break;
    default: break;
  }
}

MissionUpdate MissionEngine::update(const MissionInput &input) {
  MissionUpdate out;

  if (input.date_key == 0) return out;

  if (prepare_challenge_date_(input.date_key)) {
    out.challenge_changed = true;
  }

  // Daily mission progress only applies to the set generated for this date.
  if (state_.date_key == input.date_key) {
    const uint8_t weekday = weekday_from_key(input.date_key);

    for (size_t i = 0; i < 3; ++i) {
      MissionSlot &slot = state_.slots[i];

      if (slot.type == static_cast<uint8_t>(MissionType::None) ||
          (slot.flags & FLAG_COMPLETE) != 0) {
        continue;
      }

      uint32_t delta = 0;
      const MissionType type = static_cast<MissionType>(slot.type);

      switch (type) {
        case MissionType::StretchLegs:
        case MissionType::BigDayOut:
          delta = input.steps_delta;
          break;

        case MissionType::SignalSafari:
          delta = input.wifi_new_delta;
          break;

        case MissionType::SocialSpark:
          delta = input.bt_new_delta;
          break;

        case MissionType::EarlyBird:
          if (input.local_hour < 10) delta = input.steps_delta;
          break;

        case MissionType::LunchWalk:
          if (input.local_hour >= 12 && input.local_hour < 14) {
            delta = input.steps_delta;
          }
          break;

        case MissionType::EveningMixer:
          if (input.local_hour >= 18 && input.local_hour < 23) {
            delta = input.bt_new_delta;
          }
          break;

        case MissionType::GoExploring:
          delta = input.adventure_started ? 1u : 0u;
          break;

        case MissionType::NewHorizons:
          delta = input.wifi_lifetime_new_delta;
          break;

        case MissionType::BluetoothTrail:
          delta = input.bt_lifetime_new_delta;
          break;

        case MissionType::DoubleAdventure:
          delta = input.adventure_started ? 1u : 0u;
          break;

        case MissionType::HomeAgain:
          delta = input.adventure_ended ? 1u : 0u;
          break;

        case MissionType::WeekendWander:
          if (weekday == 0 || weekday == 6) delta = input.steps_delta;
          break;

        case MissionType::MondayMomentum:
          if (weekday == 1) delta = input.steps_delta;
          break;

        case MissionType::FridayRoam:
          if (weekday == 5) delta = input.adventure_started ? 1u : 0u;
          break;

        default:
          break;
      }

      if (delta == 0) continue;

      const uint64_t updated =
          static_cast<uint64_t>(slot.progress) + delta;

      slot.progress = static_cast<uint32_t>(
          std::min<uint64_t>(updated, slot.target));

      out.changed = true;

      if (slot.progress >= slot.target) {
        slot.flags |= FLAG_COMPLETE;
        ++out.completed_count;
        out.latest_completed = type;
        out.xp_awarded += slot.reward_xp;

        ESP_LOGI(TAG,
                 "MISSION COMPLETE: %s +%lu XP",
                 name(type),
                 static_cast<unsigned long>(slot.reward_xp));
      }
    }
  }

  // Long-term challenges are date-driven and deliberately separate from the
  // three random daily missions. A walking day qualifies once TODAY reaches
  // 1500 steps; an adventure day qualifies on the first journey start.
  if (input.steps_today >= WALK_DAY_TARGET &&
      challenge_.last_walk_date != input.date_key) {
    qualify_walk_day_(input.date_key, out);
  }

  if (input.adventure_started &&
      challenge_.last_adventure_date != input.date_key) {
    qualify_adventure_day_(input.date_key, out);
  }

  if (out.changed) {
    esp_err_t err = save_missions_();
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Could not persist mission progress: %s",
               esp_err_to_name(err));
    }
  }

  if (out.challenge_changed) {
    esp_err_t err = save_challenges_();
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Could not persist challenge progress: %s",
               esp_err_to_name(err));
    }
  }

  return out;
}

uint8_t MissionEngine::completed_count() const {
  uint8_t n = 0;
  for (const auto &slot : state_.slots) {
    if (slot.flags & FLAG_COMPLETE) ++n;
  }
  return n;
}

int MissionEngine::featured_slot() const {
  for (size_t i = 0; i < 3; ++i) {
    if ((state_.slots[i].flags & FLAG_COMPLETE) == 0) {
      return static_cast<int>(i);
    }
  }
  return 2;
}

uint8_t MissionEngine::week_walk_days() const {
  return popcount8(challenge_.week_walk_mask);
}

uint8_t MissionEngine::month_walk_days() const {
  return popcount32(challenge_.month_walk_mask);
}

const char *MissionEngine::name(MissionType type) {
  switch (type) {
    case MissionType::StretchLegs:      return "STRETCH YOUR LEGS";
    case MissionType::SignalSafari:     return "SIGNAL SAFARI";
    case MissionType::SocialSpark:      return "SOCIAL SPARK";
    case MissionType::EarlyBird:        return "EARLY BIRD";
    case MissionType::LunchWalk:        return "LUNCH WALK";
    case MissionType::EveningMixer:     return "EVENING MIXER";
    case MissionType::BigDayOut:        return "BIG DAY OUT";
    case MissionType::GoExploring:      return "GO EXPLORING";
    case MissionType::NewHorizons:      return "NEW HORIZONS";
    case MissionType::BluetoothTrail:   return "BLUETOOTH TRAIL";
    case MissionType::DoubleAdventure:  return "DOUBLE ADVENTURE";
    case MissionType::HomeAgain:        return "HOME AGAIN";
    case MissionType::WeekendWander:    return "WEEKEND WANDER";
    case MissionType::MondayMomentum:   return "MONDAY MOMENTUM";
    case MissionType::FridayRoam:       return "FRIDAY ROAM";
    default:                             return "NO MISSION";
  }
}

const char *MissionEngine::short_name(MissionType type) {
  switch (type) {
    case MissionType::StretchLegs:      return "STRETCH LEGS";
    case MissionType::SignalSafari:     return "SIGNAL SAFARI";
    case MissionType::SocialSpark:      return "SOCIAL SPARK";
    case MissionType::EarlyBird:        return "EARLY BIRD";
    case MissionType::LunchWalk:        return "LUNCH WALK";
    case MissionType::EveningMixer:     return "EVENING MIXER";
    case MissionType::BigDayOut:        return "BIG DAY OUT";
    case MissionType::GoExploring:      return "GO EXPLORING";
    case MissionType::NewHorizons:      return "NEW HORIZONS";
    case MissionType::BluetoothTrail:   return "BT TRAIL";
    case MissionType::DoubleAdventure:  return "DOUBLE ADVENTURE";
    case MissionType::HomeAgain:        return "HOME AGAIN";
    case MissionType::WeekendWander:    return "WEEKEND WANDER";
    case MissionType::MondayMomentum:   return "MONDAY MOTION";
    case MissionType::FridayRoam:       return "FRIDAY ROAM";
    default:                             return "NO MISSION";
  }
}

const char *MissionEngine::challenge_name(ChallengeType type) {
  switch (type) {
    case ChallengeType::Walk3:          return "THREE DAY TREK";
    case ChallengeType::Walk5:          return "FIVE DAY FEET";
    case ChallengeType::Walk7:          return "SEVEN DAY STRIDE";
    case ChallengeType::Walk14:         return "FORTNIGHT FEET";
    case ChallengeType::Walk30:         return "MONTH ON FOOT";
    case ChallengeType::Adventure3:     return "3 DAY EXPLORER";
    case ChallengeType::Adventure5:     return "5 DAY EXPLORER";
    case ChallengeType::Adventure7:     return "EXPLORER WEEK";
    case ChallengeType::FiveInSeven:    return "FIVE IN SEVEN";
    case ChallengeType::PerfectWeek:    return "PERFECT WEEK";
    case ChallengeType::WeekendPair:    return "WEEKEND WARRIOR";
    case ChallengeType::Month15:        return "MONTHLY MOVER";
    case ChallengeType::Month25:        return "MONTHLY LEGEND";
    default:                             return "CHALLENGE";
  }
}

const char *MissionEngine::challenge_detail(ChallengeType type) {
  switch (type) {
    case ChallengeType::Walk3:          return "1500+ STEPS x 3 DAYS";
    case ChallengeType::Walk5:          return "1500+ STEPS x 5 DAYS";
    case ChallengeType::Walk7:          return "1500+ STEPS x 7 DAYS";
    case ChallengeType::Walk14:         return "14 DAYS IN A ROW";
    case ChallengeType::Walk30:         return "30 DAYS IN A ROW";
    case ChallengeType::Adventure3:     return "ADVENTURE x 3 DAYS";
    case ChallengeType::Adventure5:     return "ADVENTURE x 5 DAYS";
    case ChallengeType::Adventure7:     return "ADVENTURE x 7 DAYS";
    case ChallengeType::FiveInSeven:    return "5 WALK DAYS THIS WEEK";
    case ChallengeType::PerfectWeek:    return "7 / 7 WALK DAYS";
    case ChallengeType::WeekendPair:    return "SATURDAY + SUNDAY";
    case ChallengeType::Month15:        return "15 WALK DAYS THIS MONTH";
    case ChallengeType::Month25:        return "25 WALK DAYS THIS MONTH";
    default:                             return "MULTI-DAY BONUS";
  }
}
