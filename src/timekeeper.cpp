#include "timekeeper.h"

#include "clock_sync_policy.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "wifi_sense.h"

static const char *TAG = "trim_clock";

namespace {
constexpr const char *NAMESPACE_NAME = "trimclock";
constexpr const char *SETTINGS_KEY = "settings";
constexpr const char *SETUP_PASSWORD = "trimagotchi";
constexpr uint32_t SETTINGS_MAGIC = 0x544D5354;
constexpr uint16_t SETTINGS_VERSION = 2;

TimeKeeper *g_owner = nullptr;
SemaphoreHandle_t g_saved_sem = nullptr;
std::atomic<bool> g_saved{false};

char from_hex(char c) {
  if (c >= '0' && c <= '9') return static_cast<char>(c - '0');
  if (c >= 'a' && c <= 'f') return static_cast<char>(10 + c - 'a');
  if (c >= 'A' && c <= 'F') return static_cast<char>(10 + c - 'A');
  return 0;
}

std::string url_decode(const char *s, size_t len) {
  std::string out;
  out.reserve(len);

  for (size_t i = 0; i < len; ++i) {
    const char c = s[i];
    if (c == '+') {
      out.push_back(' ');
    } else if (c == '%' && i + 2 < len) {
      const char hi = from_hex(s[i + 1]);
      const char lo = from_hex(s[i + 2]);
      out.push_back(static_cast<char>((hi << 4) | lo));
      i += 2;
    } else {
      out.push_back(c);
    }
  }

  return out;
}

bool form_value(const std::string &body,
                const char *name,
                std::string &value) {
  const std::string needle = std::string(name) + "=";
  size_t p = body.find(needle);
  if (p == std::string::npos) return false;
  p += needle.size();
  size_t e = body.find('&', p);
  if (e == std::string::npos) e = body.size();
  value = url_decode(body.data() + p, e - p);
  return true;
}

const char PAGE[] = R"HTML(<!doctype html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Trimagotchi First Setup</title>
<style>
body{font-family:monospace;max-width:650px;margin:32px auto;padding:0 18px}
.box{border:2px solid #222;padding:18px}
input,select,button{box-sizing:border-box;width:100%;font:inherit;padding:.7rem;margin:.35rem 0 .8rem}
button{font-weight:bold}.small{opacity:.72}.hint{padding:.6rem;background:#f3f3f3;margin:.5rem 0 1rem}
</style>
</head>
<body><div class="box">
<h2>TRIMAGOTCHI FIRST SETUP</h2>
<p>This information stays on this Trimagotchi. HOME Wi-Fi is only joined briefly when the clock needs synchronising.</p>

<form method="post" action="/save">
<label>Pet name (optional)</label>
<input name="petname" maxlength="16" placeholder="TRIMAGOTCHI">

<label>HOME Wi-Fi name (SSID)</label>
<input name="ssid" maxlength="32" required>

<label>HOME Wi-Fi password</label>
<input name="password" type="password" maxlength="64">

<label>Timezone</label>
<select name="tz" required>
  <option value="">-- choose local timezone --</option>
  <option value="UTC0|UTC">UTC</option>
  <option value="GMT0BST,M3.5.0/1,M10.5.0/2|Europe/London">United Kingdom / Ireland</option>
  <option value="CET-1CEST,M3.5.0/2,M10.5.0/3|Europe/Central">Central Europe</option>
  <option value="EET-2EEST,M3.5.0/3,M10.5.0/4|Europe/Eastern">Eastern Europe</option>
  <option value="EST5EDT,M3.2.0/2,M11.1.0/2|America/Eastern">North America - Eastern</option>
  <option value="CST6CDT,M3.2.0/2,M11.1.0/2|America/Central">North America - Central</option>
  <option value="MST7MDT,M3.2.0/2,M11.1.0/2|America/Mountain">North America - Mountain</option>
  <option value="PST8PDT,M3.2.0/2,M11.1.0/2|America/Pacific">North America - Pacific</option>
  <option value="IST-5:30|Asia/Kolkata">India</option>
  <option value="JST-9|Asia/Tokyo">Japan</option>
  <option value="AEST-10AEDT,M10.1.0/2,M4.1.0/3|Australia/Sydney">Australia - Sydney/Melbourne</option>
  <option value="NZST-12NZDT,M9.5.0/2,M4.1.0/3|Pacific/Auckland">New Zealand</option>
  <option value="CUSTOM|Custom POSIX TZ">Custom POSIX TZ rule</option>
</select>

<div id="custombox" style="display:none">
<label>Custom POSIX TZ rule</label>
<input name="customtz" maxlength="64" placeholder="e.g. UTC0">
</div>

<div class="hint" id="browserzone">Browser timezone: detecting...</div>

<button type="submit">SAVE &amp; BEGIN</button>
</form>

<p class="small">
Wi-Fi credentials, timezone and pet name are stored only in this unit's NVS.
They are never included in the BLE social beacon.
</p>
</div>
<script>
const select=document.querySelector('select[name="tz"]');
const custom=document.getElementById('custombox');
select.addEventListener('change',()=>{custom.style.display=select.value.startsWith('CUSTOM')?'block':'none';});
try{
  document.getElementById('browserzone').textContent='Browser timezone: '+Intl.DateTimeFormat().resolvedOptions().timeZone;
}catch(e){
  document.getElementById('browserzone').textContent='Browser timezone unavailable';
}
</script>
</body></html>)HTML";

esp_err_t root_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

esp_err_t save_handler(httpd_req_t *req) {
  if (!g_owner || req->content_len <= 0 || req->content_len > 768) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form");
    return ESP_FAIL;
  }

  std::string body;
  body.resize(static_cast<size_t>(req->content_len));

  int got = 0;
  while (got < req->content_len) {
    int n = httpd_req_recv(req, body.data() + got, req->content_len - got);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (n <= 0) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "Form receive failed");
      return ESP_FAIL;
    }
    got += n;
  }

  std::string ssid;
  std::string password;
  std::string pet_name;
  std::string tz_choice;
  std::string custom_tz;

  if (!form_value(body, "ssid", ssid) || ssid.empty() || ssid.size() > 32 ||
      !form_value(body, "password", password) || password.size() > 64 ||
      !form_value(body, "petname", pet_name) || pet_name.size() > 16 ||
      !form_value(body, "tz", tz_choice) || tz_choice.empty() ||
      !form_value(body, "customtz", custom_tz) || custom_tz.size() > 64) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Setup values invalid");
    return ESP_FAIL;
  }

  std::string posix_tz;
  std::string timezone_label;

  if (tz_choice == "CUSTOM") {
    if (custom_tz.empty()) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                          "Custom timezone rule is empty");
      return ESP_FAIL;
    }
    posix_tz = custom_tz;
    timezone_label = "Custom";
  } else {
    const size_t split = tz_choice.find('|');
    if (split == std::string::npos || split == 0 ||
        split + 1 >= tz_choice.size()) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                          "Timezone choice invalid");
      return ESP_FAIL;
    }
    posix_tz = tz_choice.substr(0, split);
    timezone_label = tz_choice.substr(split + 1);
  }

  esp_err_t err = g_owner->save_provisioned_settings(
      ssid.c_str(),
      password.c_str(),
      pet_name.c_str(),
      timezone_label.c_str(),
      posix_tz.c_str());
  if (err != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Could not save credentials");
    return err;
  }

  httpd_resp_set_type(req, "text/html");
  httpd_resp_sendstr(req,
      "<html><body><h2>Saved.</h2><p>Trimagotchi now knows HOME, its local timezone and its name. You can return to your normal Wi-Fi.</p></body></html>");

  g_saved.store(true);
  if (g_saved_sem) xSemaphoreGive(g_saved_sem);
  return ESP_OK;
}

