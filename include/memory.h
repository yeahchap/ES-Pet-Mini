#pragma once

#include <cstdint>
#include "esp_err.h"

struct LifeState {
  uint32_t boots = 0;
  uint32_t xp = 0;
  uint32_t level = 1;
  uint32_t epochs = 0;
  uint32_t journeys = 0;
  uint32_t unique_wifi = 0;
  int32_t best_epoch = 0;

  // Explorer / journey state.
  uint32_t best_journey = 0;
  bool journey_active = false;
  uint32_t journey_xp = 0;
  uint32_t journey_new_wifi = 0;
  uint32_t journey_epochs = 0;

  // Persistent lifetime steps.
  uint32_t steps_total = 0;

  // V0.7L.3: steps collected while the real local date is unknown.
  // They are shown provisionally as TODAY and transferred into the dated
  // daily bucket on the first successful HOME/SNTP sync.
  uint32_t pending_unsynced_steps = 0;

  // V0.5: persistent conservative BLE friend count.
  uint32_t unique_ble = 0;
};

static constexpr uint16_t WIFI_BOOK_CAPACITY = 256;

struct WifiFriendBook {
  uint32_t magic = 0x544D5746;  // "TMWF"
  uint16_t version = 1;
  uint16_t count = 0;
  uint16_t channel_mask = 0;
  uint16_t reserved = 0;
  uint64_t ids[WIFI_BOOK_CAPACITY] = {};
};

static constexpr uint16_t BLE_BOOK_CAPACITY = 256;

struct BleFriendBook {
  uint32_t magic = 0x544D4246;  // "TMBF"
  uint16_t version = 1;
  uint16_t count = 0;
  uint64_t ids[BLE_BOOK_CAPACITY] = {};
};

struct PetIdentity {
  uint32_t magic = 0x544D5049;  // "TMPI"
  uint16_t version = 1;
  uint8_t protocol_version = 1;
  uint8_t reserved = 0;
  uint64_t pet_id = 0;
};

static constexpr uint16_t TRIM_PEER_CAPACITY = 16;

struct TrimPeerRecord {
  uint64_t pet_id = 0;
  uint32_t first_seen_epoch = 0;
  uint32_t last_seen_epoch = 0;
  uint16_t encounters = 0;
  uint16_t reserved = 0;
};

struct TrimPeerBook {
  uint32_t magic = 0x544D5042;  // "TMPB"
  uint16_t version = 1;
  uint16_t count = 0;
  TrimPeerRecord peers[TRIM_PEER_CAPACITY] = {};
};

static constexpr uint16_t HOME_PROFILE_CAPACITY = 24;

struct HomeProfile {
  uint32_t magic = 0x544D484D;  // "TMHM"
  uint16_t version = 1;
  uint16_t count = 0;
  uint64_t ids[HOME_PROFILE_CAPACITY] = {};
};

esp_err_t life_memory_init();

esp_err_t life_load(LifeState &state);
esp_err_t life_save(const LifeState &state);

esp_err_t wifi_book_load(WifiFriendBook &book);
esp_err_t wifi_book_save(const WifiFriendBook &book);
bool wifi_book_contains(const WifiFriendBook &book, uint64_t id);
bool wifi_book_add(WifiFriendBook &book, uint64_t id);

esp_err_t ble_book_load(BleFriendBook &book);
esp_err_t ble_book_save(const BleFriendBook &book);
bool ble_book_contains(const BleFriendBook &book, uint64_t id);
bool ble_book_add(BleFriendBook &book, uint64_t id);

esp_err_t pet_identity_load_or_create(PetIdentity &identity);

esp_err_t trim_peer_book_load(TrimPeerBook &book);
esp_err_t trim_peer_book_save(const TrimPeerBook &book);
int trim_peer_find(const TrimPeerBook &book, uint64_t pet_id);
bool trim_peer_touch(TrimPeerBook &book, uint64_t pet_id, uint32_t epoch);

esp_err_t home_profile_load(HomeProfile &home);
esp_err_t home_profile_save(const HomeProfile &home);
bool home_profile_contains(const HomeProfile &home, uint64_t id);
