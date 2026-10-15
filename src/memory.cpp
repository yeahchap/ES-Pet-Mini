#include "memory.h"

#include <cstring>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "trim_memory";
static constexpr const char *NAMESPACE_NAME = "trimlife";
static constexpr const char *WIFI_BOOK_KEY = "wifibook";
static constexpr const char *HOME_KEY = "homeprof";
static constexpr const char *BLE_BOOK_KEY = "blebook";
static constexpr const char *PET_ID_KEY = "petident";
static constexpr const char *PEER_BOOK_KEY = "peerbook";

static constexpr uint32_t WIFI_BOOK_MAGIC = 0x544D5746;
static constexpr uint16_t WIFI_BOOK_VERSION = 1;

static constexpr uint32_t HOME_MAGIC = 0x544D484D;
static constexpr uint16_t HOME_VERSION = 1;
static constexpr uint32_t BLE_BOOK_MAGIC = 0x544D4246;
static constexpr uint16_t BLE_BOOK_VERSION = 1;
static constexpr uint32_t PET_ID_MAGIC = 0x544D5049;
static constexpr uint16_t PET_ID_VERSION = 1;
static constexpr uint32_t PEER_BOOK_MAGIC = 0x544D5042;
static constexpr uint16_t PEER_BOOK_VERSION = 1;

esp_err_t life_memory_init() {
  esp_err_t err = nvs_flash_init();

  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "NVS needs erase/re-init");
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }

  return err;
}

static uint32_t get_u32_or(nvs_handle_t h, const char *key, uint32_t fallback) {
  uint32_t value = fallback;
  if (nvs_get_u32(h, key, &value) != ESP_OK) return fallback;
  return value;
}

static int32_t get_i32_or(nvs_handle_t h, const char *key, int32_t fallback) {
  int32_t value = fallback;
  if (nvs_get_i32(h, key, &value) != ESP_OK) return fallback;
  return value;
}

static bool get_bool_or(nvs_handle_t h, const char *key, bool fallback) {
  uint8_t value = fallback ? 1 : 0;
  if (nvs_get_u8(h, key, &value) != ESP_OK) return fallback;
  return value != 0;
}

esp_err_t life_load(LifeState &state) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NAMESPACE_NAME, NVS_READONLY, &h);

  if (err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGI(TAG, "No previous life found: this is a new Trimagotchi");
    state = LifeState{};
    return ESP_OK;
  }

  if (err != ESP_OK) return err;

  state.boots       = get_u32_or(h, "boots", 0);
  state.xp          = get_u32_or(h, "xp", 0);
  state.level       = get_u32_or(h, "level", 1);
  state.epochs      = get_u32_or(h, "epochs", 0);
  state.journeys    = get_u32_or(h, "journeys", 0);
  state.unique_wifi = get_u32_or(h, "uniqwifi", 0);
  state.best_epoch  = get_i32_or(h, "bestepoch", 0);

  state.best_journey     = get_u32_or(h, "bestjour", 0);
  state.journey_active   = get_bool_or(h, "jouractive", false);
  state.journey_xp       = get_u32_or(h, "jourxp", 0);
  state.journey_new_wifi = get_u32_or(h, "journew", 0);
  state.journey_epochs   = get_u32_or(h, "jourepoch", 0);
  state.steps_total      = get_u32_or(h, "steps", 0);
  state.pending_unsynced_steps =
      get_u32_or(h, "pendsteps", 0);
  state.unique_ble       = get_u32_or(h, "uniqble", 0);

  nvs_close(h);
  return ESP_OK;
}

esp_err_t life_save(const LifeState &state) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NAMESPACE_NAME, NVS_READWRITE, &h);
  if (err != ESP_OK) return err;

#define SET_OR_DONE(call) do { err = (call); if (err != ESP_OK) goto done; } while (0)

  SET_OR_DONE(nvs_set_u32(h, "boots", state.boots));
  SET_OR_DONE(nvs_set_u32(h, "xp", state.xp));
  SET_OR_DONE(nvs_set_u32(h, "level", state.level));
  SET_OR_DONE(nvs_set_u32(h, "epochs", state.epochs));
  SET_OR_DONE(nvs_set_u32(h, "journeys", state.journeys));
  SET_OR_DONE(nvs_set_u32(h, "uniqwifi", state.unique_wifi));
  SET_OR_DONE(nvs_set_i32(h, "bestepoch", state.best_epoch));

  SET_OR_DONE(nvs_set_u32(h, "bestjour", state.best_journey));
  SET_OR_DONE(nvs_set_u8(h, "jouractive", state.journey_active ? 1 : 0));
  SET_OR_DONE(nvs_set_u32(h, "jourxp", state.journey_xp));
  SET_OR_DONE(nvs_set_u32(h, "journew", state.journey_new_wifi));
  SET_OR_DONE(nvs_set_u32(h, "jourepoch", state.journey_epochs));
  SET_OR_DONE(nvs_set_u32(h, "steps", state.steps_total));
  SET_OR_DONE(
      nvs_set_u32(h, "pendsteps", state.pending_unsynced_steps));
  SET_OR_DONE(nvs_set_u32(h, "uniqble", state.unique_ble));

  err = nvs_commit(h);

