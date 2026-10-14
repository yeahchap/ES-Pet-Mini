#include "nvs.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;
using Namespace = std::unordered_map<std::string, Bytes>;

std::unordered_map<std::string, Namespace> g_store;
std::unordered_map<nvs_handle_t, std::string> g_handles;
nvs_handle_t g_next_handle = 1;

Namespace *namespace_for(nvs_handle_t handle) {
  const auto found = g_handles.find(handle);
  if (found == g_handles.end()) return nullptr;
  return &g_store[found->second];
}

template <typename T>
esp_err_t get_scalar(nvs_handle_t handle, const char *key, T *value) {
  Namespace *space = namespace_for(handle);
  if (!space || !key || !value) return ESP_ERR_INVALID_ARG;
  const auto found = space->find(key);
  if (found == space->end()) return ESP_ERR_NVS_NOT_FOUND;
  if (found->second.size() != sizeof(T)) return ESP_FAIL;
  std::memcpy(value, found->second.data(), sizeof(T));
  return ESP_OK;
}

template <typename T>
esp_err_t set_scalar(nvs_handle_t handle, const char *key, T value) {
  Namespace *space = namespace_for(handle);
  if (!space || !key) return ESP_ERR_INVALID_ARG;
  Bytes bytes(sizeof(T));
  std::memcpy(bytes.data(), &value, sizeof(T));
  (*space)[key] = std::move(bytes);
  return ESP_OK;
}

}  // namespace

void fake_nvs_reset() {
  g_store.clear();
  g_handles.clear();
  g_next_handle = 1;
}

esp_err_t nvs_open(const char *name,
                   nvs_open_mode_t mode,
                   nvs_handle_t *handle) {
  if (!name || !handle) return ESP_ERR_INVALID_ARG;
  const auto found = g_store.find(name);
  if (found == g_store.end()) {
    if (mode == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
    g_store.emplace(name, Namespace{});
  }
  *handle = g_next_handle++;
  g_handles[*handle] = name;
  return ESP_OK;
}

void nvs_close(nvs_handle_t handle) {
  g_handles.erase(handle);
}

esp_err_t nvs_commit(nvs_handle_t handle) {
  return namespace_for(handle) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t nvs_get_u8(nvs_handle_t handle,
                     const char *key,
                     uint8_t *value) {
  return get_scalar(handle, key, value);
}

esp_err_t nvs_get_u32(nvs_handle_t handle,
                      const char *key,
                      uint32_t *value) {
  return get_scalar(handle, key, value);
}

esp_err_t nvs_get_blob(nvs_handle_t handle,
                       const char *key,
                       void *value,
                       size_t *length) {
  Namespace *space = namespace_for(handle);
  if (!space || !key || !length) return ESP_ERR_INVALID_ARG;
  const auto found = space->find(key);
  if (found == space->end()) return ESP_ERR_NVS_NOT_FOUND;
  if (!value || *length < found->second.size()) return ESP_FAIL;
  std::memcpy(value, found->second.data(), found->second.size());
  *length = found->second.size();
  return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t handle,
                     const char *key,
                     uint8_t value) {
  return set_scalar(handle, key, value);
}

esp_err_t nvs_set_u32(nvs_handle_t handle,
                      const char *key,
                      uint32_t value) {
  return set_scalar(handle, key, value);
}

esp_err_t nvs_set_blob(nvs_handle_t handle,
                       const char *key,
                       const void *value,
                       size_t length) {
  Namespace *space = namespace_for(handle);
  if (!space || !key || (!value && length > 0)) return ESP_ERR_INVALID_ARG;
  const auto *first = static_cast<const uint8_t *>(value);
  (*space)[key] = Bytes(first, first + length);
  return ESP_OK;
}
