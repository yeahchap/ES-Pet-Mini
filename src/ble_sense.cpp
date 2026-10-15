#include "ble_sense.h"

#include <algorithm>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

static const char *TAG = "trim_ble";
BleSense *BleSense::instance_ = nullptr;

namespace {

// Trimagotchi Social Protocol v1 namespace. This is an opaque custom 128-bit
// UUID, not a Bluetooth SIG 16-bit service number and not manufacturer data.
static constexpr uint8_t TRIM_SERVICE_UUID[16] = {
    0x01, 0x00, 0x4D, 0x49,
    0x52, 0x54, 0xC1, 0xB7,
    0x70, 0x4A, 0x11, 0x4F,
    0x3A, 0x5D, 0x9E, 0x7C
};

static constexpr uint8_t TRIM_PROTOCOL_VERSION = 1;
static constexpr uint8_t AD_TYPE_SERVICE_DATA_UUID128 = 0x21;

static constexpr uint8_t CAP_WIFI      = 1u << 0;
static constexpr uint8_t CAP_BLE       = 1u << 1;
static constexpr uint8_t CAP_IMU       = 1u << 2;
static constexpr uint8_t CAP_PEDOMETER = 1u << 3;
static constexpr uint8_t CAP_SNOOZE    = 1u << 4;
static constexpr uint8_t OUR_CAPABILITIES =
    CAP_WIFI | CAP_BLE | CAP_IMU | CAP_PEDOMETER | CAP_SNOOZE;

static constexpr uint8_t STATE_WALKING = 1u << 0;
static constexpr uint8_t STATE_JOURNEY = 1u << 1;

static uint64_t read_le64(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(p[i]) << (8 * i);
  }
  return v;
}

static void write_le64(uint8_t *p, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFFu);
  }
}

}  // namespace

esp_err_t BleSense::begin(uint64_t pet_id) {
  if (pet_id == 0) return ESP_ERR_INVALID_ARG;
  if (instance_ != nullptr && instance_ != this) return ESP_ERR_INVALID_STATE;

  pet_id_ = pet_id;
  instance_ = this;

  if (!sync_sem_) sync_sem_ = xSemaphoreCreateBinary();
  if (!scan_sem_) scan_sem_ = xSemaphoreCreateBinary();
  if (!host_returned_sem_) host_returned_sem_ = xSemaphoreCreateBinary();
  if (!sync_sem_ || !scan_sem_ || !host_returned_sem_) return ESP_ERR_NO_MEM;

  ESP_LOGI(TAG, "Initialising NimBLE passive social sense");
  return start_stack_();
}

esp_err_t BleSense::start_stack_() {
  if (stack_running_ && ready_) return ESP_OK;

  // Clear stale semaphore tokens from a previous host lifecycle.
  auto sync_sem = static_cast<SemaphoreHandle_t>(sync_sem_);
  auto host_sem = static_cast<SemaphoreHandle_t>(host_returned_sem_);
  while (xSemaphoreTake(sync_sem, 0) == pdTRUE) {}
  while (xSemaphoreTake(host_sem, 0) == pdTRUE) {}

  ready_ = false;
  stopping_ = false;

  esp_err_t err = nimble_port_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
    return err;
  }

  stack_running_ = true;
  ble_hs_cfg.reset_cb = host_reset_;
  ble_hs_cfg.sync_cb = host_sync_;
  nimble_port_freertos_init(host_task_);

  if (xSemaphoreTake(sync_sem, pdMS_TO_TICKS(5000)) != pdTRUE || !ready_) {
    ESP_LOGE(TAG, "Timed out waiting for NimBLE host sync");
    (void)stop_stack_();
    return ESP_ERR_TIMEOUT;
  }

  ESP_LOGI(TAG, "BLE SOCIAL ready: protocol=v%u",
           static_cast<unsigned>(TRIM_PROTOCOL_VERSION));
  return ESP_OK;
}