done:
#undef SET_OR_DONE
  nvs_close(h);
  return err;
}

esp_err_t wifi_book_load(WifiFriendBook &book) {
  book = WifiFriendBook{};

  nvs_handle_t h;
  esp_err_t err = nvs_open(NAMESPACE_NAME, NVS_READONLY, &h);
  if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
  if (err != ESP_OK) return err;

  size_t size = sizeof(WifiFriendBook);
  err = nvs_get_blob(h, WIFI_BOOK_KEY, &book, &size);
  nvs_close(h);

  if (err == ESP_ERR_NVS_NOT_FOUND) {
    book = WifiFriendBook{};
    return ESP_OK;
  }

  if (err != ESP_OK ||
      size != sizeof(WifiFriendBook) ||
      book.magic != WIFI_BOOK_MAGIC ||
      book.version != WIFI_BOOK_VERSION ||
      book.count > WIFI_BOOK_CAPACITY) {
    ESP_LOGW(TAG, "Friend book invalid/mismatched; starting fresh");
    book = WifiFriendBook{};
    return ESP_OK;
  }

  ESP_LOGI(TAG, "Loaded %u remembered Wi-Fi friends",
           static_cast<unsigned>(book.count));
  return ESP_OK;
}

esp_err_t wifi_book_save(const WifiFriendBook &book) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NAMESPACE_NAME, NVS_READWRITE, &h);
  if (err != ESP_OK) return err;

  err = nvs_set_blob(h, WIFI_BOOK_KEY, &book, sizeof(book));
  if (err == ESP_OK) err = nvs_commit(h);

  nvs_close(h);
  return err;
}

bool wifi_book_contains(const WifiFriendBook &book, uint64_t id) {
  if (id == 0) return false;
  for (uint16_t i = 0; i < book.count; ++i) {
    if (book.ids[i] == id) return true;
  }
  return false;
}

bool wifi_book_add(WifiFriendBook &book, uint64_t id) {
  if (id == 0 || wifi_book_contains(book, id)) return false;
  if (book.count >= WIFI_BOOK_CAPACITY) return false;
  book.ids[book.count++] = id;
  return true;
}

esp_err_t ble_book_load(BleFriendBook &book) {
  book = BleFriendBook{};
  nvs_handle_t h;
  esp_err_t err = nvs_open(NAMESPACE_NAME, NVS_READONLY, &h);
  if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
  if (err != ESP_OK) return err;
  size_t size = sizeof(BleFriendBook);
  err = nvs_get_blob(h, BLE_BOOK_KEY, &book, &size);
  nvs_close(h);
  if (err == ESP_ERR_NVS_NOT_FOUND) { book = BleFriendBook{}; return ESP_OK; }
  if (err != ESP_OK || size != sizeof(BleFriendBook) ||
      book.magic != BLE_BOOK_MAGIC || book.version != BLE_BOOK_VERSION ||
      book.count > BLE_BOOK_CAPACITY) {
    ESP_LOGW(TAG, "BLE friend book invalid/mismatched; starting fresh");
    book = BleFriendBook{};
    return ESP_OK;
  }
  ESP_LOGI(TAG, "Loaded %u remembered BLE friends", static_cast<unsigned>(book.count));
  return ESP_OK;
}

esp_err_t ble_book_save(const BleFriendBook &book) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NAMESPACE_NAME, NVS_READWRITE, &h);
  if (err != ESP_OK) return err;
  err = nvs_set_blob(h, BLE_BOOK_KEY, &book, sizeof(book));
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  return err;
}

bool ble_book_contains(const BleFriendBook &book, uint64_t id) {
  if (id == 0) return false;
  for (uint16_t i = 0; i < book.count; ++i) if (book.ids[i] == id) return true;
  return false;
}

bool ble_book_add(BleFriendBook &book, uint64_t id) {
  if (id == 0 || ble_book_contains(book, id)) return false;
  if (book.count >= BLE_BOOK_CAPACITY) return false;
  book.ids[book.count++] = id;
  return true;
}

