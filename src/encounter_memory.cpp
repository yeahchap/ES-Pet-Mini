#include "encounter_memory.h"

#include <algorithm>
#include <cstring>

#include "esp_log.h"
#include "esp_partition.h"
#include "nvs.h"

static const char *TAG = "trim_diary";

namespace {
constexpr const char *NAMESPACE_NAME = "trimdiary";
constexpr uint32_t LAYOUT_VERSION = 1;

constexpr uint32_t WIFI_LIFE_OFF = 0x00000;
constexpr uint32_t WIFI_LIFE_SIZE = 0x40000;  // 256 KiB
constexpr uint32_t BLE_LIFE_OFF = 0x40000;
constexpr uint32_t BLE_LIFE_SIZE = 0x40000;   // 256 KiB
constexpr uint32_t WIFI_DAY_OFF = 0x80000;
constexpr uint32_t WIFI_DAY_SIZE = 0x04000;   // 16 KiB
constexpr uint32_t BLE_DAY_OFF = 0x84000;
constexpr uint32_t BLE_DAY_SIZE = 0x04000;    // 16 KiB
constexpr uint32_t SUMMARY_OFF = 0x88000;
constexpr uint32_t SUMMARY_SIZE = 0x10000;    // 64 KiB
constexpr uint32_t EVENT_OFF = 0x98000;
constexpr uint32_t EVENT_SIZE = 0x10000;      // 64 KiB
constexpr uint32_t USED_END = 0xA8000;
constexpr uint32_t ERASE_SECTOR = 0x1000;

constexpr uint8_t LIFE_HASHES = 7;
constexpr uint8_t DAY_HASHES = 6;

#pragma pack(push, 1)
struct DailySummaryRecord {
  uint32_t magic = 0x544D4453;  // TMDS
  uint32_t date_key = 0;
  uint32_t steps = 0;
  uint32_t wifi_unique = 0;
  uint32_t ble_unique = 0;
  uint32_t xp = 0;
  uint16_t adventures = 0;
  uint16_t peer_meetings = 0;
  uint32_t reserved = 0;
};

struct EventRecord {
  uint32_t magic = 0x544D4556;  // TMEV
  uint8_t type = 0;
  uint8_t version = 1;
  uint16_t flags = 0;
  int64_t unix_time = 0;
  uint64_t subject_id = 0;
  uint32_t value1 = 0;
  uint32_t value2 = 0;
};
#pragma pack(pop)

static_assert(sizeof(DailySummaryRecord) == 32, "Daily summary must be 32 bytes");
static_assert(sizeof(EventRecord) == 32, "Event record must be 32 bytes");

uint64_t mix64(uint64_t x) {
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
}

uint32_t get_u32_or(nvs_handle_t h, const char *key, uint32_t fallback) {
  uint32_t v = fallback;
  return nvs_get_u32(h, key, &v) == ESP_OK ? v : fallback;
}

}  // namespace

esp_err_t EncounterMemory::begin(const WifiFriendBook &wifi_book,
                                 const BleFriendBook &ble_book,
                                 LifeState &life) {
  const esp_partition_t *part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA,
      ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
      "diary");
  if (!part) return ESP_ERR_NOT_FOUND;
  if (part->size < USED_END) return ESP_ERR_INVALID_SIZE;
  partition_ = part;

  nvs_handle_t h;
  esp_err_t err = nvs_open(NAMESPACE_NAME, NVS_READWRITE, &h);
  if (err != ESP_OK) return err;

  const uint32_t version = get_u32_or(h, "layout", 0);

  if (version != LAYOUT_VERSION) {
    ESP_LOGI(TAG,
             "Initialising TIME & MEMORY diary layout: %lu bytes used of %lu",
             static_cast<unsigned long>(USED_END),
             static_cast<unsigned long>(part->size));

    err = esp_partition_erase_range(part, 0, USED_END);
    if (err != ESP_OK) {
      nvs_close(h);
      return err;
    }

    // Seed the scalable lifetime filters with everything V0.6 already knew
    // exactly. The pre-V0.6.1 world was capped at 256, so the new lifetime
    // counter starts from the stored truth and then grows without that cap.
    for (uint16_t i = 0; i < wifi_book.count; ++i) {
      (void)bloom_test_and_add_(WIFI_LIFE_OFF, WIFI_LIFE_SIZE,
                                wifi_book.ids[i], LIFE_HASHES);
    }
    for (uint16_t i = 0; i < ble_book.count; ++i) {
      (void)bloom_test_and_add_(BLE_LIFE_OFF, BLE_LIFE_SIZE,
                                ble_book.ids[i], LIFE_HASHES);
    }

    life.unique_wifi = std::max<uint32_t>(life.unique_wifi, wifi_book.count);
    life.unique_ble = std::max<uint32_t>(life.unique_ble, ble_book.count);

    daily_ = DailyStats{};
    summary_index_ = 0;
    event_index_ = 0;

    ESP_ERROR_CHECK(nvs_set_blob(h, "daily", &daily_, sizeof(daily_)));
    ESP_ERROR_CHECK(nvs_set_u32(h, "sumidx", summary_index_));
    ESP_ERROR_CHECK(nvs_set_u32(h, "evtidx", event_index_));
    ESP_ERROR_CHECK(nvs_set_u32(h, "layout", LAYOUT_VERSION));
    err = nvs_commit(h);

    if (err == ESP_OK) {
      ESP_LOGI(TAG,
               "Lifetime memory seeded: Wi-Fi=%lu BLE=%lu; 256-entry ceiling removed",
               static_cast<unsigned long>(life.unique_wifi),
               static_cast<unsigned long>(life.unique_ble));
    }
  } else {
    size_t size = sizeof(daily_);
    if (nvs_get_blob(h, "daily", &daily_, &size) != ESP_OK ||
        size != sizeof(daily_)) {
      daily_ = DailyStats{};
    }
    summary_index_ = get_u32_or(h, "sumidx", 0);
    event_index_ = get_u32_or(h, "evtidx", 0);
  }

  nvs_close(h);

  ESP_LOGI(TAG,
           "Diary ready: life bloom 256KiB+256KiB, daily bloom 16KiB+16KiB, summaries/events enabled");
  return err;
}