httpd_handle_t start_server() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;
  cfg.stack_size = 6144;
  cfg.max_uri_handlers = 3;

  httpd_handle_t server = nullptr;
  if (httpd_start(&server, &cfg) != ESP_OK) return nullptr;

  httpd_uri_t root{};
  root.uri = "/";
  root.method = HTTP_GET;
  root.handler = root_handler;
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));

  httpd_uri_t save{};
  save.uri = "/save";
  save.method = HTTP_POST;
  save.handler = save_handler;
  ESP_ERROR_CHECK(httpd_register_uri_handler(server, &save));

  return server;
}

}  // namespace

esp_err_t TimeKeeper::begin() {
  // Before provisioning, UTC is the neutral/safe default.
  setenv("TZ", "UTC0", 1);
  tzset();

  make_setup_name_(setup_ap_name_, sizeof(setup_ap_name_));

  esp_err_t err = load_settings_();
  if (err == ESP_OK && configured_) {
    apply_timezone_();
  }
  return err;
}

bool TimeKeeper::valid() const {
  const time_t now = time(nullptr);
  return now > static_cast<time_t>(1700000000);  // safely after 2023
}

int64_t TimeKeeper::unix_time() const {
  if (!valid()) return 0;
  return static_cast<int64_t>(time(nullptr));
}

uint32_t TimeKeeper::local_date_key() const {
  if (!valid()) return 0;
  const time_t now = time(nullptr);
  struct tm local{};
  localtime_r(&now, &local);
  return static_cast<uint32_t>((local.tm_year + 1900) * 10000 +
                               (local.tm_mon + 1) * 100 +
                               local.tm_mday);
}

