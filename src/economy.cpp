#include "economy.h"

#include "nvs.h"

namespace {

constexpr const char *NAMESPACE_NAME = "trimecon";
constexpr const char *VERSION_KEY = "version";
constexpr const char *DATE_KEY = "date";
constexpr const char *RADIO_XP_KEY = "radioxp";
constexpr uint8_t ECONOMY_VERSION = 1;

}  // namespace

esp_err_t EconomyTracker::begin() {
  date_key_ = 0;
  radio_xp_today_ = 0;
  initialised_ = false;
  dirty_ = false;

  nvs_handle_t h;
  esp_err_t err = nvs_open(NAMESPACE_NAME, NVS_READONLY, &h);
  if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
  if (err != ESP_OK) return err;

  uint8_t version = 0;
  uint32_t stored_date = 0;
  uint32_t stored_radio_xp = 0;

  const esp_err_t version_err = nvs_get_u8(h, VERSION_KEY, &version);
  const esp_err_t date_err = nvs_get_u32(h, DATE_KEY, &stored_date);
  const esp_err_t radio_err = nvs_get_u32(h, RADIO_XP_KEY, &stored_radio_xp);
  nvs_close(h);

  if (version_err != ESP_OK || date_err != ESP_OK ||
      radio_err != ESP_OK || version != ECONOMY_VERSION) {
    return ESP_OK;
  }

  date_key_ = stored_date;
  radio_xp_today_ =
      stored_radio_xp > es_pet::economy::kDailyRadioXpCap
          ? es_pet::economy::kDailyRadioXpCap
          : stored_radio_xp;
  initialised_ = true;
  return ESP_OK;
}

void EconomyTracker::initialise_undated_() {
  if (initialised_) return;
  date_key_ = 0;
  radio_xp_today_ = 0;
  initialised_ = true;
  dirty_ = true;
}

bool EconomyTracker::ensure_day(uint32_t date_key) {
  if (date_key == 0) return false;

  initialise_undated_();

  if (date_key_ == 0) {
    // The first HOME time sync adopts the undated allowance already consumed
    // during this boot, preventing a second 100 XP bucket on the same day.
    date_key_ = date_key;
    dirty_ = true;
    return true;
  }

  if (date_key_ == date_key) return false;

  date_key_ = date_key;
  radio_xp_today_ = 0;
  dirty_ = true;
  return true;
}

uint32_t EconomyTracker::award_radio(uint32_t requested) {
  initialise_undated_();

  const uint32_t awarded =
      es_pet::economy::capped_radio_award(radio_xp_today_, requested);
  if (awarded == 0) return 0;

  radio_xp_today_ += awarded;
  dirty_ = true;
  return awarded;
}

esp_err_t EconomyTracker::save() {
  if (!dirty_) return ESP_OK;

  nvs_handle_t h;
  esp_err_t err = nvs_open(NAMESPACE_NAME, NVS_READWRITE, &h);
  if (err != ESP_OK) return err;

  err = nvs_set_u8(h, VERSION_KEY, ECONOMY_VERSION);
  if (err == ESP_OK) err = nvs_set_u32(h, DATE_KEY, date_key_);
  if (err == ESP_OK) err = nvs_set_u32(h, RADIO_XP_KEY, radio_xp_today_);
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);

  if (err == ESP_OK) dirty_ = false;
  return err;
}
