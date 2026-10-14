#include "achievements.h"

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "trim_badges";

namespace {

constexpr const char *NAMESPACE_NAME = "trimgame";
constexpr const char *BITS_KEY = "badgebits";
constexpr const char *INIT_KEY = "badgeinit";
constexpr const char *VERSION_KEY = "badgever";
constexpr uint8_t CURRENT_BADGE_VERSION = 2;
constexpr uint8_t LEGACY_BADGE_COUNT = 8;

uint32_t reward_for(AchievementType type) {
  switch (type) {
    case AchievementType::FirstSteps:       return 25;
    case AchievementType::TenKClub:         return 75;
    case AchievementType::WifiWanderer:     return 50;
    case AchievementType::SocialButterfly:  return 50;
    case AchievementType::BigDayOut:        return 75;
    case AchievementType::HomeSweetHome:    return 50;
    case AchievementType::AnotherOne:       return 100;
    case AchievementType::LevelFive:        return 50;

    case AchievementType::CenturyClub:          return 150;
    case AchievementType::Seriously:            return 250;
    case AchievementType::TinyLegsBigMiles:     return 500;
    case AchievementType::SeasonedExplorer:     return 150;
    case AchievementType::WellTravelled:        return 250;
    case AchievementType::OldHand:              return 400;
    case AchievementType::SignalHunter:         return 200;
    case AchievementType::RadioMagpie:          return 200;
    case AchievementType::TheCollector:         return 350;
    case AchievementType::ThePack:              return 250;
    case AchievementType::SocialCircle:         return 350;
    case AchievementType::PopularLittleThing:   return 500;
    case AchievementType::Veteran:              return 300;
    case AchievementType::Legend:               return 500;
    case AchievementType::CreatureOfHabit:      return 300;
    case AchievementType::Unstoppable:          return 750;
    default:                                    return 0;
  }
}

}  // namespace

uint32_t AchievementEngine::bit_(AchievementType type) const {
  const uint8_t n = static_cast<uint8_t>(type);
  if (n == 0 || n > 31) return 0;
  return 1u << (n - 1);
}

esp_err_t AchievementEngine::load_() {
  unlocked_bits_ = 0;
  initialised_ = false;
  schema_version_ = 0;

  nvs_handle_t h;
  esp_err_t err = nvs_open(NAMESPACE_NAME, NVS_READONLY, &h);
  if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
  if (err != ESP_OK) return err;

  uint8_t init = 0;
  (void)nvs_get_u8(h, INIT_KEY, &init);
  (void)nvs_get_u32(h, BITS_KEY, &unlocked_bits_);
  (void)nvs_get_u8(h, VERSION_KEY, &schema_version_);
  nvs_close(h);

  initialised_ = init != 0;
  // RC4 and earlier had no explicit badge schema key. An initialised legacy
  // badge book is therefore version 1.
  if (initialised_ && schema_version_ == 0) schema_version_ = 1;
  return ESP_OK;
}

esp_err_t AchievementEngine::save_() {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NAMESPACE_NAME, NVS_READWRITE, &h);
  if (err != ESP_OK) return err;

  err = nvs_set_u32(h, BITS_KEY, unlocked_bits_);
  if (err == ESP_OK) err = nvs_set_u8(h, INIT_KEY, 1);
  if (err == ESP_OK) err = nvs_set_u8(h, VERSION_KEY, CURRENT_BADGE_VERSION);
  if (err == ESP_OK) err = nvs_commit(h);

  nvs_close(h);
  return err;
}

