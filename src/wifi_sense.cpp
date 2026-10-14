#include "wifi_sense.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *TAG = "trim_wifi";

namespace {
constexpr EventBits_t GOT_IP_BIT = BIT0;
constexpr EventBits_t CONNECT_FAIL_BIT = BIT1;
}

void WifiSense::event_handler_(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data) {
  (void)event_data;
  auto *self = static_cast<WifiSense *>(arg);
  if (!self || !self->event_group_) return;

  auto group = static_cast<EventGroupHandle_t>(self->event_group_);

  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    self->internet_connected_ = true;
    self->connecting_ = false;
    xEventGroupSetBits(group, GOT_IP_BIT);
    return;
  }

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    self->internet_connected_ = false;
    if (self->connecting_) {
      xEventGroupSetBits(group, CONNECT_FAIL_BIT);
    }
  }
}

esp_err_t WifiSense::begin() {
  ESP_LOGI(TAG, "Initialising passive Wi-Fi sense");

  esp_err_t err = esp_netif_init();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

  // Passive scanning itself does not need an LwIP interface, but V0.6.1's
  // brief HOME time sync does. Create it once here before the driver starts.
  auto *sta = esp_netif_create_default_wifi_sta();
  if (!sta) return ESP_FAIL;
  sta_netif_ = sta;

  event_group_ = xEventGroupCreate();
  if (!event_group_) return ESP_ERR_NO_MEM;

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

  ESP_ERROR_CHECK(esp_event_handler_register(
      WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, event_handler_, this));
  ESP_ERROR_CHECK(esp_event_handler_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler_, this));

  ESP_ERROR_CHECK(esp_wifi_set_country_code("GB", false));
  ESP_ERROR_CHECK(esp_wifi_start());
  started_ = true;

  ESP_LOGI(TAG,
           "Wi-Fi sense ready: station mode, GB domain, passive scans by default");
  return ESP_OK;
}

esp_err_t WifiSense::sleep() {
  if (!started_) return ESP_OK;

  if (internet_connected_ || connecting_) {
    disconnect_from_internet();
  }

  esp_err_t err = esp_wifi_stop();
  if (err == ESP_OK) {
    started_ = false;
    ESP_LOGI(TAG, "Wi-Fi radio OFF for SNOOZE");
  }
  return err;
}

esp_err_t WifiSense::wake() {
  if (started_) return ESP_OK;

  if (setup_ap_active_) {
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
  } else {
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  }

  esp_err_t err = esp_wifi_start();
  if (err == ESP_OK) {
    started_ = true;
    ESP_LOGI(TAG, "Wi-Fi radio awake");
  }
  return err;
}

esp_err_t WifiSense::connect_for_internet(const char *ssid,
                                          const char *password,
                                          uint32_t timeout_ms) {
  if (!ssid || !ssid[0] || !password) return ESP_ERR_INVALID_ARG;
  if (setup_ap_active_) return ESP_ERR_INVALID_STATE;

  if (!started_) {
    esp_err_t err = wake();
    if (err != ESP_OK) return err;
  }

  wifi_config_t sta_cfg{};
  std::strncpy(reinterpret_cast<char *>(sta_cfg.sta.ssid),
               ssid, sizeof(sta_cfg.sta.ssid) - 1);
  std::strncpy(reinterpret_cast<char *>(sta_cfg.sta.password),
               password, sizeof(sta_cfg.sta.password) - 1);

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));

  auto group = static_cast<EventGroupHandle_t>(event_group_);

  for (int attempt = 1; attempt <= 3; ++attempt) {
    xEventGroupClearBits(group, GOT_IP_BIT | CONNECT_FAIL_BIT);
    connecting_ = true;
    internet_connected_ = false;

    ESP_LOGI(TAG, "HOME Wi-Fi connect attempt %d/3 to '%s'", attempt, ssid);

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
      connecting_ = false;
      ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    const EventBits_t bits = xEventGroupWaitBits(
        group,
        GOT_IP_BIT | CONNECT_FAIL_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    if (bits & GOT_IP_BIT) {
      connecting_ = false;
      internet_connected_ = true;
      ESP_LOGI(TAG, "HOME Wi-Fi connected; IP acquired");
      return ESP_OK;
    }

    connecting_ = false;
    internet_connected_ = false;
    (void)esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  return ESP_ERR_TIMEOUT;
}

void WifiSense::disconnect_from_internet() {
  connecting_ = false;
  if (started_) {
    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK) {
      ESP_LOGD(TAG, "HOME Wi-Fi disconnect returned: %s",
               esp_err_to_name(err));
    }
  }
  internet_connected_ = false;
}

