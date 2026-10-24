#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_partition.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#if defined(ES_PET_ULTIMATE_XIAO)
#include "driver/ledc.h"
#endif
#include "esp_timer.h"
#include "esp_system.h"
#include <ctime>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "epaper.h"
#include "encounter_memory.h"
#include "ble_sense.h"
#include "explorer.h"
#include "gfx.h"
#include "pet_art.h"
#include "ui_font.h"
#include "ui_icons.h"
#include "memory.h"
#include "imu.h"
#include "onboard_led.h"
#include "reward.h"
#include "patch.h"
#include "wifi_sense.h"
#include "timekeeper.h"
#include "mission.h"
#include "achievements.h"
#include "factory_reset.h"
#include "battery.h"
#include "economy.h"
#include "economy_rules.h"

#ifndef ES_PET_ECONOMY_TRACE
#define ES_PET_ECONOMY_TRACE 0
#endif

static const char *TAG = "trimagotchi";
#if defined(ES_PET_ULTIMATE_XIAO)
static bool g_ultimate_tap_chirped = false;
static ImuSense *g_ultimate_imu = nullptr;

class TapFeedbackGuard {
 public:
  TapFeedbackGuard() : imu_(g_ultimate_imu) {
    if (imu_) imu_->begin_feedback();
  }
  ~TapFeedbackGuard() { if (imu_) imu_->finish_feedback(); }
 private:
  ImuSense *imu_;
};
#endif

#if defined(ES_PET_ULTIMATE_XIAO)
// Direct-drive PASSIVE ceramic piezo + 120 ohm series resistor on D1/GPIO1.
// An active buzzer, 4-ohm speaker or magnetic transducer MUST NOT be used here.
static constexpr gpio_num_t PIEZO_GPIO = GPIO_NUM_1;
static bool haptic_ready = false;
static esp_err_t haptic_init() {
  ledc_timer_config_t timer{};
  timer.speed_mode = LEDC_LOW_SPEED_MODE;
  timer.duty_resolution = LEDC_TIMER_10_BIT;
  timer.timer_num = LEDC_TIMER_0;
  timer.freq_hz = 3800;
  timer.clk_cfg = LEDC_AUTO_CLK;
  ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "piezo timer");
  ledc_channel_config_t ch{};
  ch.gpio_num = static_cast<int>(PIEZO_GPIO);
  ch.speed_mode = LEDC_LOW_SPEED_MODE;
  ch.channel = LEDC_CHANNEL_0;
  ch.timer_sel = LEDC_TIMER_0;
  ch.duty = 0;
  ESP_RETURN_ON_ERROR(ledc_channel_config(&ch), TAG, "piezo channel");
  haptic_ready = true;
  ESP_LOGI(TAG, "Passive piezo on XIAO D1/GPIO1 at 3800 Hz");
  return ESP_OK;
}
static void haptic_tap(const char *reason) {
  if (!haptic_ready) return;
  TapFeedbackGuard feedback_guard;
  ESP_LOGI(TAG, "PIEZO chirp: %s", reason ? reason : "event");
  if (ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 512) != ESP_OK)
    return;
  if (ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0) != ESP_OK)
    return;
  vTaskDelay(pdMS_TO_TICKS(100));
  (void)ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
  (void)ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
#else
// Final hardware: GPIO1 drives the AO3400A haptic motor gate.
// A 100 ms pulse keeps the tactile feedback punchy while using one-third
// less motor-on time than the 150 ms sacrificial-board proof test.
static constexpr gpio_num_t HAPTIC_GPIO = GPIO_NUM_1;
static constexpr uint32_t HAPTIC_PULSE_MS = 100;
static bool haptic_ready = false;

static esp_err_t haptic_init() {
  esp_err_t err = gpio_reset_pin(HAPTIC_GPIO);
  if (err != ESP_OK) return err;

  err = gpio_set_direction(HAPTIC_GPIO, GPIO_MODE_OUTPUT);
  if (err != ESP_OK) return err;

  err = gpio_set_level(HAPTIC_GPIO, 0);
  if (err != ESP_OK) return err;

  haptic_ready = true;
  ESP_LOGI(TAG, "Haptic ready: GPIO1, %lu ms pulse",
           static_cast<unsigned long>(HAPTIC_PULSE_MS));
  return ESP_OK;
}

static void haptic_tap(const char *reason) {
  if (!haptic_ready) return;

  if (reason && *reason) {
    ESP_LOGI(TAG, "HAPTIC: %s", reason);
  }

  if (gpio_set_level(HAPTIC_GPIO, 1) != ESP_OK) return;
  vTaskDelay(pdMS_TO_TICKS(HAPTIC_PULSE_MS));
  (void)gpio_set_level(HAPTIC_GPIO, 0);
}

#endif

static bool refresh_epaper_nonfatal(Epaper29Tri &epd, const char *reason) {
#if defined(ES_PET_ULTIMATE_XIAO)
  if (g_ultimate_imu) g_ultimate_imu->begin_display_refresh();
#endif
  const esp_err_t err = epd.refresh();
#if defined(ES_PET_ULTIMATE_XIAO)
  if (g_ultimate_imu) g_ultimate_imu->finish_display_refresh();
#endif
  if (err == ESP_OK) return true;

  ESP_LOGE(TAG,
           "DISPLAY update failed (%s): %s; ES-PET continues and will retry",
           reason ? reason : "unspecified", esp_err_to_name(err));
  return false;
}


static uint32_t popcount16(uint16_t v) {
  uint32_t n = 0;
  while (v) {
    n += (v & 1u);
    v >>= 1u;
  }
  return n;
}


static void compact_number(uint32_t value, char *out, size_t len) {
  if (!out || len == 0) return;

  if (value < 1000) {
    std::snprintf(out, len, "%lu",
                  static_cast<unsigned long>(value));
  } else if (value < 10000) {
    const uint32_t whole = value / 1000;
    const uint32_t tenth = (value % 1000) / 100;
    std::snprintf(out, len, "%lu.%luK",
                  static_cast<unsigned long>(whole),
                  static_cast<unsigned long>(tenth));
  } else if (value < 1000000) {
    std::snprintf(out, len, "%luK",
                  static_cast<unsigned long>(value / 1000));
  } else {
    const uint32_t whole = value / 1000000;
    const uint32_t tenth = (value % 1000000) / 100000;
    std::snprintf(out, len, "%lu.%luM",
                  static_cast<unsigned long>(whole),
                  static_cast<unsigned long>(tenth));
  }
}

static void mission_progress_text(const MissionSlot &slot,
                                  char *out,
                                  size_t len) {
  if (!out || len == 0) return;

  if ((slot.flags & 0x01u) != 0) {
    std::snprintf(out, len, "DONE");
    return;
  }

  std::snprintf(out, len, "%lu/%lu",
                static_cast<unsigned long>(slot.progress),
                static_cast<unsigned long>(slot.target));
}


enum class PrettyMood : uint8_t {
  Home,
  Curious,
  Walkies,
  Snoozing,
  Perched,
  Adventure,
  Discovery,
  Friend,
  Homecoming,
  Celebrate,
  MissionComplete,
  BadgeUnlock,
  LevelUp,
  FactoryReset,
};

static void dither_rect(TinyGfx &g, int x, int y, int w, int h,
                        InkColor color, uint8_t density = 1) {
  if (w <= 0 || h <= 0) return;
  if (density < 1) density = 1;
  for (int yy = 0; yy < h; ++yy) {
    for (int xx = 0; xx < w; ++xx) {
      const uint8_t p = static_cast<uint8_t>((xx + yy * 2) & 0x03u);
      if (p < density) g.pixel(x + xx, y + yy, color);
    }
  }
}

static void rpg_frame(TinyGfx &g, int x, int y, int w, int h,
                      bool accent = true) {
  // A sparse yellow shadow gives the white page a warm four-colour lift
  // without sacrificing the crisp black storybook frame.
  dither_rect(g, x + 3, y + 3, w, h, InkColor::Yellow, 1);
  g.fill_rect(x, y, w, h, InkColor::White);
  g.rect(x, y, w, h, InkColor::Black);
  if (w > 6 && h > 6) g.rect(x + 2, y + 2, w - 4, h - 4, InkColor::Black);

  if (accent && w > 18 && h > 8) {
    // Small pigment ticks make every panel feel intentionally four-colour.
    const int red_end = std::min(x + w - 4, x + 18);
    g.line(x + 5, y, red_end, y, InkColor::Red);

    const int yellow_start = std::max(x + 4, x + w - 19);
    g.line(yellow_start, y + h - 1, x + w - 6, y + h - 1,
           InkColor::Yellow);
  }
}

static void draw_sparkle(TinyGfx &g, int x, int y, InkColor color) {
  g.line(x, y - 3, x, y + 3, color);
  g.line(x - 3, y, x + 3, y, color);
  g.pixel(x - 2, y - 2, color);
  g.pixel(x + 2, y + 2, color);
}

static void draw_page_accents(TinyGfx &g) {
  // Tiny edge confetti is our "soft colour background": real pigment, but
  // sparse enough that the page still reads as clean white paper.
  for (int y = 27; y < 198; y += 13) {
    g.pixel(1, y, InkColor::Yellow);
    if (y + 1 < 200) g.pixel(2, y + 1, InkColor::Yellow);

    const int ry = y + 5;
    if (ry < 199) {
      g.pixel(198, ry, InkColor::Red);
      if (ry + 1 < 200) g.pixel(197, ry + 1, InkColor::Red);
    }
  }
}

static void centered_ui_text(TinyGfx &g, int y, const char *text,
                             UiFont font, InkColor color,
                             int left = 0, int right = 200) {
  if (!text) text = "";
  const int w = ui_text_width(text, font);
  const int x = left + std::max(0, (right - left - w) / 2);
  ui_text(g, x, y, text, font, color);
}

static std::string fit_text_width(const char *text, UiFont font, int max_w) {
  std::string s = text ? text : "";
  while (!s.empty() && ui_text_width(s.c_str(), font) > max_w) {
    s.pop_back();
  }
  return s;
}

static uint32_t displayed_today_steps(const LifeState &life,
                                      const DailyStats &day,
                                      const TimeKeeper &clock) {
  return clock.valid() ? day.steps : life.pending_unsynced_steps;
}