bool EncounterMemory::bloom_test_and_add_(uint32_t base,
                                          uint32_t size,
                                          uint64_t id,
                                          uint8_t hashes) {
  if (!partition_ || id == 0 || size == 0) return false;

  const auto *part = static_cast<const esp_partition_t *>(partition_);
  const uint64_t bit_count = static_cast<uint64_t>(size) * 8ULL;
  const uint64_t h1 = mix64(id ^ 0x5452494D41474F54ULL);
  const uint64_t h2 = mix64(id ^ 0x534F4349414C4D45ULL) | 1ULL;

  bool already_seen = true;

  for (uint8_t i = 0; i < hashes; ++i) {
    const uint64_t bit = (h1 + static_cast<uint64_t>(i) * h2) % bit_count;
    const uint32_t byte_off = base + static_cast<uint32_t>(bit >> 3);
    const uint8_t mask = static_cast<uint8_t>(1u << (bit & 7u));

    uint8_t value = 0xFF;
    if (esp_partition_read(part, byte_off, &value, 1) != ESP_OK) {
      ESP_LOGW(TAG, "Bloom read failed at 0x%lX",
               static_cast<unsigned long>(byte_off));
      return false;
    }

    if (value & mask) {
      already_seen = false;
      const uint8_t updated = static_cast<uint8_t>(value & ~mask);
      if (esp_partition_write(part, byte_off, &updated, 1) != ESP_OK) {
        ESP_LOGW(TAG, "Bloom write failed at 0x%lX",
                 static_cast<unsigned long>(byte_off));
        return false;
      }
    }
  }

  return !already_seen;
}

bool EncounterMemory::observe_lifetime_wifi(uint64_t id) {
  return bloom_test_and_add_(WIFI_LIFE_OFF, WIFI_LIFE_SIZE, id, LIFE_HASHES);
}

bool EncounterMemory::observe_lifetime_ble(uint64_t id) {
  return bloom_test_and_add_(BLE_LIFE_OFF, BLE_LIFE_SIZE, id, LIFE_HASHES);
}

bool EncounterMemory::observe_daily_wifi(uint64_t id) {
  if (daily_.date_key == 0) return false;
  return bloom_test_and_add_(WIFI_DAY_OFF, WIFI_DAY_SIZE, id, DAY_HASHES);
}

bool EncounterMemory::observe_daily_ble(uint64_t id) {
  if (daily_.date_key == 0) return false;
  return bloom_test_and_add_(BLE_DAY_OFF, BLE_DAY_SIZE, id, DAY_HASHES);
}

esp_err_t EncounterMemory::reset_daily_filters_() {
  if (!partition_) return ESP_ERR_INVALID_STATE;
  const auto *part = static_cast<const esp_partition_t *>(partition_);
  return esp_partition_erase_range(
      part,
      WIFI_DAY_OFF,
      WIFI_DAY_SIZE + BLE_DAY_SIZE);
}