esp_err_t BleSense::stop_stack_() {
  if (!stack_running_) {
    ready_ = false;
    return ESP_OK;
  }

  // No scan/advertising procedure is allowed to survive into teardown.
  if (ble_gap_disc_active()) {
    (void)ble_gap_disc_cancel();
  }
  if (ble_gap_adv_active()) {
    (void)ble_gap_adv_stop();
  }

  active_result_ = nullptr;
  ready_ = false;
  stopping_ = true;

  auto host_sem = static_cast<SemaphoreHandle_t>(host_returned_sem_);
  while (xSemaphoreTake(host_sem, 0) == pdTRUE) {}

  const int rc = nimble_port_stop();
  if (rc != 0) {
    stopping_ = false;
    ESP_LOGE(TAG, "nimble_port_stop failed: rc=%d", rc);
    return ESP_FAIL;
  }

  // nimble_port_stop() asks nimble_port_run() to return asynchronously.
  // Wait until the host task has reached its shutdown path before deinit.
  if (xSemaphoreTake(host_sem, pdMS_TO_TICKS(3000)) != pdTRUE) {
    stopping_ = false;
    ESP_LOGE(TAG, "Timed out waiting for NimBLE host task to stop");
    return ESP_ERR_TIMEOUT;
  }

  const int deinit_rc = nimble_port_deinit();
  if (deinit_rc != 0) {
    stopping_ = false;
    ESP_LOGE(TAG, "nimble_port_deinit failed: rc=%d", deinit_rc);
    return ESP_FAIL;
  }

  stack_running_ = false;
  stopping_ = false;
  ESP_LOGI(TAG, "BLE SOCIAL asleep: NimBLE stopped/deinitialised");
  return ESP_OK;
}

esp_err_t BleSense::sleep() {
  return stop_stack_();
}

esp_err_t BleSense::wake() {
  if (stack_running_ && ready_) return ESP_OK;

  ESP_LOGI(TAG, "BLE SOCIAL waking: reinitialising NimBLE");
  return start_stack_();
}

void BleSense::host_task_(void *param) {
  (void)param;
  ESP_LOGI(TAG, "NimBLE host task started");
  nimble_port_run();

  // nimble_port_run() returns when nimble_port_stop() succeeds. Signal the
  // application before the FreeRTOS port helper deletes this host task.
  if (instance_ && instance_->host_returned_sem_) {
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(
        instance_->host_returned_sem_));
  }

  nimble_port_freertos_deinit();
}

void BleSense::host_sync_() {
  if (!instance_) return;

  int rc = ble_hs_util_ensure_addr(0);
  if (rc != 0) {
    ESP_LOGE(TAG, "Could not ensure BLE identity address: rc=%d", rc);
    return;
  }

  rc = ble_hs_id_infer_auto(0, &instance_->own_addr_type_);
  if (rc != 0) {
    ESP_LOGE(TAG, "Could not infer BLE address type: rc=%d", rc);
    return;
  }

  instance_->ready_ = true;
  if (instance_->sync_sem_) {
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(instance_->sync_sem_));
  }
}

void BleSense::host_reset_(int reason) {
  if (instance_ && instance_->stopping_) {
    ESP_LOGI(TAG, "NimBLE host stopping: reason=%d", reason);
  } else {
    ESP_LOGW(TAG, "NimBLE host reset: reason=%d", reason);
  }
  if (instance_) instance_->ready_ = false;
}

bool BleSense::address_is_stable_(const uint8_t addr[6],
                                  uint8_t addr_type) const {
  if (addr_type == BLE_ADDR_PUBLIC) return true;
  if (addr_type == BLE_ADDR_RANDOM) {
    // Static-random BLE addresses have both top bits set. RPA/NRPA addresses
    // rotate and are deliberately excluded from the XP friend book.
    return (addr[5] & 0xC0u) == 0xC0u;
  }
  return false;
}