static void draw_header_battery(TinyGfx &g) {
  if (!battery_available()) return;

  const BatteryReading battery = battery_read();
  if (!battery.fitted) return;

  char text[24];
  const int whole_v = battery.millivolts / 1000;
  const int hundredths_v = (battery.millivolts % 1000) / 10;
  std::snprintf(text, sizeof(text), "BAT %d%% %d.%02dV",
                battery.percent, whole_v, hundredths_v);
  ui_text(g, 6, 10, text, UiFont::Tiny, InkColor::Yellow);
}

static void draw_pretty_header(TinyGfx &g, const TimeKeeper &clock) {
  constexpr int H = 20;
  draw_page_accents(g);
  g.fill_rect(0, 0, 200, H, InkColor::Black);
  g.line(0, 17, 199, 17, InkColor::Yellow);
  g.line(0, 18, 199, 18, InkColor::Red);

  const char *version = "V0.8L RC7";
  const int vw = ui_text_width(version, UiFont::Tiny);
  ui_text(g, 6, 1, "ES-PET", UiFont::SmallBold, InkColor::Yellow);
  ui_text(g, 194 - vw, 1, version, UiFont::Tiny, InkColor::Red);
  draw_header_battery(g);

  char status[24];
  if (clock.valid()) {
    char when[24];
    clock.format_local_datetime(when, sizeof(when));
    const char *time_part = strrchr(when, ' ');
    std::snprintf(status, sizeof(status), "%s",
                  time_part && *(time_part + 1) ? time_part + 1 : when);
  } else {
    std::snprintf(status, sizeof(status), "NO CLOCK");
  }
  const int sw = ui_text_width(status, UiFont::Tiny);
  ui_text(g, 194 - sw, 10, status, UiFont::Tiny, InkColor::White);
}

static void draw_static_header(TinyGfx &g, const char *status) {
  draw_page_accents(g);
  g.fill_rect(0, 0, 200, 20, InkColor::Black);
  g.line(0, 17, 199, 17, InkColor::Yellow);
  g.line(0, 18, 199, 18, InkColor::Red);
  ui_text(g, 6, 1, "ES-PET", UiFont::SmallBold, InkColor::Yellow);
  const char *version = "V0.8L RC7";
  const int vw = ui_text_width(version, UiFont::Tiny);
  ui_text(g, 194 - vw, 1, version, UiFont::Tiny, InkColor::Red);
  draw_header_battery(g);
  if (status && *status) {
    const int sw = ui_text_width(status, UiFont::Tiny);
    ui_text(g, 194 - sw, 10, status, UiFont::Tiny, InkColor::White);
  }
}

static PetArtMood pet_art_for(PrettyMood mood) {
  switch (mood) {
    case PrettyMood::Home: return PetArtMood::Home;
    case PrettyMood::Curious: return PetArtMood::Curious;
    case PrettyMood::Walkies: return PetArtMood::Walkies;
    case PrettyMood::Snoozing: return PetArtMood::Snoozing;
    case PrettyMood::Perched: return PetArtMood::Perched;
    case PrettyMood::Adventure: return PetArtMood::Adventure;
    case PrettyMood::Discovery: return PetArtMood::Discovery;
    case PrettyMood::Friend: return PetArtMood::Friend;
    case PrettyMood::Homecoming: return PetArtMood::Homecoming;
    case PrettyMood::Celebrate: return PetArtMood::Celebrate;
    case PrettyMood::MissionComplete: return PetArtMood::MissionComplete;
    case PrettyMood::BadgeUnlock: return PetArtMood::BadgeUnlock;
    case PrettyMood::LevelUp: return PetArtMood::LevelUp;
    case PrettyMood::FactoryReset: return PetArtMood::FactoryReset;
  }
  return PetArtMood::Home;
}

static void draw_pet_panel(TinyGfx &g, PrettyMood mood,
                           int x = 4, int y = 24) {
  // The double-line 118x86 panel has an exact 112x80 usable interior.
  // Storybook art fills it edge-to-edge: no white side gutters.
  rpg_frame(g, x, y, 118, 86, true);
  draw_pet_art(g, pet_art_for(mood), x + 3, y + 3);
}

static void draw_state_frame(TinyGfx &g, const char *title) {
  constexpr int X = 126, Y = 24, W = 70, H = 86;
  rpg_frame(g, X, Y, W, H, true);
  std::string t = fit_text_width(title ? title : "", UiFont::SmallBold, W - 10);
  centered_ui_text(g, Y + 5, t.c_str(), UiFont::SmallBold,
                   InkColor::Red, X + 4, X + W - 4);
  g.line(X + 8, Y + 20, X + W - 9, Y + 20, InkColor::Yellow);
  for (int px = X + 10; px < X + W - 10; px += 9) {
    g.pixel(px, Y + 20, InkColor::Red);
  }
}

static void draw_metric_row(TinyGfx &g, UiIcon icon,
                            const char *label, uint32_t value,
                            int y) {
  char v[16];
  compact_number(value, v, sizeof(v));
  draw_ui_icon(g, icon, 131, y + 1, InkColor::Red);
  std::string lab = fit_text_width(label ? label : "", UiFont::Small, 47);
  ui_text(g, 146, y, lab.c_str(), UiFont::Small, InkColor::Red);
  ui_text(g, 146, y + 9, v, UiFont::SmallBold, InkColor::Black);
}

static void draw_generic_state(TinyGfx &g, const char *title,
                               const LifeState &life,
                               const DailyStats &day,
                               const TimeKeeper &clock) {
  draw_state_frame(g, title);
  draw_metric_row(g, UiIcon::Steps, "STEPS",
                  displayed_today_steps(life, day, clock), 46);
  draw_metric_row(g, UiIcon::Wifi, "WIFI", day.wifi_unique, 67);
  draw_metric_row(g, UiIcon::Bluetooth, "BT", day.ble_unique, 88);
}

static void draw_walkies_state(TinyGfx &g,
                               const LifeState &life,
                               const DailyStats &day,
                               const TimeKeeper &clock) {
  draw_state_frame(g, "WALKIES!");
  const uint32_t steps = displayed_today_steps(life, day, clock);
  char b[16];
  compact_number(steps, b, sizeof(b));
  draw_ui_icon_large(g, UiIcon::Steps, 132, 49, InkColor::Red);
  centered_ui_text(g, 48, b, UiFont::Number, InkColor::Black, 150, 194);
  centered_ui_text(g, 76, "STEPS", UiFont::SmallBold, InkColor::Red, 130, 194);
  centered_ui_text(g, 90, "TODAY", UiFont::SmallBold, InkColor::Black, 130, 194);
  draw_sparkle(g, 188, 90, InkColor::Yellow);
}

static void draw_adventure_state(TinyGfx &g, const LifeState &life,
                                 const DailyStats &day,
                                 const TimeKeeper &clock) {
  draw_state_frame(g, "ADVENTURE");
  draw_metric_row(g, UiIcon::Xp, "TRIP XP", life.journey_xp, 46);
  draw_metric_row(g, UiIcon::Wifi, "NEW WIFI", life.journey_new_wifi, 67);
  draw_metric_row(g, UiIcon::Steps, "STEPS",
                  displayed_today_steps(life, day, clock), 88);
}

static void draw_social_state(TinyGfx &g, const char *title,
                              UiIcon icon1, const char *lab1, uint32_t v1,
                              UiIcon icon2, const char *lab2, uint32_t v2) {
  draw_state_frame(g, title);
  draw_metric_row(g, icon1, lab1, v1, 54);
  draw_metric_row(g, icon2, lab2, v2, 82);
}

static void draw_pretty_mission(TinyGfx &g, const MissionEngine &missions,
                                int x = 4, int y = 114) {
  const int featured = missions.featured_slot();
  const MissionSlot &slot =
      missions.slot(featured >= 0 ? static_cast<size_t>(featured) : 0);
  char progress[24];
  mission_progress_text(slot, progress, sizeof(progress));

  constexpr int W = 192, H = 47;
  rpg_frame(g, x, y, W, H, true);
  draw_ui_icon_large(g, UiIcon::Mission, x + 7, y + 4, InkColor::Red);
  ui_text(g, x + 29, y + 5, "MISSION", UiFont::SmallBold, InkColor::Red);

  if (missions.walk_streak() > 0) {
    char streak[20];
    std::snprintf(streak, sizeof(streak), "%uD WALK",
                  static_cast<unsigned>(missions.walk_streak()));
    const int sw = ui_text_width(streak, UiFont::SmallBold);
    ui_text(g, x + W - 7 - sw, y + 5, streak,
            UiFont::SmallBold, InkColor::Black);
  }

  g.line(x + 30, y + 20, x + W - 9, y + 20, InkColor::Yellow);
  for (int px = x + 33; px < x + W - 10; px += 11) {
    g.pixel(px, y + 20, InkColor::Red);
  }

  const char *name = MissionEngine::short_name(
      static_cast<MissionType>(slot.type));
  std::string fitted = fit_text_width(name, UiFont::SmallBold, W - 15);
  ui_text(g, x + 8, y + 22, fitted.c_str(), UiFont::SmallBold, InkColor::Black);

  const int pw = ui_text_width(progress, UiFont::SmallBold);
  ui_text(g, x + W - 8 - pw, y + 34, progress, UiFont::SmallBold,
          InkColor::Red);
  const int bw = std::max(18, W - 23 - pw);
  g.rect(x + 8, y + 36, bw, 7, InkColor::Black);
  g.fill_rect(x + 9, y + 37, bw - 2, 5, InkColor::Yellow);

  uint32_t filled = 0;
  if (slot.target > 0) {
    filled = static_cast<uint32_t>(
        (static_cast<uint64_t>(slot.progress) * static_cast<uint64_t>(bw - 2)) /
        static_cast<uint64_t>(slot.target));
  }
  if (filled > static_cast<uint32_t>(bw - 2)) filled = bw - 2;
  if (filled > 0) {
    g.fill_rect(x + 9, y + 37, static_cast<int>(filled), 5, InkColor::Red);
  }
}

