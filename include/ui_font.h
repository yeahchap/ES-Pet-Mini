#pragma once
#include <cstdint>
#include <cstddef>
#include "epaper.h"
class TinyGfx;
enum class UiFont : uint8_t { Tiny, Small, SmallBold, Title, Number };
int ui_text_width(const char *s, UiFont font);
void ui_text(TinyGfx &g, int x, int y, const char *s,
             UiFont font, InkColor color);
