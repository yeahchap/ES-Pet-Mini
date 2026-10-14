#pragma once

#include <cstdint>

#include "memory.h"
#include "wifi_sense.h"

static constexpr uint8_t HOME_LEARN_EPOCHS = 3;

struct ExplorerUpdate {
  bool home_profile_created = false;
  bool journey_started = false;
  bool journey_ended = false;

  bool home_confident = false;
  bool away_confident = false;

  float home_similarity = 0.0f;

  uint32_t completed_journey_xp = 0;
  uint32_t completed_journey_new = 0;
  uint32_t completed_journey_epochs = 0;
};

class Explorer {
 public:
  Explorer();

  bool needs_home_learning(const HomeProfile &home) const;
  uint8_t learning_epoch() const;

  ExplorerUpdate update(HomeProfile &home,
                        LifeState &life,
                        const WifiScanResult &scan,
                        uint32_t epoch_xp,
                        uint32_t epoch_new_friends);

 private:
  static constexpr uint16_t LEARN_CAPACITY = 64;

  uint64_t learn_ids_[LEARN_CAPACITY];
  uint8_t learn_hits_[LEARN_CAPACITY];
  uint16_t learn_count_;
  uint8_t learn_epoch_;

  uint8_t away_streak_;
  uint8_t home_streak_;

  uint32_t pending_away_xp_;
  uint32_t pending_away_new_;
  uint32_t pending_away_epochs_;

  void learn_from_scan_(const WifiScanResult &scan);
  bool finalise_home_(HomeProfile &home);

  float home_similarity_(const HomeProfile &home,
                         const WifiScanResult &scan) const;
};