static void draw_pretty_footer(TinyGfx &g,
                               const LifeState &life,
                               const TrimPeerBook &peer_book,
                               const AchievementEngine &badges,
                               const TimeKeeper &clock) {
  (void)clock;
  constexpr int Y = 165;
  g.line(4, Y, 195, Y, InkColor::Yellow);
  for (int x = 8; x < 194; x += 18) g.pixel(x, Y, InkColor::Red);

  struct Col {
    int x;
    UiIcon icon;
    const char *label;
    uint32_t value;
  };
  const Col cols[3] = {
      {4, UiIcon::Friends, "FRIENDS", peer_book.count},
      {68, UiIcon::Badge, "BADGES", badges.unlocked_count()},
      {132, UiIcon::Xp, "XP", life.xp},
  };

  char b[20];
  for (const Col &c : cols) {
    draw_ui_icon_large(g, c.icon, c.x + 3, Y + 5, InkColor::Red);
    ui_text(g, c.x + 23, Y + 4, c.label, UiFont::Small, InkColor::Red);
    compact_number(c.value, b, sizeof(b));
    ui_text(g, c.x + 23, Y + 16, b, UiFont::SmallBold, InkColor::Black);
  }
  for (int yy = Y + 4; yy < 198; yy += 4) {
    g.pixel(66, yy, InkColor::Yellow);
    g.pixel(130, yy, InkColor::Yellow);
  }
}

static void draw_learning_home(Epaper29Tri &epd,
                               const LifeState &life,
                               uint8_t learn_epoch,
                               uint32_t visible) {
  TinyGfx g(epd);
  epd.clear(InkColor::White);
  draw_static_header(g, "LEARNING HOME");
  rpg_frame(g, 41, 24, 118, 86, true);
  draw_pet_art(g, PetArtMood::Curious, 44, 27);
  g.fill_rect(4, 114, 192, 22, InkColor::Yellow);
  centered_ui_text(g, 118, "LEARNING", UiFont::Title, InkColor::Black, 7, 193);
  rpg_frame(g, 4, 140, 192, 56, true);
  char b[64];
  std::snprintf(b, sizeof(b), "SCAN %u / %u",
                static_cast<unsigned>(learn_epoch),
                static_cast<unsigned>(HOME_LEARN_EPOCHS));
  centered_ui_text(g, 145, "THIS FEELS LIKE HOME", UiFont::SmallBold,
                   InkColor::Black, 8, 192);
  centered_ui_text(g, 159, b, UiFont::SmallBold, InkColor::Black, 8, 192);
  std::snprintf(b, sizeof(b), "RADIOS %lu   STEPS %lu   XP %lu",
                static_cast<unsigned long>(visible),
                static_cast<unsigned long>(life.steps_total),
                static_cast<unsigned long>(life.xp));
  centered_ui_text(g, 176, b, UiFont::Small, InkColor::Black, 8, 192);
}

static void draw_clock_setup(Epaper29Tri &epd, const TimeKeeper &clock) {
  TinyGfx g(epd);
  epd.clear(InkColor::White);
  draw_static_header(g, "FIRST SETUP");
  rpg_frame(g, 41, 24, 118, 86, true);
  draw_pet_art(g, PetArtMood::Curious, 44, 27);
  g.fill_rect(4, 114, 192, 22, InkColor::Yellow);
  centered_ui_text(g, 118, "HELLO!", UiFont::Title, InkColor::Red, 7, 193);
  rpg_frame(g, 4, 140, 192, 56, true);
  centered_ui_text(g, 145, "HOME + TIME + NAME", UiFont::SmallBold,
                   InkColor::Black, 8, 192);
  centered_ui_text(g, 160, clock.setup_ap_name(), UiFont::SmallBold,
                   InkColor::Black, 8, 192);
  centered_ui_text(g, 175, "PASS: trimagotchi", UiFont::Small,
                   InkColor::Black, 8, 192);
  centered_ui_text(g, 187, "192.168.4.1", UiFont::Small,
                   InkColor::Black, 8, 192);
}

static void draw_naptime(Epaper29Tri &epd,
                         const LifeState &life,
                         const TrimPeerBook &peer_book,
                         const ExplorerUpdate &ex,
                         const DailyStats &day,
                         const TimeKeeper &clock,
                         const MissionEngine &missions,
                         const AchievementEngine &badges) {
  (void)ex;
  TinyGfx g(epd);
  epd.clear(InkColor::White);
  draw_pretty_header(g, clock);
  draw_pet_panel(g, PrettyMood::Snoozing);
  draw_generic_state(g, "SNOOZING", life, day, clock);
  draw_pretty_mission(g, missions);
  draw_pretty_footer(g, life, peer_book, badges, clock);
}

static void draw_perched(Epaper29Tri &epd,
                         const LifeState &life,
                         const TrimPeerBook &peer_book,
                         const DailyStats &day,
                         const TimeKeeper &clock,
                         const MissionEngine &missions,
                         const AchievementEngine &badges) {
  TinyGfx g(epd);
  epd.clear(InkColor::White);
  draw_pretty_header(g, clock);
  draw_pet_panel(g, PrettyMood::Perched);
  draw_generic_state(g, "PERCHED", life, day, clock);
  draw_pretty_mission(g, missions);
  draw_pretty_footer(g, life, peer_book, badges, clock);
}

static void draw_event_card(TinyGfx &g, const char *line1,
                            const char *line2, UiIcon icon,
                            const char *big_value, InkColor icon_color) {
  rpg_frame(g, 4, 140, 192, 56, true);
  if (line1 && *line1) {
    std::string l1 = fit_text_width(line1, UiFont::SmallBold, 178);
    centered_ui_text(g, 144, l1.c_str(), UiFont::SmallBold,
                     InkColor::Red, 8, 192);
  }
  if (line2 && *line2) {
    std::string l2 = fit_text_width(line2, UiFont::Small, 178);
    centered_ui_text(g, 157, l2.c_str(), UiFont::Small,
                     InkColor::Black, 8, 192);
  }
  const InkColor event_icon =
      icon_color == InkColor::Black ? InkColor::Red : icon_color;
  draw_ui_icon_large(g, icon, 18, 174, event_icon);
  if (big_value && *big_value) {
    std::string val = fit_text_width(big_value, UiFont::Title, 142);
    ui_text(g, 43, 172, val.c_str(), UiFont::Title, InkColor::Red);
  }
  g.line(12, 192, 187, 192, InkColor::Yellow);
  for (int x = 16; x < 188; x += 17) g.pixel(x, 192, InkColor::Red);
}

static void draw_event_shell(TinyGfx &g, PrettyMood mood, const char *title) {
  rpg_frame(g, 41, 24, 118, 86, true);
  draw_pet_art(g, pet_art_for(mood), 44, 27);
  g.fill_rect(4, 114, 192, 22, InkColor::Red);
  std::string fitted = fit_text_width(title ? title : "", UiFont::Title, 178);
  centered_ui_text(g, 118, fitted.c_str(), UiFont::Title,
                   InkColor::White, 8, 192);
}

static void draw_mission_complete(Epaper29Tri &epd,
                                  const TimeKeeper &clock,
                                  const MissionEngine &missions,
                                  MissionType mission,
                                  uint32_t reward_xp,
                                  uint32_t perfect_day_bonus_xp) {
  TinyGfx g(epd);
  epd.clear(InkColor::White);
  draw_pretty_header(g, clock);
  draw_event_shell(g, PrettyMood::MissionComplete, "MISSION DONE!");
  char reward[32], count[32];
  const uint32_t total_reward = reward_xp + perfect_day_bonus_xp;
  std::snprintf(reward, sizeof(reward), "+%lu XP",
                static_cast<unsigned long>(total_reward));
  if (perfect_day_bonus_xp > 0) {
    std::snprintf(count, sizeof(count), "ALL 3 BONUS +%lu",
                  static_cast<unsigned long>(perfect_day_bonus_xp));
  } else {
    std::snprintf(count, sizeof(count), "%u / 3 TODAY",
                  static_cast<unsigned>(missions.completed_count()));
  }
  draw_event_card(g, MissionEngine::name(mission), count,
                  UiIcon::Xp, reward, InkColor::Black);
}

static void draw_challenge_complete(Epaper29Tri &epd,
                                    const TimeKeeper &clock,
                                    ChallengeType challenge,
                                    uint32_t reward_xp) {
  TinyGfx g(epd);
  epd.clear(InkColor::White);
  draw_pretty_header(g, clock);
  draw_event_shell(g, PrettyMood::MissionComplete, "CHALLENGE!");
  char reward[32];
  std::snprintf(reward, sizeof(reward), "+%lu XP",
                static_cast<unsigned long>(reward_xp));
  draw_event_card(g, MissionEngine::challenge_name(challenge),
                  MissionEngine::challenge_detail(challenge),
                  UiIcon::Steps, reward, InkColor::Black);
}

static void draw_badge_unlocked(Epaper29Tri &epd,
                                const TimeKeeper &clock,
                                const AchievementEngine &badges,
                                AchievementType badge,
                                uint32_t reward_xp) {
  TinyGfx g(epd);
  epd.clear(InkColor::White);
  draw_pretty_header(g, clock);
  draw_event_shell(g, PrettyMood::BadgeUnlock, "BADGE!");
  char reward[32], count[32];
  std::snprintf(reward, sizeof(reward), "+%lu XP",
                static_cast<unsigned long>(reward_xp));
  std::snprintf(count, sizeof(count), "BADGES %u",
                static_cast<unsigned>(badges.unlocked_count()));
  draw_event_card(g, AchievementEngine::name(badge), count,
                  UiIcon::Badge, reward, InkColor::Black);
}

static void draw_level_up(Epaper29Tri &epd,
                          const LifeState &life,
                          const TimeKeeper &clock) {
  TinyGfx g(epd);
  epd.clear(InkColor::White);
  draw_pretty_header(g, clock);
  draw_event_shell(g, PrettyMood::LevelUp, "LEVEL UP!");
  char level[32], xp[32];
  std::snprintf(level, sizeof(level), "LEVEL %lu",
                static_cast<unsigned long>(life.level));
  std::snprintf(xp, sizeof(xp), "XP %lu",
                static_cast<unsigned long>(life.xp));
  draw_event_card(g, "GETTING CLEVERER!", xp,
                  UiIcon::Xp, level, InkColor::Black);
}

static void draw_factory_reset_prompt(Epaper29Tri &epd,
                                      const TimeKeeper &clock) {
  TinyGfx g(epd);
  epd.clear(InkColor::White);
  draw_pretty_header(g, clock);
  draw_event_shell(g, PrettyMood::FactoryReset, "START OVER?");
  rpg_frame(g, 4, 140, 192, 56, true);
  centered_ui_text(g, 145, "SECRET SERVICE MENU", UiFont::SmallBold,
                   InkColor::Red, 8, 192);
  centered_ui_text(g, 159, "HOLD BOOT 3 SEC", UiFont::SmallBold,
                   InkColor::Black, 8, 192);
  centered_ui_text(g, 174, "ERASES PET MEMORY", UiFont::Small,
                   InkColor::Red, 8, 192);
  centered_ui_text(g, 187, "RELEASE TO CANCEL", UiFont::Small,
                   InkColor::Black, 8, 192);
}