uint32_t AchievementEngine::qualifying_bits_(
    const LifeState &life,
    const TrimPeerBook &peers,
    const DailyStats &day,
    uint16_t best_walk_streak) const {
  uint32_t bits = 0;

  if (life.steps_total >= 1)
    bits |= bit_(AchievementType::FirstSteps);

  if (life.steps_total >= 10000)
    bits |= bit_(AchievementType::TenKClub);

  if (life.unique_wifi >= 100)
    bits |= bit_(AchievementType::WifiWanderer);

  if (life.unique_ble >= 100)
    bits |= bit_(AchievementType::SocialButterfly);

  if (day.steps >= 5000)
    bits |= bit_(AchievementType::BigDayOut);

  if (life.journeys >= 1)
    bits |= bit_(AchievementType::HomeSweetHome);

  if (peers.count >= 1)
    bits |= bit_(AchievementType::AnotherOne);

  if (life.level >= 5)
    bits |= bit_(AchievementType::LevelFive);

  // RC5 LONG GAME milestones. These intentionally use counters that already
  // persist in RC4, so the badge expansion does not alter LifeState, peer-book
  // or diary layouts.
  if (life.steps_total >= 100000)
    bits |= bit_(AchievementType::CenturyClub);

  if (life.steps_total >= 250000)
    bits |= bit_(AchievementType::Seriously);

  if (life.steps_total >= 500000)
    bits |= bit_(AchievementType::TinyLegsBigMiles);

  if (life.journeys >= 10)
    bits |= bit_(AchievementType::SeasonedExplorer);

  if (life.journeys >= 25)
    bits |= bit_(AchievementType::WellTravelled);

  if (life.journeys >= 50)
    bits |= bit_(AchievementType::OldHand);

  if (life.unique_wifi >= 500)
    bits |= bit_(AchievementType::SignalHunter);

  if (life.unique_ble >= 500)
    bits |= bit_(AchievementType::RadioMagpie);

  if ((static_cast<uint64_t>(life.unique_wifi) +
       static_cast<uint64_t>(life.unique_ble)) >= 1000ULL)
    bits |= bit_(AchievementType::TheCollector);

  // THE PACK means this pet plus two distinct known peers: the first social
  // prestige badge is therefore achievable with the three real ES-PETs now
  // in circulation.
  if (peers.count >= 2)
    bits |= bit_(AchievementType::ThePack);

  if (peers.count >= 5)
    bits |= bit_(AchievementType::SocialCircle);

  if (peers.count >= 10)
    bits |= bit_(AchievementType::PopularLittleThing);

  if (life.level >= 40)
    bits |= bit_(AchievementType::Veteran);

  if (life.level >= 50)
    bits |= bit_(AchievementType::Legend);

  if (best_walk_streak >= 30)
    bits |= bit_(AchievementType::CreatureOfHabit);

  if (best_walk_streak >= 100)
    bits |= bit_(AchievementType::Unstoppable);

  return bits;
}

esp_err_t AchievementEngine::begin(const LifeState &life,
                                   const TrimPeerBook &peers,
                                   const DailyStats &day,
                                   uint16_t best_walk_streak) {
  ESP_ERROR_CHECK(load_());

  const uint32_t qualified =
      qualifying_bits_(life, peers, day, best_walk_streak);

  if (!initialised_) {
    // A genuinely new badge book gets all already-satisfied badges silently.
    // This preserves the original anti-windfall behaviour.
    unlocked_bits_ = qualified;
    initialised_ = true;
    schema_version_ = CURRENT_BADGE_VERSION;
    ESP_ERROR_CHECK(save_());

    ESP_LOGI(TAG,
             "Badge book born: seeded %u existing achievements (schema v%u)",
             static_cast<unsigned>(unlocked_count()),
             static_cast<unsigned>(CURRENT_BADGE_VERSION));
  } else if (schema_version_ < CURRENT_BADGE_VERSION) {
    // RC5 adds prestige badges 9+. Existing pets may already meet some of
    // those thresholds. Seed only the newly-added generation silently so an
    // upgrade cannot dump retroactive XP or a stack of badge scenes.
    uint32_t prestige_mask = 0;
    for (uint8_t n = LEGACY_BADGE_COUNT + 1;
         n <= static_cast<uint8_t>(AchievementType::Unstoppable);
         ++n) {
      prestige_mask |= bit_(static_cast<AchievementType>(n));
    }

    const uint8_t before = unlocked_count();
    unlocked_bits_ |= (qualified & prestige_mask);
    schema_version_ = CURRENT_BADGE_VERSION;
    ESP_ERROR_CHECK(save_());
    const uint8_t seeded = static_cast<uint8_t>(unlocked_count() - before);

    ESP_LOGI(TAG,
             "Badge schema migrated v1->v2: silently seeded %u prestige badges; total=%u",
             static_cast<unsigned>(seeded),
             static_cast<unsigned>(unlocked_count()));
  } else {
    ESP_LOGI(TAG,
             "Loaded %u achievements (schema v%u)",
             static_cast<unsigned>(unlocked_count()),
             static_cast<unsigned>(schema_version_));
  }

  return ESP_OK;
}

