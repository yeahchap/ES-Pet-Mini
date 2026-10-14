#include "patch.h"

#include <array>
#include <cstring>

#include "esp_log.h"
#include "esp_partition.h"

namespace {

constexpr const char *TAG = "trim_patch";

constexpr char PATCH_MAGIC[8] = {
    'E', 'S', 'P', 'A', 'T', 'C', 'H', '\0'
};

constexpr uint16_t PATCH_FORMAT_VERSION = 1;
// RC7 keeps accepting the existing base-6 micro-patch format so an app-only
// update does not turn a preserved RC6 patch slot into an error. The current
// loader validates metadata only; no patch payload changes runtime behaviour.
constexpr uint32_t PATCH_COMPAT_BASE_RC6 = 6;

#pragma pack(push, 1)
struct PatchHeader {
  char magic[8];
  uint16_t format_version;
  uint16_t header_size;
  uint32_t base_version;
  uint32_t sequence;
  uint32_t payload_size;
  uint32_t payload_crc32;
};
#pragma pack(pop)

PatchInfo g_info;

uint32_t crc32(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;

  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];

    for (int bit = 0; bit < 8; ++bit) {
      const uint32_t mask =
          static_cast<uint32_t>(-(static_cast<int32_t>(crc & 1u)));
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }

  return ~crc;
}

}  // namespace

esp_err_t patch_begin() {
  g_info = PatchInfo{};

  const esp_partition_t *partition =
      esp_partition_find_first(
          ESP_PARTITION_TYPE_ANY,
          ESP_PARTITION_SUBTYPE_ANY,
          "patch");

  if (!partition) {
    ESP_LOGW(TAG, "Patch partition not found; using baked-in RC7 defaults");
    return ESP_OK;
  }

  ESP_LOGI(TAG,
           "Patch slot: offset=0x%06lX size=0x%04lX",
           static_cast<unsigned long>(partition->address),
           static_cast<unsigned long>(partition->size));

  PatchHeader header{};

  esp_err_t err =
      esp_partition_read(partition, 0, &header, sizeof(header));

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Could not read patch header: %s",
             esp_err_to_name(err));
    return ESP_OK;
  }

  // Blank flash = no patch.
  bool blank = true;
  const uint8_t *header_bytes =
      reinterpret_cast<const uint8_t *>(&header);

  for (size_t i = 0; i < sizeof(header); ++i) {
    if (header_bytes[i] != 0xFF) {
      blank = false;
      break;
    }
  }

  if (blank) {
    ESP_LOGI(TAG, "No patch installed; using baked-in RC7 defaults");
    return ESP_OK;
  }

  if (std::memcmp(header.magic, PATCH_MAGIC,
                  sizeof(PATCH_MAGIC)) != 0) {
    ESP_LOGW(TAG, "Patch magic invalid; ignoring patch");
    return ESP_OK;
  }

  if (header.format_version != PATCH_FORMAT_VERSION) {
    ESP_LOGW(TAG,
             "Unsupported patch format %u; ignoring patch",
             static_cast<unsigned>(header.format_version));
    return ESP_OK;
  }

  if (header.base_version != PATCH_COMPAT_BASE_RC6) {
    ESP_LOGW(TAG,
             "Patch requires base %lu; RC7 accepts preserved base-6 patches only; ignoring patch",
             static_cast<unsigned long>(header.base_version));
    return ESP_OK;
  }

  if (header.header_size != sizeof(PatchHeader) ||
      header.payload_size >
          partition->size - sizeof(PatchHeader)) {
    ESP_LOGW(TAG, "Patch size/header invalid; ignoring patch");
    return ESP_OK;
  }

  std::array<uint8_t, 8192> buffer{};

  if (header.payload_size > buffer.size()) {
    ESP_LOGW(TAG, "Patch payload too large; ignoring patch");
    return ESP_OK;
  }

  err = esp_partition_read(
      partition,
      sizeof(PatchHeader),
      buffer.data(),
      header.payload_size);

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Could not read patch payload: %s",
             esp_err_to_name(err));
    return ESP_OK;
  }

  const uint32_t actual_crc =
      crc32(buffer.data(), header.payload_size);

  if (actual_crc != header.payload_crc32) {
    ESP_LOGW(TAG,
             "Patch CRC mismatch; ignoring patch");
    return ESP_OK;
  }

  g_info.active = true;
  g_info.sequence = header.sequence;
  g_info.payload_size = header.payload_size;

  ESP_LOGI(TAG,
           "RC7 COMPAT PATCH PRESENT: sequence=%lu payload=%lu bytes",
           static_cast<unsigned long>(g_info.sequence),
           static_cast<unsigned long>(g_info.payload_size));

  return ESP_OK;
}

const PatchInfo &patch_info() {
  return g_info;
}