static void draw_factory_reset_goodbye(Epaper29Tri &epd,
                                       const TimeKeeper &clock) {
  TinyGfx g(epd);
  epd.clear(InkColor::White);
  draw_pretty_header(g, clock);
  draw_event_shell(g, PrettyMood::Homecoming, "GOODBYE!");
  rpg_frame(g, 4, 140, 192, 56, true);
  centered_ui_text(g, 148, "FORGETTING EVERYTHING", UiFont::SmallBold,
                   InkColor::Black, 8, 192);
  centered_ui_text(g, 165, "A FRESH PET IS NEXT", UiFont::SmallBold,
                   InkColor::Black, 8, 192);
  g.rect(18, 184, 164, 8, InkColor::Black);
  g.fill_rect(20, 186, 160, 4, InkColor::Yellow);
}

static void draw_factory_reset_failed(Epaper29Tri &epd,
                                      const TimeKeeper &clock) {
  TinyGfx g(epd);
  epd.clear(InkColor::White);
  draw_pretty_header(g, clock);
  draw_event_shell(g, PrettyMood::Curious, "RESET FAILED");
  rpg_frame(g, 4, 140, 192, 56, true);
  centered_ui_text(g, 153, "MEMORY NOT ERASED", UiFont::SmallBold,
                   InkColor::Black, 8, 192);
  centered_ui_text(g, 174, "REBOOT + TRY AGAIN", UiFont::SmallBold,
                   InkColor::Black, 8, 192);
}

static void draw_status(Epaper29Tri &epd,
                        const LifeState &life,
                        const WifiFriendBook &wifi_book,
                        const BleFriendBook &ble_book,
                        const TrimPeerBook &peer_book,
                        const WifiScanResult &scan,
                        const BleScanResult &ble_scan,
                        const EpochReward &reward,
                        uint32_t new_ble,
                        uint32_t new_peers,
                        uint32_t peer_bonus_xp,
                        uint32_t total_epoch_xp,
                        uint64_t newest_peer_id,
                        const ExplorerUpdate &ex,
                        const ImuMotionSnapshot &motion,
                        const DailyStats &day,
                        const TimeKeeper &clock,
                        const MissionEngine &missions,
                        const AchievementEngine &badges) {
  (void)wifi_book; (void)ble_book; (void)scan; (void)ble_scan;
  (void)total_epoch_xp; (void)newest_peer_id;

  TinyGfx g(epd);
  epd.clear(InkColor::White);
  draw_pretty_header(g, clock);

  const bool homecoming = ex.journey_ended;
  const bool walkies = motion.present && motion.walkies_since_last_take;
  const bool radio_curious = reward.new_friends > 0 || new_ble > 0;
  const bool discovery = life.journey_active && radio_curious;

  PrettyMood mood = PrettyMood::Home;
  if (homecoming) mood = PrettyMood::Homecoming;
  else if (new_peers > 0) mood = PrettyMood::Friend;
  else if (life.journey_active || ex.journey_started)
    mood = discovery ? PrettyMood::Discovery : PrettyMood::Adventure;
  else if (walkies) mood = PrettyMood::Walkies;
  else if (radio_curious) mood = PrettyMood::Curious;

  draw_pet_panel(g, mood);

  if (homecoming) {
    draw_social_state(g, "HOME AGAIN", UiIcon::Xp, "TRIP XP",
                      ex.completed_journey_xp,
                      UiIcon::Wifi, "NEW WIFI", ex.completed_journey_new);
  } else if (new_peers > 0) {
    draw_social_state(g, "NEW FRIEND", UiIcon::Xp, "BONUS",
                      peer_bonus_xp,
                      UiIcon::Friends, "FRIENDS", peer_book.count);
  } else if (life.journey_active || ex.journey_started) {
    draw_adventure_state(g, life, day, clock);
  } else if (walkies) {
    draw_walkies_state(g, life, day, clock);
  } else {
    draw_generic_state(g, radio_curious ? "CURIOUS" : "HOME",
                       life, day, clock);
  }

  draw_pretty_mission(g, missions);
  draw_pretty_footer(g, life, peer_book, badges, clock);
}


struct SnoozeResult {
  bool early_walkies = false;
  bool used_light_sleep = false;
};

static SnoozeResult snooze_until(ImuSense &imu,
                                  WifiSense &wifi,
                                  BleSense &ble,
                                  FactoryResetGesture &factory_reset,
                                  int64_t sleep_ms) {
  SnoozeResult result;
  if (sleep_ms <= 0) return result;

  const esp_err_t ble_sleep_err = ble.sleep();
  if (ble_sleep_err != ESP_OK) {
    ESP_LOGW(TAG, "Could not stop BLE for SNOOZE: %s; staying awake this cycle",
             esp_err_to_name(ble_sleep_err));
    vTaskDelay(pdMS_TO_TICKS(static_cast<uint32_t>(sleep_ms)));
    return result;
  }

  const esp_err_t wifi_err = wifi.sleep();
  if (wifi_err != ESP_OK) {
    ESP_LOGW(TAG, "Could not stop Wi-Fi for SNOOZE: %s",
             esp_err_to_name(wifi_err));
    (void)ble.wake();
    vTaskDelay(pdMS_TO_TICKS(static_cast<uint32_t>(sleep_ms)));
    return result;
  }

  const int64_t deadline_us =
      esp_timer_get_time() + (sleep_ms * 1000LL);

  bool imu_snooze = false;
  if (imu.present() && imu.pedometer_ready()) {
    const esp_err_t imu_err = imu.enter_snooze();
    if (imu_err == ESP_OK) {
      imu_snooze = true;
    } else {
      ESP_LOGW(TAG, "IMU SNOOZE unavailable: %s", esp_err_to_name(imu_err));
    }
  }

  // The WHO_AM_I 0x70 MPU-compatible board has no trusted hardware wake
  // interrupt path. Keep its known-good 20 Hz FIFO strategy: the C6 sleeps in
  // four-second slices, polls accumulated accel samples, and only calls WALKIES
  // after four cadence-confirmed steps. Radios stay off for the whole period.
#if defined(ES_PET_ULTIMATE_XIAO)
  constexpr int64_t MPU_SLICE_US = 60000000LL; // LSM tap IRQ can wake earlier
#else
  constexpr int64_t MPU_SLICE_US = 4000000LL;
#endif
  constexpr uint32_t WALKIES_CONFIRM_STEPS = 4;
  uint32_t consecutive_cadence_steps = 0;

  while (true) {
#if defined(ES_PET_ULTIMATE_XIAO)
    // The sampler may already have consumed the latched IRQ just before we
    // entered snooze. Honour its queued redraw before waiting for another IRQ.
    if (imu_snooze && imu.redraw_pending()) {
      haptic_tap("double tap");
      g_ultimate_tap_chirped = true;
      break;
    }
#endif
    const int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us <= 1000) break;

    const int64_t slice_us =
        imu_snooze ? std::min<int64_t>(remaining_us, MPU_SLICE_US)
                   : remaining_us;

    ESP_ERROR_CHECK(
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL));
    ESP_ERROR_CHECK(
        esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(slice_us)));

    // Hidden factory-reset service button must work even while sleeping.
    ESP_ERROR_CHECK(
        gpio_wakeup_enable(FactoryResetGesture::kButtonGpio,
                           GPIO_INTR_LOW_LEVEL));
#if defined(ES_PET_ULTIMATE_XIAO)
    if (imu_snooze) {
      // GPIO2/D2 has deep/light-sleep wake capability on XIAO C6.
      ESP_ERROR_CHECK(gpio_wakeup_enable(GPIO_NUM_2, GPIO_INTR_HIGH_LEVEL));
    }
#endif
    ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup());

    ESP_LOGI(TAG,
             "SNOOZE: light sleep %lld ms%s",
             static_cast<long long>(slice_us / 1000),
#if defined(ES_PET_ULTIMATE_XIAO)
             imu_snooze ? " (LSM hardware steps + double tap + BOOT)" : " (timer + BOOT)");
#else
             imu_snooze ? " (MPU FIFO + BOOT watch)" : " (timer + BOOT)");
#endif

    result.used_light_sleep = true;
    const esp_err_t sleep_err = esp_light_sleep_start();
    if (sleep_err != ESP_OK) {
      ESP_LOGW(TAG, "Light sleep failed: %s", esp_err_to_name(sleep_err));
      (void)gpio_wakeup_disable(FactoryResetGesture::kButtonGpio);
      break;
    }

    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    const bool boot_button_wake =
        cause == ESP_SLEEP_WAKEUP_GPIO &&
        gpio_get_level(FactoryResetGesture::kButtonGpio) == 0;
    (void)gpio_wakeup_disable(FactoryResetGesture::kButtonGpio);
#if defined(ES_PET_ULTIMATE_XIAO)
    (void)gpio_wakeup_disable(GPIO_NUM_2);
#endif

    if (boot_button_wake) {
      factory_reset.note_wake_press();
      ESP_LOGI(TAG, "SNOOZE wake: BOOT service button");
      if (imu_snooze) (void)imu.poll_snooze();
      break;
    }

    if (!imu_snooze) break;

    const ImuSnoozePoll poll = imu.poll_snooze();
#if defined(ES_PET_ULTIMATE_XIAO)
    if (poll.wake_event) {
      ESP_LOGI(TAG, "LSM6DS3 double tap: leave snooze for requested redraw");
      haptic_tap("double tap");
      g_ultimate_tap_chirped = true;
      break;
    }