esp_err_t EncounterMemory::append_daily_summary_() {
  if (!partition_ || daily_.date_key == 0) return ESP_OK;
  const auto *part = static_cast<const esp_partition_t *>(partition_);

  constexpr uint32_t RECORDS = SUMMARY_SIZE / sizeof(DailySummaryRecord);
  if (summary_index_ >= RECORDS) {
    ESP_ERROR_CHECK(esp_partition_erase_range(part, SUMMARY_OFF, SUMMARY_SIZE));
    summary_index_ = 0;
  }

  const uint32_t off = SUMMARY_OFF + summary_index_ * sizeof(DailySummaryRecord);
  uint32_t marker = 0;
  ESP_ERROR_CHECK(esp_partition_read(part, off, &marker, sizeof(marker)));
  if (marker != 0xFFFFFFFFu) {
    ESP_ERROR_CHECK(esp_partition_erase_range(part, SUMMARY_OFF, SUMMARY_SIZE));
    summary_index_ = 0;
  }

  DailySummaryRecord r;
  r.date_key = daily_.date_key;
  r.steps = daily_.steps;
  r.wifi_unique = daily_.wifi_unique;
  r.ble_unique = daily_.ble_unique;
  r.xp = daily_.xp;
  r.adventures = daily_.adventures;
  r.peer_meetings = daily_.peer_meetings;

  const uint32_t write_off = SUMMARY_OFF + summary_index_ * sizeof(r);
  ESP_ERROR_CHECK(esp_partition_write(part, write_off, &r, sizeof(r)));
  ++summary_index_;

  ESP_LOGI(TAG,
           "Archived day %lu: steps=%lu wifi=%lu ble=%lu xp=%lu adventures=%u peers=%u",
           static_cast<unsigned long>(r.date_key),
           static_cast<unsigned long>(r.steps),
           static_cast<unsigned long>(r.wifi_unique),
           static_cast<unsigned long>(r.ble_unique),
           static_cast<unsigned long>(r.xp),
           static_cast<unsigned>(r.adventures),
           static_cast<unsigned>(r.peer_meetings));
  return ESP_OK;
}

bool EncounterMemory::ensure_day(uint32_t date_key) {
  if (date_key == 0) return false;
  if (daily_.date_key == date_key) return false;

  if (daily_.date_key != 0) {
    esp_err_t err = append_daily_summary_();
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Could not archive previous day: %s", esp_err_to_name(err));
    }
  }

  esp_err_t err = reset_daily_filters_();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Could not reset daily filters: %s", esp_err_to_name(err));
    return false;
  }

  daily_ = DailyStats{};
  daily_.date_key = date_key;
  (void)save_daily();

  ESP_LOGI(TAG, "NEW DAY: %lu", static_cast<unsigned long>(date_key));
  return true;
}

esp_err_t EncounterMemory::save_daily() {
  nvs_handle_t h;
  esp_err_t err = nvs_open(NAMESPACE_NAME, NVS_READWRITE, &h);
  if (err != ESP_OK) return err;

  err = nvs_set_blob(h, "daily", &daily_, sizeof(daily_));
  if (err == ESP_OK) err = nvs_set_u32(h, "sumidx", summary_index_);
  if (err == ESP_OK) err = nvs_set_u32(h, "evtidx", event_index_);
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  return err;
}

esp_err_t EncounterMemory::append_event(DiaryEventType type,
                                        int64_t unix_time,
                                        uint64_t subject_id,
                                        uint32_t value1,
                                        uint32_t value2) {
  if (!partition_) return ESP_ERR_INVALID_STATE;
  const auto *part = static_cast<const esp_partition_t *>(partition_);

  constexpr uint32_t RECORDS = EVENT_SIZE / sizeof(EventRecord);
  if (event_index_ >= RECORDS) {
    ESP_ERROR_CHECK(esp_partition_erase_range(part, EVENT_OFF, EVENT_SIZE));
    event_index_ = 0;
  }

  uint32_t off = EVENT_OFF + event_index_ * sizeof(EventRecord);
  uint32_t marker = 0;
  ESP_ERROR_CHECK(esp_partition_read(part, off, &marker, sizeof(marker)));
  if (marker != 0xFFFFFFFFu) {
    ESP_ERROR_CHECK(esp_partition_erase_range(part, EVENT_OFF, EVENT_SIZE));
    event_index_ = 0;
    off = EVENT_OFF;
  }

  EventRecord r;
  r.type = static_cast<uint8_t>(type);
  r.unix_time = unix_time;
  r.subject_id = subject_id;
  r.value1 = value1;
  r.value2 = value2;

  ESP_ERROR_CHECK(esp_partition_write(part, off, &r, sizeof(r)));
  ++event_index_;
  return save_daily();
}