uint64_t BleSense::anonymise_stable_address_(const uint8_t addr[6],
                                             uint8_t addr_type) const {
  uint64_t h = 1469598103934665603ULL ^ 0x424C455452494D35ULL;
  h ^= static_cast<uint64_t>(addr_type);
  h *= 1099511628211ULL;
  for (int i = 0; i < 6; ++i) {
    h ^= static_cast<uint64_t>(addr[i]);
    h *= 1099511628211ULL;
  }
  return h == 0 ? 1 : h;
}

void BleSense::build_identity_advertisement_(uint8_t out[29],
                                             bool walking,
                                             bool journey_active) const {
  // 29-byte legacy advertising payload:
  //   len, AD-type(0x21), 128-bit protocol UUID,
  //   protocol-v, capabilities, state, permanent 64-bit Pet ID.
  // No human name and no raw MAC are broadcast by our application packet.
  std::memset(out, 0, 29);
  out[0] = 28;
  out[1] = AD_TYPE_SERVICE_DATA_UUID128;
  std::memcpy(&out[2], TRIM_SERVICE_UUID, sizeof(TRIM_SERVICE_UUID));
  out[18] = TRIM_PROTOCOL_VERSION;
  out[19] = OUR_CAPABILITIES;

  uint8_t state = 0;
  if (walking) state |= STATE_WALKING;
  if (journey_active) state |= STATE_JOURNEY;
  out[20] = state;
  write_le64(&out[21], pet_id_);
}

bool BleSense::parse_trimagotchi_packet_(const uint8_t *data,
                                         uint8_t len,
                                         TrimPeerSighting &peer) const {
  size_t pos = 0;
  while (pos + 2 <= len) {
    const uint8_t field_len = data[pos];
    if (field_len == 0) break;

    const size_t total = static_cast<size_t>(field_len) + 1u;
    if (pos + total > len || field_len < 1) break;

    const uint8_t type = data[pos + 1];
    if (type == AD_TYPE_SERVICE_DATA_UUID128) {
      const size_t payload_len = static_cast<size_t>(field_len) - 1u;
      const uint8_t *payload = &data[pos + 2];

      // UUID16 + protocol1 + capabilities1 + state1 + PetID8 = 27 bytes.
      if (payload_len >= 27 &&
          std::memcmp(payload, TRIM_SERVICE_UUID, 16) == 0) {
        peer.protocol_version = payload[16];
        peer.capabilities = payload[17];
        peer.state_flags = payload[18];
        peer.pet_id = read_le64(&payload[19]);
        return peer.protocol_version >= 1 && peer.pet_id != 0;
      }
    }
    pos += total;
  }
  return false;
}

void BleSense::handle_advertisement_(const struct ble_gap_disc_desc &disc) {
  if (!active_result_) return;

  BleScanResult &result = *active_result_;
  ++result.visible_advertisers;
  if (disc.rssi > result.strongest_rssi) result.strongest_rssi = disc.rssi;

  TrimPeerSighting peer;
  if (parse_trimagotchi_packet_(disc.data, disc.length_data, peer) &&
      peer.pet_id != pet_id_) {
    peer.rssi = disc.rssi;
    auto it = std::find_if(result.trimagotchi_peers.begin(),
                           result.trimagotchi_peers.end(),
                           [&](const TrimPeerSighting &p) {
                             return p.pet_id == peer.pet_id;
                           });
    if (it == result.trimagotchi_peers.end()) {
      result.trimagotchi_peers.push_back(peer);
    } else if (peer.rssi > it->rssi) {
      *it = peer;
    }
  }

  if (address_is_stable_(disc.addr.val, disc.addr.type)) {
    BleObservation obs;
    obs.anonymous_id = anonymise_stable_address_(disc.addr.val, disc.addr.type);
    obs.rssi = disc.rssi;

    auto it = std::find_if(result.stable_observations.begin(),
                           result.stable_observations.end(),
                           [&](const BleObservation &o) {
                             return o.anonymous_id == obs.anonymous_id;
                           });
    if (it == result.stable_observations.end()) {
      result.stable_observations.push_back(obs);
    } else if (obs.rssi > it->rssi) {
      it->rssi = obs.rssi;
    }
  }
}