#endif
    if (!poll.ok) {
      ESP_LOGW(TAG,
               "IMU snooze poll failed; continuing toward normal SCAN deadline");
      consecutive_cadence_steps = 0;
    } else if (poll.new_steps > 0) {
      consecutive_cadence_steps += poll.new_steps;
      ESP_LOGI(TAG,
               "SNOOZE: +%lu steps this slice; cadence total=%lu",
               static_cast<unsigned long>(poll.new_steps),
               static_cast<unsigned long>(consecutive_cadence_steps));

      if (consecutive_cadence_steps >= WALKIES_CONFIRM_STEPS) {
        imu.latch_walkies_from_snooze();
        result.early_walkies = true;
        ESP_LOGI(TAG,
                 "SNOOZE: WALKIES confirmed (+%lu cadence steps) -> early SCAN",
                 static_cast<unsigned long>(consecutive_cadence_steps));
        break;
      }
    } else {
      // A no-step slice breaks a handling-only sequence.
      consecutive_cadence_steps = 0;
    }
  }

  if (imu_snooze) {
    const esp_err_t imu_err = imu.exit_snooze();
    if (imu_err != ESP_OK) {
      ESP_LOGW(TAG, "Could not fully restore active IMU mode: %s",
               esp_err_to_name(imu_err));
    }
  }

  ESP_ERROR_CHECK(
      esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL));

  const esp_err_t wake_err = wifi.wake();
  if (wake_err != ESP_OK) {
    ESP_LOGE(TAG, "Could not restart Wi-Fi after SNOOZE: %s",
             esp_err_to_name(wake_err));
  }

  const esp_err_t ble_wake_err = ble.wake();
  if (ble_wake_err != ESP_OK) {
    ESP_LOGW(TAG, "Could not restart BLE after SNOOZE: %s",
             esp_err_to_name(ble_wake_err));
  }

  return result;
}

static void report_big_brain_partitions() {
  const esp_partition_t *factory = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP,
      ESP_PARTITION_SUBTYPE_APP_FACTORY,
      nullptr);

  const esp_partition_t *nvs = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA,
      ESP_PARTITION_SUBTYPE_DATA_NVS,
      "nvs");

  const esp_partition_t *diary = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA,
      ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
      "diary");

  if (factory) {
    ESP_LOGI(TAG,
             "BIG BRAIN partition: factory offset=0x%06lX size=0x%06lX (%lu bytes)",
             static_cast<unsigned long>(factory->address),
             static_cast<unsigned long>(factory->size),
             static_cast<unsigned long>(factory->size));
  } else {
    ESP_LOGE(TAG, "BIG BRAIN CHECK FAILED: factory partition not found");
  }

  if (nvs) {
    ESP_LOGI(TAG,
             "NVS preserved: offset=0x%06lX size=0x%04lX",
             static_cast<unsigned long>(nvs->address),
             static_cast<unsigned long>(nvs->size));
  }

  if (diary) {
    ESP_LOGI(TAG,
             "DIARY preserved: offset=0x%06lX size=0x%06lX",
             static_cast<unsigned long>(diary->address),
             static_cast<unsigned long>(diary->size));
  }
}

