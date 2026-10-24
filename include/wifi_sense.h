#pragma once

#include <cstdint>
#include <vector>

#include "esp_err.h"
#include "esp_event.h"

struct WifiObservation {
  uint64_t anonymous_id = 0;
  int8_t rssi = -127;
  uint8_t channel = 0;
};

struct WifiScanResult {
  std::vector<WifiObservation> observations;
  int8_t strongest_rssi = -127;
  uint16_t channel_mask = 0;
  // Ephemeral clock hint. SSID text is not added to the observation book.
  bool saved_home_visible = false;
};

class WifiSense {
 public:
  esp_err_t begin();
  esp_err_t passive_scan(WifiScanResult &result,
                         const char *saved_home_ssid = nullptr);

  // V0.6.1 normally remains a passive scanner, but can briefly connect to the
  // owner's HOME AP for SNTP. Credentials are supplied by TimeKeeper and are
  // stored in RAM by the Wi-Fi driver only.
  esp_err_t connect_for_internet(const char *ssid,
                                 const char *password,
                                 uint32_t timeout_ms = 15000);
  void disconnect_from_internet();
  bool internet_connected() const { return internet_connected_; }

  // One-time local provisioning mode for HOME Wi-Fi credentials.
  esp_err_t start_setup_ap(const char *ssid, const char *password);
  esp_err_t stop_setup_ap();

  // Explicit radio power control for SNOOZE.
  esp_err_t sleep();
  esp_err_t wake();

 private:
  static void event_handler_(void *arg,
                             esp_event_base_t event_base,
                             int32_t event_id,
                             void *event_data);

  uint64_t anonymise_bssid_(const uint8_t bssid[6]) const;

  bool started_ = false;
  bool connecting_ = false;
  bool internet_connected_ = false;
  bool setup_ap_active_ = false;

  void *event_group_ = nullptr;
  void *sta_netif_ = nullptr;
  void *ap_netif_ = nullptr;
};