esp_err_t WifiSense::start_setup_ap(const char *ssid, const char *password) {
  if (!ssid || !ssid[0] || !password || std::strlen(password) < 8) {
    return ESP_ERR_INVALID_ARG;
  }

  if (started_) {
    ESP_ERROR_CHECK(esp_wifi_stop());
    started_ = false;
  }

  if (!ap_netif_) {
    auto *ap = esp_netif_create_default_wifi_ap();
    if (!ap) return ESP_FAIL;
    ap_netif_ = ap;
  }

  wifi_config_t ap_cfg{};
  std::strncpy(reinterpret_cast<char *>(ap_cfg.ap.ssid),
               ssid, sizeof(ap_cfg.ap.ssid) - 1);
  ap_cfg.ap.ssid_len = std::strlen(ssid);
  std::strncpy(reinterpret_cast<char *>(ap_cfg.ap.password),
               password, sizeof(ap_cfg.ap.password) - 1);
  ap_cfg.ap.channel = 1;
  ap_cfg.ap.max_connection = 2;
  ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
  ap_cfg.ap.pmf_cfg.required = false;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
  ESP_ERROR_CHECK(esp_wifi_start());

  setup_ap_active_ = true;
  started_ = true;
  ESP_LOGI(TAG, "Clock setup AP ready: %s", ssid);
  return ESP_OK;
}

esp_err_t WifiSense::stop_setup_ap() {
  if (started_) {
    ESP_ERROR_CHECK(esp_wifi_stop());
    started_ = false;
  }

  setup_ap_active_ = false;
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());
  started_ = true;
  ESP_LOGI(TAG, "Clock setup AP closed; passive station mode restored");
  return ESP_OK;
}

uint64_t WifiSense::anonymise_bssid_(const uint8_t bssid[6]) const {
  uint64_t h = 1469598103934665603ULL ^ 0x5452494D41474F54ULL;
  for (int i = 0; i < 6; ++i) {
    h ^= static_cast<uint64_t>(bssid[i]);
    h *= 1099511628211ULL;
  }
  return h == 0 ? 1 : h;
}

esp_err_t WifiSense::passive_scan(WifiScanResult &result) {
  result = WifiScanResult{};

  if (!started_ || setup_ap_active_ || internet_connected_ || connecting_) {
    return ESP_ERR_INVALID_STATE;
  }

  wifi_scan_config_t scan_cfg{};
  scan_cfg.ssid = nullptr;
  scan_cfg.bssid = nullptr;
  scan_cfg.channel = 0;
  scan_cfg.show_hidden = true;
  scan_cfg.scan_type = WIFI_SCAN_TYPE_PASSIVE;
  scan_cfg.scan_time.passive = 150;

  ESP_LOGI(TAG, "Opening Wi-Fi eyes...");

  esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Passive scan failed: %s", esp_err_to_name(err));
    return err;
  }

  uint16_t found = 0;
  ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&found));

  if (found == 0) {
    ESP_LOGI(TAG, "Passive scan complete: nothing visible");
    return ESP_OK;
  }

  uint16_t requested = std::min<uint16_t>(found, 128);
  std::vector<wifi_ap_record_t> records(requested);
  uint16_t returned = requested;

  err = esp_wifi_scan_get_ap_records(&returned, records.data());
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Could not retrieve scan records: %s", esp_err_to_name(err));
    return err;
  }

  result.observations.reserve(returned);

  for (uint16_t i = 0; i < returned; ++i) {
    const auto &ap = records[i];
    WifiObservation obs;
    obs.anonymous_id = anonymise_bssid_(ap.bssid);
    obs.rssi = ap.rssi;
    obs.channel = ap.primary;
    result.observations.push_back(obs);

    if (obs.rssi > result.strongest_rssi) result.strongest_rssi = obs.rssi;
    if (obs.channel >= 1 && obs.channel <= 16) {
      result.channel_mask |= static_cast<uint16_t>(1u << (obs.channel - 1));
    }
  }

  ESP_LOGI(TAG, "Passive scan complete: %u visible (strongest %d dBm)",
           static_cast<unsigned>(returned),
           static_cast<int>(result.strongest_rssi));
  return ESP_OK;
}