AchievementUpdate AchievementEngine::evaluate(
    const LifeState &life,
    const TrimPeerBook &peers,
    const DailyStats &day,
    uint16_t best_walk_streak) {
  AchievementUpdate out;

  const uint32_t qualified =
      qualifying_bits_(life, peers, day, best_walk_streak);
  const uint32_t newly = qualified & ~unlocked_bits_;

  if (newly == 0) return out;

  for (uint8_t n = 1;
       n <= static_cast<uint8_t>(AchievementType::Unstoppable);
       ++n) {
    const AchievementType type = static_cast<AchievementType>(n);
    const uint32_t bit = bit_(type);

    if (newly & bit) {
      unlocked_bits_ |= bit;
      ++out.new_count;
      out.latest = type;
      out.xp_awarded += reward_for(type);

      ESP_LOGI(TAG,
               "BADGE UNLOCKED: %s +%lu XP",
               name(type),
               static_cast<unsigned long>(reward_for(type)));
    }
  }

  esp_err_t err = save_();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Could not persist achievements: %s",
             esp_err_to_name(err));
  }

  return out;
}

uint8_t AchievementEngine::unlocked_count() const {
  uint32_t v = unlocked_bits_;
  uint8_t n = 0;
  while (v) {
    n += static_cast<uint8_t>(v & 1u);
    v >>= 1u;
  }
  return n;
}

const char *AchievementEngine::name(AchievementType type) {
  switch (type) {
    case AchievementType::FirstSteps:      return "FIRST STEPS";
    case AchievementType::TenKClub:        return "10K CLUB";
    case AchievementType::WifiWanderer:    return "WIFI WANDERER";
    case AchievementType::SocialButterfly: return "SOCIAL BUTTERFLY";
    case AchievementType::BigDayOut:       return "BIG DAY OUT";
    case AchievementType::HomeSweetHome:   return "HOME SWEET HOME";
    case AchievementType::AnotherOne:          return "ANOTHER ONE";
    case AchievementType::LevelFive:           return "LEVEL FIVE";
    case AchievementType::CenturyClub:         return "CENTURY CLUB";
    case AchievementType::Seriously:           return "SERIOUSLY?!";
    case AchievementType::TinyLegsBigMiles:    return "TINY LEGS BIG MILES";
    case AchievementType::SeasonedExplorer:    return "SEASONED EXPLORER";
    case AchievementType::WellTravelled:       return "WELL TRAVELLED";
    case AchievementType::OldHand:             return "OLD HAND";
    case AchievementType::SignalHunter:        return "SIGNAL HUNTER";
    case AchievementType::RadioMagpie:         return "RADIO MAGPIE";
    case AchievementType::TheCollector:        return "THE COLLECTOR";
    case AchievementType::ThePack:             return "THE PACK";
    case AchievementType::SocialCircle:        return "SOCIAL CIRCLE";
    case AchievementType::PopularLittleThing:  return "POPULAR LITTLE THING";
    case AchievementType::Veteran:             return "VETERAN";
    case AchievementType::Legend:              return "LEGEND";
    case AchievementType::CreatureOfHabit:     return "CREATURE OF HABIT";
    case AchievementType::Unstoppable:         return "UNSTOPPABLE";
    default:                                   return "NEW BADGE";
  }
}
