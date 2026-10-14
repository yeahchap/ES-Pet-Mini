#pragma once

#include <cstdint>
#include <vector>

#include "esp_err.h"

struct BleObservation {
  uint64_t anonymous_id = 0;
  int8_t rssi = -127;
};

struct TrimPeerSighting {
  uint64_t pet_id = 0;
  int8_t rssi = -127;
  uint8_t protocol_version = 0;
  uint8_t capabilities = 0;
  uint8_t state_flags = 0;
};

struct BleScanResult {
  uint32_t visible_advertisers = 0;
  std::vector<BleObservation> stable_observations;
  std::vector<TrimPeerSighting> trimagotchi_peers;
  int8_t strongest_rssi = -127;
};

class BleSense {
 public:
  esp_err_t begin(uint64_t pet_id);

  // Manual light sleep on ESP32-C6 requires Bluetooth to be disabled first.
  // SOCIAL SLEEP therefore tears NimBLE down before SNOOZE and restores it
  // after wake. Pet identity and friend memory live outside the BLE stack.
  esp_err_t sleep();
  esp_err_t wake();

  bool ready() const { return ready_; }
  bool stack_running() const { return stack_running_; }

  esp_err_t sense(BleScanResult &result,
                  bool walking,
                  bool journey_active,
                  uint32_t duration_ms = 3500);

  uint64_t pet_id() const { return pet_id_; }

 private:
  static void host_task_(void *param);
  static void host_sync_();
  static void host_reset_(int reason);
  esp_err_t start_stack_();
  esp_err_t stop_stack_();
  static int gap_event_(struct ble_gap_event *event, void *arg);

  int handle_gap_event_(struct ble_gap_event *event);
  void handle_advertisement_(const struct ble_gap_disc_desc &disc);

  bool address_is_stable_(const uint8_t addr[6], uint8_t addr_type) const;
  uint64_t anonymise_stable_address_(const uint8_t addr[6],
                                     uint8_t addr_type) const;

  void build_identity_advertisement_(uint8_t out[29],
                                     bool walking,
                                     bool journey_active) const;
  bool parse_trimagotchi_packet_(const uint8_t *data,
                                 uint8_t len,
                                 TrimPeerSighting &peer) const;

  uint64_t pet_id_ = 0;
  uint8_t own_addr_type_ = 0;
  volatile bool ready_ = false;
  volatile bool stack_running_ = false;
  volatile bool stopping_ = false;

  void *sync_sem_ = nullptr;
  void *scan_sem_ = nullptr;
  void *host_returned_sem_ = nullptr;
  BleScanResult *active_result_ = nullptr;

  static BleSense *instance_;
};
