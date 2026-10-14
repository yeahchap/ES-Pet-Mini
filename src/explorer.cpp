#include "explorer.h"

#include <algorithm>
#include <cstring>

#include "esp_log.h"

static const char *TAG = "trim_explorer";

Explorer::Explorer()
    : learn_ids_{},
      learn_hits_{},
      learn_count_(0),
      learn_epoch_(0),
      away_streak_(0),
      home_streak_(0),
      pending_away_xp_(0),
      pending_away_new_(0),
      pending_away_epochs_(0) {}

bool Explorer::needs_home_learning(const HomeProfile &home) const {
  return home.count == 0;
}

uint8_t Explorer::learning_epoch() const {
  return learn_epoch_;
}

void Explorer::learn_from_scan_(const WifiScanResult &scan) {
  ++learn_epoch_;

  // Count each anonymous radio at most once in this learning epoch.
  for (const auto &obs : scan.observations) {
    int found = -1;
    for (uint16_t i = 0; i < learn_count_; ++i) {
      if (learn_ids_[i] == obs.anonymous_id) {
        found = static_cast<int>(i);
        break;
      }
    }

    if (found >= 0) {
      if (learn_hits_[found] < 255) ++learn_hits_[found];
    } else if (learn_count_ < LEARN_CAPACITY) {
      learn_ids_[learn_count_] = obs.anonymous_id;
      learn_hits_[learn_count_] = 1;
      ++learn_count_;
    }
  }

  ESP_LOGI(TAG, "Learning HOME: epoch %u/%u, %u candidates",
           static_cast<unsigned>(learn_epoch_),
           static_cast<unsigned>(HOME_LEARN_EPOCHS),
           static_cast<unsigned>(learn_count_));
}

bool Explorer::finalise_home_(HomeProfile &home) {
  if (learn_epoch_ < HOME_LEARN_EPOCHS) return false;

  home = HomeProfile{};

  // Prefer radios seen in at least two of the three HOME-learning scans.
  for (uint16_t i = 0;
       i < learn_count_ && home.count < HOME_PROFILE_CAPACITY;
       ++i) {
    if (learn_hits_[i] >= 2) {
      home.ids[home.count++] = learn_ids_[i];
    }
  }

  // Very sparse environment fallback: use whatever was consistently available.
  if (home.count == 0) {
    for (uint16_t i = 0;
         i < learn_count_ && home.count < HOME_PROFILE_CAPACITY;
         ++i) {
      home.ids[home.count++] = learn_ids_[i];
    }
  }

  ESP_LOGI(TAG, "HOME learned: %u fingerprint radios",
           static_cast<unsigned>(home.count));
  return home.count > 0;
}

float Explorer::home_similarity_(const HomeProfile &home,
                                 const WifiScanResult &scan) const {
  if (home.count == 0) return 0.0f;

  uint16_t matches = 0;

  for (uint16_t i = 0; i < home.count; ++i) {
    for (const auto &obs : scan.observations) {
      if (home.ids[i] == obs.anonymous_id) {
        ++matches;
        break;
      }
    }
  }

  return static_cast<float>(matches) /
         static_cast<float>(home.count);
}

ExplorerUpdate Explorer::update(HomeProfile &home,
                                LifeState &life,
                                const WifiScanResult &scan,
                                uint32_t epoch_xp,
                                uint32_t epoch_new_friends) {
  ExplorerUpdate u;

  if (home.count == 0) {
    learn_from_scan_(scan);

    if (learn_epoch_ >= HOME_LEARN_EPOCHS && finalise_home_(home)) {
      u.home_profile_created = true;
      u.home_similarity = 1.0f;
      u.home_confident = true;
    }

    return u;
  }

  u.home_similarity = home_similarity_(home, scan);

  // Hysteresis: >=50% is confidently HOME; <=25% confidently AWAY.
  // Middle ground is deliberately "not sure" so one weak scan doesn't
  // start/end an adventure.
  u.home_confident = u.home_similarity >= 0.50f;
  u.away_confident = u.home_similarity <= 0.25f;

  if (!life.journey_active) {
    if (u.away_confident) {
      if (away_streak_ < 255) ++away_streak_;
      home_streak_ = 0;

      pending_away_xp_ += epoch_xp;
      pending_away_new_ += epoch_new_friends;
      ++pending_away_epochs_;

      if (away_streak_ >= 3) {
        life.journey_active = true;
        ++life.journeys;

        // Include the three confirmation epochs so the first part of a walk
        // still counts.
        life.journey_xp = pending_away_xp_;
        life.journey_new_wifi = pending_away_new_;
        life.journey_epochs = pending_away_epochs_;

        pending_away_xp_ = 0;
        pending_away_new_ = 0;
        pending_away_epochs_ = 0;

        away_streak_ = 0;
        u.journey_started = true;

        ESP_LOGI(TAG,
                 "ADVENTURE STARTED: journey=%lu seed_xp=%lu seed_new=%lu",
                 static_cast<unsigned long>(life.journeys),
                 static_cast<unsigned long>(life.journey_xp),
                 static_cast<unsigned long>(life.journey_new_wifi));
      }
    } else {
      away_streak_ = 0;
      pending_away_xp_ = 0;
      pending_away_new_ = 0;
      pending_away_epochs_ = 0;

      if (u.home_confident && home_streak_ < 255) ++home_streak_;
      else home_streak_ = 0;
    }

    return u;
  }

  // Journey is active.
  ++life.journey_epochs;
  life.journey_xp += epoch_xp;
  life.journey_new_wifi += epoch_new_friends;

  if (u.home_confident) {
    if (home_streak_ < 255) ++home_streak_;
    away_streak_ = 0;

    if (home_streak_ >= 3) {
      u.journey_ended = true;
      u.completed_journey_xp = life.journey_xp;
      u.completed_journey_new = life.journey_new_wifi;
      u.completed_journey_epochs = life.journey_epochs;

      life.best_journey =
          std::max(life.best_journey, life.journey_xp);

      ESP_LOGI(TAG,
               "HOME AGAIN: xp=%lu new=%lu epochs=%lu best=%lu",
               static_cast<unsigned long>(life.journey_xp),
               static_cast<unsigned long>(life.journey_new_wifi),
               static_cast<unsigned long>(life.journey_epochs),
               static_cast<unsigned long>(life.best_journey));

      life.journey_active = false;
      life.journey_xp = 0;
      life.journey_new_wifi = 0;
      life.journey_epochs = 0;

      home_streak_ = 0;
    }
  } else {
    home_streak_ = 0;
    if (u.away_confident && away_streak_ < 255) ++away_streak_;
    else away_streak_ = 0;
  }

  return u;
}