esp_err_t pet_identity_load_or_create(PetIdentity &identity) {
  identity = PetIdentity{};
  nvs_handle_t h;
  esp_err_t err = nvs_open(NAMESPACE_NAME, NVS_READWRITE, &h);
  if (err != ESP_OK) return err;

  size_t size = sizeof(PetIdentity);
  PetIdentity stored{};
  err = nvs_get_blob(h, PET_ID_KEY, &stored, &size);
  if (err == ESP_OK && size == sizeof(PetIdentity) &&
      stored.magic == PET_ID_MAGIC && stored.version == PET_ID_VERSION &&
      stored.pet_id != 0) {
    identity = stored;
    nvs_close(h);
    ESP_LOGI(TAG, "Loaded permanent Pet identity");
    return ESP_OK;
  }

  uint64_t id = 0;
  do { esp_fill_random(&id, sizeof(id)); } while (id == 0);
  identity = PetIdentity{};
  identity.pet_id = id;
  err = nvs_set_blob(h, PET_ID_KEY, &identity, sizeof(identity));
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "BORN SOCIAL: created permanent Pet identity");
  }
  return err;
}

esp_err_t trim_peer_book_load(TrimPeerBook &book) {
  book = TrimPeerBook{};
  nvs_handle_t h;
  esp_err_t err = nvs_open(NAMESPACE_NAME, NVS_READONLY, &h);
  if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
  if (err != ESP_OK) return err;
  size_t size = sizeof(TrimPeerBook);
  err = nvs_get_blob(h, PEER_BOOK_KEY, &book, &size);
  nvs_close(h);
  if (err == ESP_ERR_NVS_NOT_FOUND) { book = TrimPeerBook{}; return ESP_OK; }
  if (err != ESP_OK || size != sizeof(TrimPeerBook) ||
      book.magic != PEER_BOOK_MAGIC || book.version != PEER_BOOK_VERSION ||
      book.count > TRIM_PEER_CAPACITY) {
    ESP_LOGW(TAG, "Peer book invalid/mismatched; starting fresh");
    book = TrimPeerBook{};
    return ESP_OK;
  }
  ESP_LOGI(TAG, "Loaded %u remembered Trimagotchi peers", static_cast<unsigned>(book.count));
  return ESP_OK;
}

esp_err_t trim_peer_book_save(const TrimPeerBook &book) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NAMESPACE_NAME, NVS_READWRITE, &h);
  if (err != ESP_OK) return err;
  err = nvs_set_blob(h, PEER_BOOK_KEY, &book, sizeof(book));
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  return err;
}

int trim_peer_find(const TrimPeerBook &book, uint64_t pet_id) {
  if (pet_id == 0) return -1;
  for (uint16_t i = 0; i < book.count; ++i)
    if (book.peers[i].pet_id == pet_id) return static_cast<int>(i);
  return -1;
}

bool trim_peer_touch(TrimPeerBook &book, uint64_t pet_id, uint32_t epoch) {
  if (pet_id == 0) return false;
  const int i = trim_peer_find(book, pet_id);
  if (i >= 0) {
    book.peers[i].last_seen_epoch = epoch;
    return false;
  }
  if (book.count >= TRIM_PEER_CAPACITY) return false;
  TrimPeerRecord &p = book.peers[book.count++];
  p.pet_id = pet_id;
  p.first_seen_epoch = epoch;
  p.last_seen_epoch = epoch;
  p.encounters = 1;
  return true;
}

esp_err_t home_profile_load(HomeProfile &home) {
  home = HomeProfile{};

  nvs_handle_t h;
  esp_err_t err = nvs_open(NAMESPACE_NAME, NVS_READONLY, &h);
  if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
  if (err != ESP_OK) return err;

  size_t size = sizeof(HomeProfile);
  err = nvs_get_blob(h, HOME_KEY, &home, &size);
  nvs_close(h);

  if (err == ESP_ERR_NVS_NOT_FOUND) {
    home = HomeProfile{};
    return ESP_OK;
  }

  if (err != ESP_OK ||
      size != sizeof(HomeProfile) ||
      home.magic != HOME_MAGIC ||
      home.version != HOME_VERSION ||
      home.count > HOME_PROFILE_CAPACITY) {
    ESP_LOGW(TAG, "Home profile invalid/mismatched; relearning home");
    home = HomeProfile{};
    return ESP_OK;
  }

  ESP_LOGI(TAG, "Loaded HOME fingerprint with %u radios",
           static_cast<unsigned>(home.count));
  return ESP_OK;
}

esp_err_t home_profile_save(const HomeProfile &home) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NAMESPACE_NAME, NVS_READWRITE, &h);
  if (err != ESP_OK) return err;

  err = nvs_set_blob(h, HOME_KEY, &home, sizeof(home));
  if (err == ESP_OK) err = nvs_commit(h);

  nvs_close(h);
  return err;
}

bool home_profile_contains(const HomeProfile &home, uint64_t id) {
  if (id == 0) return false;
  for (uint16_t i = 0; i < home.count; ++i) {
    if (home.ids[i] == id) return true;
  }
  return false;
}
