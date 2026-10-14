#pragma once
#include <cstdint>
#include "epaper.h"
class TinyGfx;
enum class UiIcon : uint8_t { Steps, Wifi, Bluetooth, Friends, Badge, Xp, Home, Mission };
void draw_ui_icon(TinyGfx &g, UiIcon icon, int x, int y, InkColor color);
// RC3.3 portrait footer/card icons: 16x16, redrawn rather than 2x-scaled.
void draw_ui_icon_large(TinyGfx &g, UiIcon icon, int x, int y, InkColor color);
