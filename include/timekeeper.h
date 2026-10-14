#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

class WifiSense;

class TimeKeeper {
 public:
  esp_err_t begin();

  bool configured() const { return configured_; }
  bool valid() const;

  // First-run setup identity / user-selected local settings.
  const char *setup_ap_name() const { return setup_ap_name_; }
  const char *pet_name() const { return settings_.pet_name; }
  const char *timezone_label() const { return settings_.timezone_label; }

  // Blocks for up to timeout_ms waiting for a local browser form. Credentials
  // never leave the Trimagotchi/local Wi-Fi link and are stored in NVS.
  esp_err_t run_setup_portal(WifiSense &wifi,
                             uint32_t timeout_ms,
                             bool &saved);

  bool should_sync(bool home_confident) const;
  esp_err_t sync_at_home(WifiSense &wifi);

  // Used only by the local provisioning HTTP handler.
  esp_err_t save_provisioned_settings(const char *ssid,
                                      const char *password,
                                      const char *pet_name,
                                      const char *timezone_label,
                                      const char *posix_tz);

  int64_t unix_time() const;
  uint32_t local_date_key() const;  // YYYYMMDD, or 0 when clock invalid.
  uint8_t local_hour() const;          // 0..23, or 0 when clock invalid.

  void format_local_date(char *out, size_t len) const;      // SAT 15 AUG
  void format_local_datetime(char *out, size_t len) const;  // 15 AUG 16:42

 private:
  struct Settings {
    uint32_t magic = 0x544D5354;  // TMST
    uint16_t version = 2;
    uint16_t reserved = 0;

    char ssid[33] = {};
    char password[65] = {};

    // Kept intentionally small so it fits the existing e-paper header/font.
    // Provisioning sanitises this to uppercase ASCII, digits, space and '-'.
    char pet_name[17] = "TRIMAGOTCHI";

    // Human-readable choice for logs/setup screens.
    char timezone_label[33] = "UTC";

    // Standard POSIX TZ rule consumed by libc setenv("TZ", ...)/tzset().
    char posix_tz[65] = "UTC0";
  };

  esp_err_t load_settings_();
  esp_err_t save_settings_(const char *ssid,
                           const char *password,
                           const char *pet_name,
                           const char *timezone_label,
                           const char *posix_tz);
  void apply_timezone_() const;

  static void make_setup_name_(char *out, size_t len);
  static void sanitise_pet_name_(const char *input, char *out, size_t len);

  Settings settings_{};
  bool configured_ = false;
  int64_t last_sync_monotonic_us_ = -1;
  int64_t last_attempt_monotonic_us_ = -1;
  char setup_ap_name_[33] = {};
};