int BleSense::gap_event_(struct ble_gap_event *event, void *arg) {
  auto *self = static_cast<BleSense *>(arg);
  return self ? self->handle_gap_event_(event) : 0;
}

int BleSense::handle_gap_event_(struct ble_gap_event *event) {
  switch (event->type) {
    case BLE_GAP_EVENT_DISC:
      handle_advertisement_(event->disc);
      return 0;
    case BLE_GAP_EVENT_DISC_COMPLETE:
      if (scan_sem_) {
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(scan_sem_));
      }
      return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
      return 0;
    default:
      return 0;
  }
}

esp_err_t BleSense::sense(BleScanResult &result,
                          bool walking,
                          bool journey_active,
                          uint32_t duration_ms) {
  result = BleScanResult{};
  if (!ready_) return ESP_ERR_INVALID_STATE;

  duration_ms = std::max<uint32_t>(500u, std::min<uint32_t>(duration_ms, 10000u));
  auto scan_sem = static_cast<SemaphoreHandle_t>(scan_sem_);
  while (xSemaphoreTake(scan_sem, 0) == pdTRUE) {}

  active_result_ = &result;

  uint8_t adv_data[29]{};
  build_identity_advertisement_(adv_data, walking, journey_active);
  int rc = ble_gap_adv_set_data(adv_data, sizeof(adv_data));
  if (rc != 0) {
    active_result_ = nullptr;
    ESP_LOGE(TAG, "Could not set identity advertisement: rc=%d", rc);
    return ESP_FAIL;
  }

  struct ble_gap_adv_params adv_params{};
  adv_params.conn_mode = BLE_GAP_CONN_MODE_NON;
  adv_params.disc_mode = BLE_GAP_DISC_MODE_NON;
  adv_params.itvl_min = 320;  // 200ms, 0.625ms units
  adv_params.itvl_max = 320;

  rc = ble_gap_adv_start(own_addr_type_, nullptr, BLE_HS_FOREVER,
                         &adv_params, gap_event_, this);
  if (rc != 0) {
    active_result_ = nullptr;
    ESP_LOGE(TAG, "Could not start identity beacon: rc=%d", rc);
    return ESP_FAIL;
  }

  struct ble_gap_disc_params disc_params{};
  disc_params.filter_duplicates = 1;
  disc_params.passive = 1;
  disc_params.itvl = 0;
  disc_params.window = 0;
  disc_params.filter_policy = 0;
  disc_params.limited = 0;

  ESP_LOGI(TAG, "BLE eyes open for %lu ms; Pet ID beacon active",
           static_cast<unsigned long>(duration_ms));

  rc = ble_gap_disc(own_addr_type_, static_cast<int32_t>(duration_ms),
                    &disc_params, gap_event_, this);
  if (rc != 0) {
    (void)ble_gap_adv_stop();
    active_result_ = nullptr;
    ESP_LOGE(TAG, "Could not start passive BLE scan: rc=%d", rc);
    return ESP_FAIL;
  }

  if (xSemaphoreTake(scan_sem, pdMS_TO_TICKS(duration_ms + 1500u)) != pdTRUE) {
    ESP_LOGW(TAG, "BLE scan completion timeout; cancelling");
    (void)ble_gap_disc_cancel();
  }

  // No BLE GAP activity is left running into Trimagotchi's SNOOZE window.
  (void)ble_gap_adv_stop();
  active_result_ = nullptr;

  ESP_LOGI(TAG, "BLE scan complete: visible=%lu stable=%u peers=%u strongest=%d dBm",
           static_cast<unsigned long>(result.visible_advertisers),
           static_cast<unsigned>(result.stable_observations.size()),
           static_cast<unsigned>(result.trimagotchi_peers.size()),
           static_cast<int>(result.strongest_rssi));
  return ESP_OK;
}