uint8_t TimeKeeper::local_hour() const {
  if (!valid()) return 0;
  const time_t now = time(nullptr);
  struct tm local{};
  localtime_r(&now, &local);
  return static_cast<uint8_t>(local.tm_hour);
}

void TimeKeeper::format_local_date(char *out, size_t len) const {
  if (!out || len == 0) return;
  if (!valid()) {
    std::snprintf(out, len, "CLOCK NOT SYNCED");
    return;
  }

  static const char *DAYS[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  static const char *MONS[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

  const time_t now = time(nullptr);
  struct tm local{};
  localtime_r(&now, &local);
  std::snprintf(out, len, "%s %02d %s",
                DAYS[local.tm_wday], local.tm_mday, MONS[local.tm_mon]);
}

void TimeKeeper::format_local_datetime(char *out, size_t len) const {
  if (!out || len == 0) return;
  if (!valid()) {
    std::snprintf(out, len, "CLOCK NOT SYNCED");
    return;
  }

  static const char *MONS[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  const time_t now = time(nullptr);
  struct tm local{};
  localtime_r(&now, &local);
  std::snprintf(out, len, "%02d %s %02d:%02d",
                local.tm_mday, MONS[local.tm_mon], local.tm_hour, local.tm_min);
}

void TimeKeeper::make_setup_name_(char *out, size_t len) {
  uint8_t mac[6]{};
  if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
    std::snprintf(out, len, "Trimagotchi-Setup");
    return;
  }
  std::snprintf(out, len, "Trimagotchi-Setup-%02X%02X", mac[4], mac[5]);
}

void TimeKeeper::sanitise_pet_name_(const char *input,
                                      char *out,
                                      size_t len) {
  if (!out || len == 0) return;
  out[0] = '\0';

  if (!input || !input[0]) {
    std::snprintf(out, len, "TRIMAGOTCHI");
    return;
  }

  size_t w = 0;
  bool previous_space = false;

  for (size_t i = 0; input[i] && w + 1 < len; ++i) {
    unsigned char ch = static_cast<unsigned char>(input[i]);

    if (ch >= 'a' && ch <= 'z') ch = static_cast<unsigned char>(ch - 'a' + 'A');

    const bool allowed =
        (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') ||
        ch == '-' || ch == ' ';

    if (!allowed) continue;

    if (ch == ' ') {
      if (w == 0 || previous_space) continue;
      previous_space = true;
    } else {
      previous_space = false;
    }

    out[w++] = static_cast<char>(ch);
  }

  while (w > 0 && out[w - 1] == ' ') --w;
  out[w] = '\0';

  if (w == 0) {
    std::snprintf(out, len, "TRIMAGOTCHI");
  }
}

void TimeKeeper::apply_timezone_() const {
  const char *tz =
      (settings_.posix_tz[0] != '\0') ? settings_.posix_tz : "UTC0";

  setenv("TZ", tz, 1);
  tzset();

  ESP_LOGI(TAG, "Local timezone: %s [%s]",
           settings_.timezone_label,
           tz);
}

esp_err_t TimeKeeper::load_settings_() {
  configured_ = false;
  settings_ = Settings{};

  nvs_handle_t h;
  esp_err_t err = nvs_open(NAMESPACE_NAME, NVS_READONLY, &h);
  if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
  if (err != ESP_OK) return err;

  size_t size = sizeof(settings_);
  err = nvs_get_blob(h, SETTINGS_KEY, &settings_, &size);
  nvs_close(h);

  if (err == ESP_ERR_NVS_NOT_FOUND) {
    settings_ = Settings{};
    return ESP_OK;
  }

  if (err != ESP_OK || size != sizeof(settings_) ||
      settings_.magic != SETTINGS_MAGIC ||
      settings_.version != SETTINGS_VERSION ||
      settings_.ssid[0] == '\0' ||
      settings_.posix_tz[0] == '\0') {
    ESP_LOGW(TAG, "First-run settings invalid/missing; setup required");
    settings_ = Settings{};
    return ESP_OK;
  }

  configured_ = true;
  ESP_LOGI(TAG, "Setup loaded: HOME='%s' PET='%s' TZ='%s'",
           settings_.ssid,
           settings_.pet_name,
           settings_.timezone_label);
  return ESP_OK;
}

esp_err_t TimeKeeper::save_settings_(const char *ssid,
                                     const char *password,
                                     const char *pet_name,
                                     const char *timezone_label,
                                     const char *posix_tz) {
  if (!ssid || !ssid[0] || !password || !timezone_label ||
      !timezone_label[0] || !posix_tz || !posix_tz[0]) {
    return ESP_ERR_INVALID_ARG;
  }

  if (std::strlen(ssid) > 32 ||
      std::strlen(password) > 64 ||
      std::strlen(timezone_label) > 32 ||
      std::strlen(posix_tz) > 64) {
    return ESP_ERR_INVALID_ARG;
  }

  Settings s{};
  std::strncpy(s.ssid, ssid, sizeof(s.ssid) - 1);
  std::strncpy(s.password, password, sizeof(s.password) - 1);
  std::strncpy(s.timezone_label, timezone_label,
               sizeof(s.timezone_label) - 1);
  std::strncpy(s.posix_tz, posix_tz, sizeof(s.posix_tz) - 1);
  sanitise_pet_name_(pet_name, s.pet_name, sizeof(s.pet_name));

  nvs_handle_t h;
  esp_err_t err = nvs_open(NAMESPACE_NAME, NVS_READWRITE, &h);
  if (err != ESP_OK) return err;

  err = nvs_set_blob(h, SETTINGS_KEY, &s, sizeof(s));
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);

  if (err == ESP_OK) {
    settings_ = s;
    configured_ = true;
    apply_timezone_();

    ESP_LOGI(TAG, "First-run setup saved: HOME='%s' PET='%s' TZ='%s'",
             settings_.ssid,
             settings_.pet_name,
             settings_.timezone_label);
  }

  return err;
}

esp_err_t TimeKeeper::save_provisioned_settings(const char *ssid,
                                                const char *password,
                                                const char *pet_name,
                                                const char *timezone_label,
                                                const char *posix_tz) {
  return save_settings_(ssid, password, pet_name, timezone_label, posix_tz);
}

esp_err_t TimeKeeper::run_setup_portal(WifiSense &wifi,
                                       uint32_t timeout_ms,
                                       bool &saved) {
  saved = false;
  if (configured_) return ESP_OK;

  g_owner = this;
  g_saved.store(false);
  g_saved_sem = xSemaphoreCreateBinary();
  if (!g_saved_sem) return ESP_ERR_NO_MEM;

  esp_err_t err = wifi.start_setup_ap(setup_ap_name_, SETUP_PASSWORD);
  if (err != ESP_OK) {
    vSemaphoreDelete(g_saved_sem);
    g_saved_sem = nullptr;
    g_owner = nullptr;
    return err;
  }

  httpd_handle_t server = start_server();
  if (!server) {
    (void)wifi.stop_setup_ap();
    vSemaphoreDelete(g_saved_sem);
    g_saved_sem = nullptr;
    g_owner = nullptr;
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "FIRST SETUP: AP '%s' password '%s'", setup_ap_name_, SETUP_PASSWORD);
  ESP_LOGI(TAG, "FIRST SETUP: open http://192.168.4.1");

  xSemaphoreTake(g_saved_sem, pdMS_TO_TICKS(timeout_ms));
  saved = g_saved.load();

  if (saved) vTaskDelay(pdMS_TO_TICKS(1200));

  httpd_stop(server);
  err = wifi.stop_setup_ap();

  vSemaphoreDelete(g_saved_sem);
  g_saved_sem = nullptr;
  g_owner = nullptr;

  ESP_LOGI(TAG, "FIRST SETUP closed: %s", saved ? "settings saved" : "timed out");
  return err;
}

bool TimeKeeper::should_sync(bool home_confident, bool saved_home_visible) const {
  return clock_sync::due(configured_, home_confident, saved_home_visible,
                         valid(), esp_timer_get_time(),
                         last_sync_monotonic_us_, last_attempt_monotonic_us_);
}

esp_err_t TimeKeeper::sync_at_home(WifiSense &wifi) {
  if (!configured_) return ESP_ERR_INVALID_STATE;

  last_attempt_monotonic_us_ = esp_timer_get_time();
  ESP_LOGI(TAG, "HOME clock sync: briefly connecting to saved Wi-Fi for SNTP");

  esp_err_t err = wifi.connect_for_internet(
      settings_.ssid, settings_.password, 12000);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Clock Wi-Fi connection failed: %s", esp_err_to_name(err));
    wifi.disconnect_from_internet();
    return err;
  }

  esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
  err = esp_netif_sntp_init(&config);
  if (err == ESP_OK) {
    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000));
    esp_netif_sntp_deinit();
  }

  wifi.disconnect_from_internet();

  if (err != ESP_OK || !valid()) {
    ESP_LOGW(TAG, "SNTP did not produce a valid clock: %s", esp_err_to_name(err));
    return err == ESP_OK ? ESP_ERR_TIMEOUT : err;
  }

  last_sync_monotonic_us_ = esp_timer_get_time();

  char when[40];
  format_local_datetime(when, sizeof(when));
  ESP_LOGI(TAG, "CLOCK SYNCED: %s (%s)", when, settings_.timezone_label);
  return ESP_OK;
}