extern "C" void app_main(void) {
  ESP_LOGI(TAG, "==========================================");
  ESP_LOGI(TAG, " ES-PET V0.8L RC7 - MISSION ECONOMY");
  ESP_LOGI(TAG, " 200x200 UNIVERSAL + PRESERVED RC6 LIFE DATA");
  ESP_LOGI(TAG, "==========================================");

  // Kill the onboard RGB LED as early as possible.
  esp_err_t led_err = onboard_rgb_off();
  if (led_err != ESP_OK) {
    ESP_LOGW(TAG, "Could not extinguish onboard RGB LED: %s",
             esp_err_to_name(led_err));
  }

  // Final haptic hardware: GPIO1 -> AO3400A gate -> vibration motor.
  // Failure is non-fatal so the pet still boots if the add-on is absent.
  esp_err_t haptic_err = haptic_init();
  if (haptic_err != ESP_OK) {
    ESP_LOGW(TAG, "Haptic unavailable this boot: %s",
             esp_err_to_name(haptic_err));
  }

  // RC7 optional battery monitor.
  // GPIO0 with final 10k/4.7k divider when fitted.
  // Absence or ADC failure must never prevent ES-PET from booting.
  esp_err_t battery_err = battery_init();
  if (battery_err != ESP_OK) {
    ESP_LOGW(TAG,
             "Battery monitor unavailable this boot: %s",
             esp_err_to_name(battery_err));
  }

  ESP_ERROR_CHECK(life_memory_init());
  ESP_ERROR_CHECK(patch_begin());
  
  report_big_brain_partitions();

  static LifeState life;
  ESP_ERROR_CHECK(life_load(life));

  static WifiFriendBook book;
  ESP_ERROR_CHECK(wifi_book_load(book));

  static BleFriendBook ble_book;
  ESP_ERROR_CHECK(ble_book_load(ble_book));

  static TrimPeerBook peer_book;
  ESP_ERROR_CHECK(trim_peer_book_load(peer_book));

  static HomeProfile home;
  ESP_ERROR_CHECK(home_profile_load(home));

  static EncounterMemory encounters;
  ESP_ERROR_CHECK(encounters.begin(book, ble_book, life));

  static EconomyTracker economy;
  ESP_ERROR_CHECK(economy.begin());

  life.boots += 1;
  // From V0.7 onward unique_wifi/unique_ble are scalable lifetime counters,
  // no longer aliases for the 256-entry exact recognition caches.
  life.level = level_from_xp(life.xp);
  ESP_ERROR_CHECK(life_save(life));

  ESP_LOGI(TAG,
           "Life: boot=%lu level=%lu xp=%lu epochs=%lu wifi=%u ble=%u "
           "peers=%u journeys=%lu home=%u steps=%lu",
           static_cast<unsigned long>(life.boots),
           static_cast<unsigned long>(life.level),
           static_cast<unsigned long>(life.xp),
           static_cast<unsigned long>(life.epochs),
           static_cast<unsigned>(life.unique_wifi),
           static_cast<unsigned>(life.unique_ble),
           static_cast<unsigned>(peer_book.count),
           static_cast<unsigned long>(life.journeys),
           static_cast<unsigned>(home.count),
           static_cast<unsigned long>(life.steps_total));
  ESP_LOGI(TAG, "MINI pending TODAY steps=%lu",
           static_cast<unsigned long>(life.pending_unsynced_steps));

  // V0.8L intentionally has no OTA portal. Updates are performed over USB.

  Epaper29Tri epd;
  ESP_ERROR_CHECK(epd.begin());

  FactoryResetGesture factory_reset;
  esp_err_t factory_reset_err = factory_reset.begin();
  if (factory_reset_err != ESP_OK) {
    ESP_LOGW(TAG, "Factory reset gesture unavailable: %s",
             esp_err_to_name(factory_reset_err));
  }

  ImuSense imu;
  esp_err_t imu_err = imu.begin();
  if (imu_err == ESP_OK) {
    ESP_ERROR_CHECK(imu.start());
#if defined(ES_PET_ULTIMATE_XIAO)
    g_ultimate_imu = &imu;
    ESP_LOGI(TAG, "Ultimate maintenance v0.2: RF antenna, clock recovery, live steps, single-refresh taps");
#endif
  } else {
    ESP_LOGW(TAG, "IMU unavailable this boot: %s", esp_err_to_name(imu_err));
  }

  WifiSense wifi;
  ESP_ERROR_CHECK(wifi.begin());

  TimeKeeper clock;
  ESP_ERROR_CHECK(clock.begin());

  if (!clock.configured()) {
    draw_clock_setup(epd, clock);
    refresh_epaper_nonfatal(epd, "screen update");

    bool clock_saved = false;
    esp_err_t setup_err = clock.run_setup_portal(wifi, 300000, clock_saved);
    if (setup_err != ESP_OK) {
      ESP_LOGW(TAG, "First-run setup portal failed: %s", esp_err_to_name(setup_err));
    } else if (!clock_saved) {
      ESP_LOGW(TAG, "First-run setup skipped/timed out; time features remain dormant");
    }
  }

  PetIdentity identity;
  ESP_ERROR_CHECK(pet_identity_load_or_create(identity));

  MissionEngine missions;
  ESP_ERROR_CHECK(missions.begin(identity.pet_id));

  AchievementEngine badges;
  ESP_ERROR_CHECK(
      badges.begin(life, peer_book, encounters.daily(),
                   missions.best_walk_streak()));

  BleSense ble;
  esp_err_t ble_err = ble.begin(identity.pet_id);
  if (ble_err != ESP_OK) {
    ESP_LOGW(TAG, "BLE SOCIAL unavailable this boot: %s",
             esp_err_to_name(ble_err));
  }

  Explorer explorer;
  bool first_epoch_this_boot = true;

  // E-paper retains its image with no power. Track what we last painted so
  // repeated minute-by-minute sleeps do not waste energy refreshing NAPTIME.
  bool screen_snoozing = false;

  // V0.8 PERCHED:
  // If an active ADVENTURE has had no genuine walking for five consecutive
  // SCANs, stop minute-by-minute radio work. On the MINI the 0x70 MPU clone
  // is watched through short low-power FIFO slices; genuine walking resumes
  // scanning immediately.
  bool screen_perched = false;
  uint8_t away_stationary_scans = 0;

  while (true) {
    if (factory_reset.armed()) {
      haptic_tap("factory reset armed");
      draw_factory_reset_prompt(epd, clock);
      refresh_epaper_nonfatal(epd, "screen update");
      factory_reset.acknowledge_prompt();

      while (factory_reset.armed() && !factory_reset.reset_requested()) {
        vTaskDelay(pdMS_TO_TICKS(50));
      }

      if (factory_reset.reset_requested()) {
        draw_factory_reset_goodbye(epd, clock);
        refresh_epaper_nonfatal(epd, "screen update");
        vTaskDelay(pdMS_TO_TICKS(400));

        const esp_err_t erase_err = erase_trimagotchi_user_data();
        if (erase_err != ESP_OK) {
          ESP_LOGE(TAG, "Factory reset failed: %s",
                   esp_err_to_name(erase_err));
          draw_factory_reset_failed(epd, clock);
          refresh_epaper_nonfatal(epd, "screen update");
          vTaskDelay(pdMS_TO_TICKS(5000));
          esp_restart();
        } else {
          vTaskDelay(pdMS_TO_TICKS(600));
          esp_restart();
        }
      }
    }

    const int64_t epoch_started_us = esp_timer_get_time();

    WifiScanResult scan;
#if defined(ES_PET_ULTIMATE_XIAO)
    esp_err_t scan_err = wifi.passive_scan(scan, clock.home_ssid());
#else
    esp_err_t scan_err = wifi.passive_scan(scan);
#endif

    if (scan_err != ESP_OK) {
      ESP_LOGE(TAG, "Scan error: %s", esp_err_to_name(scan_err));
      vTaskDelay(pdMS_TO_TICKS(10000));
      continue;
    }

    ++life.epochs;

    ImuMotionSnapshot motion = imu.take_snapshot();

    BleScanResult ble_scan;
    if (ble.ready()) {
      esp_err_t bs = ble.sense(
          ble_scan,
          motion.walkies_since_last_take || motion.walking_now,
          life.journey_active,
          3500);
      if (bs != ESP_OK) {
        ESP_LOGW(TAG, "BLE epoch unavailable: %s", esp_err_to_name(bs));
      }
    }

    bool new_day = false;
    bool new_mission_day = false;
    bool daily_observed_this_scan = false;

    uint32_t today_new_wifi = 0;
    uint32_t today_new_ble = 0;

    // Known-but-first-today identities earn half the new-ever reward.
    uint32_t known_today_wifi = 0;
    uint32_t repeat_today_wifi = 0;
    uint32_t known_today_ble = 0;
    uint32_t repeat_today_ble = 0;

    if (clock.valid()) {
      const uint32_t date_key = clock.local_date_key();
      new_day = encounters.ensure_day(date_key);

      esp_err_t mission_day_err =
          missions.ensure_day(date_key, clock.local_hour(), new_mission_day);
      if (mission_day_err != ESP_OK) {
        ESP_LOGW(TAG, "Could not prepare daily missions: %s",
                 esp_err_to_name(mission_day_err));
      }

      (void)economy.ensure_day(date_key);

      daily_observed_this_scan = true;
    }

    uint32_t new_friends = 0;
    uint32_t familiar_friends = 0;
    bool wifi_cache_changed = false;

    const uint16_t lifetime_new_channels =
        static_cast<uint16_t>(scan.channel_mask & ~book.channel_mask);

    for (const auto &obs : scan.observations) {
      const bool lifetime_new =
          encounters.observe_lifetime_wifi(obs.anonymous_id);

      bool first_today = false;
      if (daily_observed_this_scan) {
        first_today =
            encounters.observe_daily_wifi(obs.anonymous_id);
        if (first_today) ++today_new_wifi;
      }

      if (lifetime_new) {
        ++new_friends;
        ++life.unique_wifi;
      } else if (first_today) {
        ++known_today_wifi;
        ++familiar_friends;
      } else {
        ++repeat_today_wifi;
        ++familiar_friends;
      }

      // Keep the legacy exact book as a small recognition cache/seed. Once
      // full it simply stops growing; lifetime identity is now the Bloom map.
      if (!wifi_book_contains(book, obs.anonymous_id) &&
          book.count < WIFI_BOOK_CAPACITY &&
          wifi_book_add(book, obs.anonymous_id)) {
        wifi_cache_changed = true;
      }
    }

    const uint32_t new_channels = popcount16(lifetime_new_channels);
    if (new_channels > 0) wifi_cache_changed = true;
    book.channel_mask |= scan.channel_mask;

    EpochReward reward =
        calculate_epoch_reward(
            new_friends,
            known_today_wifi,
            repeat_today_wifi,
            new_channels);

    uint32_t new_ble = 0;
    uint32_t familiar_ble = 0;
    bool ble_cache_changed = false;

    for (const auto &obs : ble_scan.stable_observations) {
      const bool lifetime_new =
          encounters.observe_lifetime_ble(obs.anonymous_id);

      bool first_today = false;
      if (daily_observed_this_scan) {
        first_today =
            encounters.observe_daily_ble(obs.anonymous_id);
        if (first_today) ++today_new_ble;
      }

      if (lifetime_new) {
        ++new_ble;
        ++life.unique_ble;
      } else if (first_today) {
        ++known_today_ble;
        ++familiar_ble;
      } else {
        ++repeat_today_ble;
        ++familiar_ble;
      }

      if (!ble_book_contains(ble_book, obs.anonymous_id) &&
          ble_book.count < BLE_BOOK_CAPACITY &&
          ble_book_add(ble_book, obs.anonymous_id)) {
        ble_cache_changed = true;
      }
    }

    uint32_t new_peers = 0;
    uint32_t familiar_peers = 0;
    uint64_t newest_peer_id = 0;
    bool peer_book_changed = false;

    for (const auto &peer : ble_scan.trimagotchi_peers) {
      const bool first_ever =
          trim_peer_touch(peer_book, peer.pet_id, life.epochs);
      if (first_ever) {
        ++new_peers;
        newest_peer_id = peer.pet_id;
        peer_book_changed = true;
        ESP_LOGI(TAG,
                 ">>> ANOTHER TRIMAGOTCHI: rssi=%d "
                 "proto=v%u caps=0x%02X state=0x%02X <<<",
                 static_cast<int>(peer.rssi),
                 static_cast<unsigned>(peer.protocol_version),
                 static_cast<unsigned>(peer.capabilities),
                 static_cast<unsigned>(peer.state_flags));
      } else {
        ++familiar_peers;
      }
    }

    const uint32_t ble_xp =
        calculate_ble_reward(new_ble, known_today_ble);
    const uint32_t raw_radio_xp = reward.xp + ble_xp;
    const uint32_t radio_xp_awarded =
        economy.award_radio(raw_radio_xp);
    const uint32_t peer_bonus_xp =
        new_peers * es_pet::economy::kNewPeerXp;
    const uint32_t base_epoch_xp =
        radio_xp_awarded + peer_bonus_xp;
    uint32_t total_epoch_xp = base_epoch_xp;

    if (motion.present) {
      const bool handled_only =
          motion.moved_since_last_take && !motion.walkies_since_last_take;

      ESP_LOGI(TAG,
               "STEPS scan=%lu +%lu total_boot=%lu recent15=%u walkies=%s "
               "raw=%s%s",
               static_cast<unsigned long>(life.epochs),
               static_cast<unsigned long>(motion.steps_since_last_take),
               static_cast<unsigned long>(motion.steps_since_boot),
               static_cast<unsigned>(motion.recent_steps_15s),
               motion.walkies_since_last_take ? "YES" : "NO",
               motion.moved_since_last_take ? "MOVED" : "STILL",
               handled_only ? " (HANDLED/IGNORED)" : "");
    }

    // Lifetime step count is backward-compatible: older firmware simply has
    // no NVS "steps" key, so V0.4.1 starts it at zero and preserves it from
    // this point onward.
    life.steps_total += motion.steps_since_last_take;

    const uint32_t old_level = life.level;

    // Explorer journey scoring uses the radio/social reward for this scan.
    // Mission and badge rewards are separate game bonuses rather than
    // artificially inflating a journey's discovery score.
    ExplorerUpdate ex =
        explorer.update(home, life, scan, base_epoch_xp, new_friends);

    // HOME is the only place Trimagotchi ever joins a Wi-Fi network. BLE is
    // briefly torn down for the infrequent SNTP transaction, then restored.
    bool clock_just_synced = false;
    if (!clock.valid()) {
      ESP_LOGI(TAG, "NO CLOCK: setup=%s HOME-fingerprint=%s saved-home-visible=%s",
               clock.configured() ? "YES" : "NO",
               ex.home_confident ? "YES" : "NO",
               scan.saved_home_visible ? "YES" : "NO");
    }
    if (clock.should_sync(ex.home_confident, scan.saved_home_visible)) {
      if (!ex.home_confident && scan.saved_home_visible) {
        ESP_LOGI(TAG, "Clock recovery: saved HOME Wi-Fi visible; fingerprint not required");
      }
      esp_err_t bs = ble.sleep();
      if (bs == ESP_OK) {
        esp_err_t ts = clock.sync_at_home(wifi);
        clock_just_synced = (ts == ESP_OK);
        esp_err_t bw = ble.wake();
        if (bw != ESP_OK) {
          ESP_LOGW(TAG, "BLE restore after clock sync failed: %s",
                   esp_err_to_name(bw));
        }
      } else {
        ESP_LOGW(TAG, "Could not pause BLE for clock sync: %s",
                 esp_err_to_name(bs));
      }
    }

    MissionUpdate mission_update;
    AchievementUpdate badge_update;
    uint32_t perfect_day_bonus_xp = 0;

    if (clock.valid()) {
      const uint32_t date_key = clock.local_date_key();

      // The clock may have become valid during this SCAN after HOME SNTP.
      // If so, establish today's filters now. Novelty XP for familiar IDs
      // begins from the next SCAN, but today's counters and missions can use
      // the current observations immediately.
      if (!daily_observed_this_scan) {
        new_day = encounters.ensure_day(date_key);

        esp_err_t mission_day_err =
            missions.ensure_day(
                date_key, clock.local_hour(), new_mission_day);
        if (mission_day_err != ESP_OK) {
          ESP_LOGW(TAG, "Could not prepare daily missions: %s",
                   esp_err_to_name(mission_day_err));
        }

        (void)economy.ensure_day(date_key);

        for (const auto &obs : scan.observations) {
          if (encounters.observe_daily_wifi(obs.anonymous_id)) {
            ++today_new_wifi;
          }
        }

        for (const auto &obs : ble_scan.stable_observations) {
          if (encounters.observe_daily_ble(obs.anonymous_id)) {
            ++today_new_ble;
          }
        }

        daily_observed_this_scan = true;
      }

      encounters.add_daily_wifi(today_new_wifi);
      encounters.add_daily_ble(today_new_ble);

      // MINI continuity: steps collected before HOME/SNTP knew the local date
      // are kept in a persistent provisional TODAY pot. On first valid date,
      // merge them into the daily counter exactly once. Timed daily missions
      // still receive only this scan's dated step delta below.
      const uint32_t carried_unsynced_steps =
          life.pending_unsynced_steps;
      encounters.add_daily_steps(
          motion.steps_since_last_take + carried_unsynced_steps);
      if (carried_unsynced_steps > 0) {
        ESP_LOGI(TAG,
                 "TODAY CATCH-UP: moving %lu unsynced steps into %lu",
                 static_cast<unsigned long>(carried_unsynced_steps),
                 static_cast<unsigned long>(date_key));
        life.pending_unsynced_steps = 0;
      }

      if (ex.journey_started) encounters.add_daily_adventure();
      for (uint32_t i = 0; i < new_peers; ++i) {
        encounters.add_daily_peer_meeting();
      }

      MissionInput mi;
      mi.date_key = date_key;
      mi.local_hour = clock.local_hour();
      mi.steps_delta = motion.steps_since_last_take;
      mi.steps_today = encounters.daily().steps;
      mi.wifi_new_delta = today_new_wifi;
      mi.bt_new_delta = today_new_ble;
      mi.wifi_lifetime_new_delta = new_friends;
      mi.bt_lifetime_new_delta = new_ble;
      mi.peer_new_delta = new_peers;
      mi.adventure_started = ex.journey_started;
      mi.adventure_ended = ex.journey_ended;

      mission_update = missions.update(mi);

      // Award the perfect-day bonus only on the transition that completes
      // the third mission. Existing RC6 XP and already-paid missions are
      // never recalculated or reduced during the RC7 upgrade.
      perfect_day_bonus_xp = es_pet::economy::perfect_day_bonus(
          mission_update.completed_count,
          missions.completed_count());

      total_epoch_xp +=
          mission_update.xp_awarded +
          mission_update.challenge_xp_awarded +
          perfect_day_bonus_xp;

      // Base radio/social XP and mission rewards are now applied together.
      life.xp += total_epoch_xp;
      life.level = level_from_xp(life.xp);

      // Evaluate permanent achievements after today's counters and mission XP
      // have moved, so a threshold crossed on this scan is seen immediately.
      badge_update =
          badges.evaluate(life, peer_book, encounters.daily(),
                          missions.best_walk_streak());

      if (badge_update.xp_awarded > 0) {
        life.xp += badge_update.xp_awarded;
        total_epoch_xp += badge_update.xp_awarded;
        life.level = level_from_xp(life.xp);
      }

      encounters.add_daily_xp(total_epoch_xp);
    } else {
      // MINI continuity: lifetime steps count immediately, while this second
      // persistent counter keeps TODAY useful before HOME has supplied a date.
      life.pending_unsynced_steps += motion.steps_since_last_take;
      if (motion.steps_since_last_take > 0) {
        ESP_LOGI(TAG,
                 "TODAY PENDING: +%lu steps waiting for HOME time (pending=%lu)",
                 static_cast<unsigned long>(motion.steps_since_last_take),
                 static_cast<unsigned long>(life.pending_unsynced_steps));
      }

      // Ordinary radio/social XP still works. Date/time missions and streaks
      // remain dormant until the local clock is trustworthy.
      life.xp += base_epoch_xp;
      life.level = level_from_xp(life.xp);
    }

    if (static_cast<int32_t>(total_epoch_xp) > life.best_epoch) {
      life.best_epoch = static_cast<int32_t>(total_epoch_xp);
    }

    const bool learned_radio =
        new_friends > 0 || new_channels > 0;

    if (wifi_cache_changed) {
      ESP_ERROR_CHECK(wifi_book_save(book));
    }

    if (ble_cache_changed) {
      ESP_ERROR_CHECK(ble_book_save(ble_book));
    }

    if (peer_book_changed) {
      ESP_ERROR_CHECK(trim_peer_book_save(peer_book));
    }

    if (ex.home_profile_created) {
      ESP_ERROR_CHECK(home_profile_save(home));
    }

    if (clock.valid()) {
      if (ex.journey_started) {
        (void)encounters.append_event(
            DiaryEventType::AdventureStart,
            clock.unix_time(), 0, life.journeys, total_epoch_xp);
      }
      if (ex.journey_ended) {
        (void)encounters.append_event(
            DiaryEventType::AdventureEnd,
            clock.unix_time(), 0,
            ex.completed_journey_xp,
            ex.completed_journey_epochs);
      }
      if (new_peers > 0 && newest_peer_id != 0) {
        (void)encounters.append_event(
            DiaryEventType::PeerFirstMeeting,
            clock.unix_time(), newest_peer_id,
            new_peers, peer_bonus_xp);
      }
      if (mission_update.completed_count > 0) {
        (void)encounters.append_event(
            DiaryEventType::MissionComplete,
            clock.unix_time(), 0,
            static_cast<uint32_t>(mission_update.latest_completed),
            mission_update.xp_awarded + perfect_day_bonus_xp);
      }
      if (mission_update.challenge_completed_count > 0) {
        (void)encounters.append_event(
            DiaryEventType::ChallengeComplete,
            clock.unix_time(), 0,
            static_cast<uint32_t>(mission_update.latest_challenge),
            mission_update.challenge_xp_awarded);
      }
      if (badge_update.new_count > 0) {
        (void)encounters.append_event(
            DiaryEventType::AchievementUnlocked,
            clock.unix_time(), 0,
            static_cast<uint32_t>(badge_update.latest),
            badge_update.xp_awarded);
      }
      if (life.level > old_level) {
        (void)encounters.append_event(
            DiaryEventType::LevelUp,
            clock.unix_time(), 0, life.level, life.xp);
      }
    }

    const bool important_state_change =
        learned_radio ||
        new_ble > 0 ||
        new_peers > 0 ||
        ex.home_profile_created ||
        ex.journey_started ||
        ex.journey_ended ||
        new_day ||
        new_mission_day ||
        mission_update.completed_count > 0 ||
        mission_update.challenge_completed_count > 0 ||
        badge_update.new_count > 0 ||
        clock_just_synced ||
        life.level > old_level;

    // Haptics are deliberately reserved for rare, meaningful events.
    // Do not buzz for ordinary Wi-Fi/BLE discoveries, steps, scans or wakeups.
    // If several rewards land on one scan, give only one 100 ms tap.
    const bool haptic_event =
        new_peers > 0 ||
        ex.journey_started ||
        ex.journey_ended ||
        mission_update.completed_count > 0 ||
        mission_update.challenge_completed_count > 0 ||
        badge_update.new_count > 0 ||
        life.level > old_level;

    if (haptic_event) {
      haptic_tap("significant event");
    }

    const bool steps_changed = motion.steps_since_last_take > 0;

    if (important_state_change || total_epoch_xp > 0 ||
        life.journey_active || steps_changed ||
        ((life.epochs % 10u) == 0u)) {
      ESP_ERROR_CHECK(life_save(life));
    }

    if (clock.valid() &&
        (new_day || new_mission_day ||
         mission_update.completed_count > 0 ||
         mission_update.challenge_completed_count > 0 ||
         badge_update.new_count > 0 ||
         clock_just_synced || important_state_change ||
         (life.epochs % 5u) == 0u)) {
      ESP_ERROR_CHECK(encounters.save_daily());
    }

    // Release builds retain only the daily allowance required to enforce the
    // cap. No per-source history or diagnostic event log is persisted.
    if (economy.dirty()) {
      ESP_ERROR_CHECK(economy.save());
    }

#if ES_PET_ECONOMY_TRACE
    // Opt-in serial-only correction aid. Disabled in RC7 release builds and
    // discarded automatically when the serial session/reboot ends.
    ESP_LOGI(TAG,
             "ECONOMY radio_raw=%lu radio_paid=%lu radio_today=%lu "
             "peer=%lu mission=%lu challenge=%lu perfect=%lu badge=%lu "
             "total=%lu",
             static_cast<unsigned long>(raw_radio_xp),
             static_cast<unsigned long>(radio_xp_awarded),
             static_cast<unsigned long>(economy.radio_xp_today()),
             static_cast<unsigned long>(peer_bonus_xp),
             static_cast<unsigned long>(mission_update.xp_awarded),
             static_cast<unsigned long>(mission_update.challenge_xp_awarded),
             static_cast<unsigned long>(perfect_day_bonus_xp),
             static_cast<unsigned long>(badge_update.xp_awarded),
             static_cast<unsigned long>(total_epoch_xp));
#endif

    ESP_LOGI(TAG,
             "EXPLORER scan=%lu "
             "Wvis=%u Wnew=%lu Wtoday=%lu Wrepeat=%lu "
             "Bvis=%lu Bnew=%lu Btoday=%lu Brepeat=%lu "
             "peers=%lu(+%lu) reward=+%lu XP=%lu level=%lu "
             "home=%.0f%% journey=%s tripXP=%lu steps=%lu",
             static_cast<unsigned long>(life.epochs),
             static_cast<unsigned>(scan.observations.size()),
             static_cast<unsigned long>(new_friends),
             static_cast<unsigned long>(known_today_wifi),
             static_cast<unsigned long>(repeat_today_wifi),
             static_cast<unsigned long>(ble_scan.visible_advertisers),
             static_cast<unsigned long>(new_ble),
             static_cast<unsigned long>(known_today_ble),
             static_cast<unsigned long>(repeat_today_ble),
             static_cast<unsigned long>(ble_scan.trimagotchi_peers.size()),
             static_cast<unsigned long>(new_peers),
             static_cast<unsigned long>(total_epoch_xp),
             static_cast<unsigned long>(life.xp),
             static_cast<unsigned long>(life.level),
             ex.home_similarity * 100.0f,
             life.journey_active ? "YES" : "NO",
             static_cast<unsigned long>(life.journey_xp),
             static_cast<unsigned long>(life.steps_total));

    if (clock.valid()) {
      const DailyStats &d = encounters.daily();
      char when[40];
      clock.format_local_datetime(when, sizeof(when));
      ESP_LOGI(TAG,
               "TIME/MEM %s today: steps=%lu W=%lu B=%lu XP=%lu | lifetime W=%lu B=%lu",
               when,
               static_cast<unsigned long>(d.steps),
               static_cast<unsigned long>(d.wifi_unique),
               static_cast<unsigned long>(d.ble_unique),
               static_cast<unsigned long>(d.xp),
               static_cast<unsigned long>(life.unique_wifi),
               static_cast<unsigned long>(life.unique_ble));

      const int fm = missions.featured_slot();
      const MissionSlot &ms =
          missions.slot(fm >= 0 ? static_cast<size_t>(fm) : 0);

      ESP_LOGI(TAG,
               "MISSION %u/3 complete | next=%s %lu/%lu",
               static_cast<unsigned>(missions.completed_count()),
               MissionEngine::short_name(
                   static_cast<MissionType>(ms.type)),
               static_cast<unsigned long>(ms.progress),
               static_cast<unsigned long>(ms.target));

      ESP_LOGI(TAG,
               "CHALLENGES walk=%u best=%u adventure=%u week=%u/7 month=%u",
               static_cast<unsigned>(missions.walk_streak()),
               static_cast<unsigned>(missions.best_walk_streak()),
               static_cast<unsigned>(missions.adventure_streak()),
               static_cast<unsigned>(missions.week_walk_days()),
               static_cast<unsigned>(missions.month_walk_days()));
    }

    if (ex.journey_started) {
      ESP_LOGI(TAG, ">>> ADVENTURE STARTED <<<");
    }

    if (ex.journey_ended) {
      ESP_LOGI(TAG,
               ">>> HOME AGAIN: %lu XP, %lu new friends, %lu looks <<<",
               static_cast<unsigned long>(ex.completed_journey_xp),
               static_cast<unsigned long>(ex.completed_journey_new),
               static_cast<unsigned long>(ex.completed_journey_epochs));
    }

    ESP_LOGI(TAG, "Main stack free watermark: %u bytes",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));

    const bool learning_home = home.count == 0;

    // STICKY NAP:
    //
    // Entering a nap still requires a genuinely quiet, confidently recognised
    // HOME epoch. But once the e-paper says SNOOZING, ordinary background
    // discoveries must NOT wake the display.
    //
    // This is important because e-paper refreshes are expensive compared with
    // simply storing a newly-seen anonymous Wi-Fi identity in NVS. A sleeping
    // Trimagotchi is allowed to learn quietly.
    const bool genuine_walkies =
        motion.present && motion.walkies_since_last_take;

    if (life.journey_active &&
        motion.steps_since_last_take == 0 &&
        !genuine_walkies) {
      if (away_stationary_scans < 255) ++away_stationary_scans;
    } else {
      away_stationary_scans = 0;
    }

    constexpr uint8_t PERCHED_AFTER_SCANS = 5;

    const bool can_enter_perch =
#if defined(ES_PET_ULTIMATE_XIAO)
        false && // Preserve the bench target's existing 15-minute scan policy.
#endif
        life.journey_active &&
        imu.present() &&
        imu.pedometer_ready() &&
        away_stationary_scans >= PERCHED_AFTER_SCANS &&
        !genuine_walkies &&
        !ex.journey_started &&
        !ex.journey_ended;

    const bool must_leave_perch =
        genuine_walkies ||
        ex.journey_ended ||
        new_peers > 0 ||
        mission_update.completed_count > 0 ||
        mission_update.challenge_completed_count > 0 ||
        badge_update.new_count > 0 ||
        life.level > old_level;

    const bool stay_perched =
        screen_perched &&
        life.journey_active &&
        !must_leave_perch;

    const bool ready_to_perch =
        stay_perched || can_enter_perch;

    const bool entering_perch =
        !screen_perched && can_enter_perch;

    const bool leaving_perch =
        screen_perched && must_leave_perch;

    const bool can_enter_nap =
        !learning_home &&
        ex.home_confident &&
        !life.journey_active &&
        !ex.journey_started &&
        !ex.journey_ended &&
        !genuine_walkies &&
        reward.new_friends == 0 &&
        !important_state_change;

    // Once asleep, stay asleep through:
    //   * new Wi-Fi friends/channels
    //   * XP/level changes caused by those discoveries
    //   * one- or two-epoch HOME fingerprint wobbles
    //   * raw handling / isolated false steps
    //
    // Explorer already requires consecutive away evidence before declaring
    // ADVENTURE, so the display wakes only when that transition is confirmed.
    const bool must_wake_from_nap =
        genuine_walkies ||
        new_peers > 0 ||
        mission_update.completed_count > 0 ||
        mission_update.challenge_completed_count > 0 ||
        badge_update.new_count > 0 ||
        life.level > old_level ||
        ex.journey_started ||
        ex.journey_ended ||
        life.journey_active;

    const bool stay_napping =
        screen_snoozing && !must_wake_from_nap;

    const bool ready_to_nap =
        !ready_to_perch &&
        (stay_napping || (!screen_snoozing && can_enter_nap));

    const bool leaving_nap =
        screen_snoozing && must_wake_from_nap;

    const bool entering_nap =
        !screen_snoozing && can_enter_nap;

    // While the screen is already asleep, background radio/XP events are
    // intentionally silent. They are still processed, rewarded and persisted.
    const bool active_important_refresh =
        !screen_snoozing &&
        !screen_perched &&
        important_state_change;

    const bool periodic_active_refresh =
        !screen_snoozing &&
        !screen_perched &&
        !ready_to_nap &&
        !ready_to_perch &&
        ((life.epochs % 5u) == 0u);

  #if defined(ES_PET_ULTIMATE_XIAO)
    // Ultimate: on-demand only. No redraw for radio, ordinary steps or XP.
    // First boot refresh is a one-time visibility aid after USB flashing.
    const bool user_redraw = imu.take_redraw_request();
    if (user_redraw && !g_ultimate_tap_chirped) haptic_tap("double tap");
    g_ultimate_tap_chirped = false;
    const bool refresh = first_epoch_this_boot || user_redraw;
#else
    const bool refresh =
        first_epoch_this_boot ||
        learning_home ||
        active_important_refresh ||
        leaving_nap ||
        entering_nap ||
        leaving_perch ||
        entering_perch ||
        genuine_walkies ||
        (!screen_snoozing && !screen_perched &&
         life.journey_active &&
         (new_friends > 0 || new_ble > 0 || new_peers > 0)) ||
        periodic_active_refresh;
#endif

    if (screen_snoozing && !must_wake_from_nap &&
        (new_friends > 0 || new_channels > 0 || new_ble > 0 ||
         life.level > old_level)) {
      ESP_LOGI(TAG,
               "STICKY NAP: learned quietly while asleep "
               "(wifi=%lu ble=%lu channels=%lu level=%lu)",
               static_cast<unsigned long>(new_friends),
               static_cast<unsigned long>(new_ble),
               static_cast<unsigned long>(new_channels),
               static_cast<unsigned long>(life.level));
    }

    if (refresh) {
      if (learning_home) {
        draw_learning_home(
            epd, life, explorer.learning_epoch(),
            static_cast<uint32_t>(scan.observations.size()));
        screen_snoozing = false;
        screen_perched = false;
      }
#if defined(ES_PET_ULTIMATE_XIAO)
      else {
        // Always show current status when requested; do not render
        // a permanent SNOOZING/PERCHED screen on an on-demand display.
        draw_status(epd, life, book, ble_book, peer_book,
                    scan, ble_scan, reward,
                    new_ble, new_peers, peer_bonus_xp,
                    total_epoch_xp, newest_peer_id, ex, motion,
                    encounters.daily(), clock, missions, badges);
        screen_snoozing = false;
        screen_perched = false;
      }
#else
      else if (ex.journey_ended || new_peers > 0) {
        // Homecoming and meeting another Trimagotchi outrank game popups.
        draw_status(epd, life, book, ble_book, peer_book,
                    scan, ble_scan, reward,
                    new_ble, new_peers, peer_bonus_xp,
                    total_epoch_xp, newest_peer_id, ex, motion,
                    encounters.daily(), clock, missions, badges);
        screen_snoozing = false;
        screen_perched = false;
      } else if (mission_update.challenge_completed_count > 0) {
        // Multi-day milestones are rare, so they outrank the ordinary daily
        // mission popup if both happen on the same scan. Both rewards/data are
        // still recorded.
        draw_challenge_complete(
            epd, clock,
            mission_update.latest_challenge,
            mission_update.challenge_xp_awarded);
        screen_snoozing = false;
        screen_perched = false;
      } else if (mission_update.completed_count > 0) {
        draw_mission_complete(
            epd, clock, missions,
            mission_update.latest_completed,
            mission_update.xp_awarded,
            perfect_day_bonus_xp);
        screen_snoozing = false;
        screen_perched = false;
      } else if (badge_update.new_count > 0) {
        draw_badge_unlocked(
            epd, clock, badges,
            badge_update.latest,
            badge_update.xp_awarded);
        screen_snoozing = false;
        screen_perched = false;
      } else if (life.level > old_level) {
        draw_level_up(epd, life, clock);
        screen_snoozing = false;
        screen_perched = false;
      } else if (ready_to_perch) {
        draw_perched(
            epd, life, peer_book,
            encounters.daily(), clock,
            missions, badges);
        screen_perched = true;
        screen_snoozing = false;
        ESP_LOGI(TAG,
                 "PERCHED: away + stationary; radios will sleep until walking");
      } else if (ready_to_nap) {
        draw_naptime(
            epd, life, peer_book, ex,
            encounters.daily(), clock,
            missions, badges);
        screen_snoozing = true;
        screen_perched = false;
        ESP_LOGI(TAG,
                 "NAPTIME: e-paper now shows SNOOZING; future quiet sleeps need no refresh");
      } else {
        draw_status(epd, life, book, ble_book, peer_book,
                    scan, ble_scan, reward,
                    new_ble, new_peers, peer_bonus_xp,
                    total_epoch_xp, newest_peer_id, ex, motion,
                    encounters.daily(), clock, missions, badges);
        screen_snoozing = false;
        screen_perched = false;
      }

#endif
      if (!refresh_epaper_nonfatal(epd, "epoch screen")) {
        // Do not remember a screen state that was never actually painted.
        // The next epoch will therefore attempt a fresh redraw.
        screen_snoozing = false;
        screen_perched = false;
      }
    }

    first_epoch_this_boot = false;

    if (factory_reset.armed()) {
      ESP_LOGI(TAG, "Factory reset gesture armed; skipping sleep for prompt");
      continue;
    }

    const int64_t elapsed_ms =
        (esp_timer_get_time() - epoch_started_us) / 1000;

#if defined(ES_PET_ULTIMATE_XIAO)
    // Bench starting point: radio scans ~every 15 minutes, not every minute.
    // Fine tune only after current and feature-coverage measurements.
    int64_t sleep_ms = (15LL * 60LL * 1000LL) - elapsed_ms;
#else
    int64_t sleep_ms = 60000 - elapsed_ms;
#endif
    if (sleep_ms < 1000) sleep_ms = 1000;

    if (screen_perched) {
      // MINI PERCHED keeps Wi-Fi/BLE off and uses the proven MPU FIFO cadence
      // watcher in four-second light-sleep slices. The long deadline is only
      // a safety backstop; WALKIES exits early.
      constexpr int64_t PERCHED_SAFETY_MS =
          6LL * 60LL * 60LL * 1000LL;

      ESP_LOGI(TAG,
               "PERCHED: radios off; MPU FIFO watching for WALKIES");

      SnoozeResult snooze =
          snooze_until(imu, wifi, ble, factory_reset, PERCHED_SAFETY_MS);

      if (snooze.early_walkies) {
        ESP_LOGI(TAG, "PERCHED -> WALKIES: immediate SCAN");
        screen_perched = false;
        away_stationary_scans = 0;
      } else {
        ESP_LOGI(TAG,
                 "PERCHED safety timer expired; one quiet SCAN then reassess");
      }
    } else {
      ESP_LOGI(TAG, "Next scan in %lld ms",
               static_cast<long long>(sleep_ms));

      SnoozeResult snooze =
          snooze_until(imu, wifi, ble, factory_reset, sleep_ms);

      if (snooze.early_walkies) {
        ESP_LOGI(TAG, "Walking woke Trimagotchi early");
      }
    }
  }
}
