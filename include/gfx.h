#pragma once

#include <string>
#include "epaper.h"

class TinyGfx {
 public:
  explicit TinyGfx(Epaper29Tri &display);

  void pixel(int x, int y, InkColor c);
  void line(int x0, int y0, int x1, int y1, InkColor c);
  void rect(int x, int y, int w, int h, InkColor c);
  void fill_rect(int x, int y, int w, int h, InkColor c);
  void circle(int cx, int cy, int r, InkColor c);
  void fill_circle(int cx, int cy, int r, InkColor c);

  void bitmap_1bit(int x, int y, int w, int h,
                   const uint8_t *data, InkColor c,
                   int scale = 1);

  // V0.7 friendly stat icons. Kept procedural so they cost almost no flash
  // compared with bitmap assets.
  void icon_wifi(int x, int y, InkColor c);
  void icon_bluetooth(int x, int y, InkColor c);
  void icon_steps(int x, int y, InkColor c);
  void icon_friend(int x, int y, InkColor c);
  void icon_star(int x, int y, InkColor c);
  void icon_badge(int x, int y, InkColor c);

  // V0.7.1 PRETTY PET decorative primitives.
  void icon_heart(int x, int y, InkColor c);
  void icon_house(int x, int y, InkColor c);
  void icon_spark(int x, int y, InkColor c);

  void text(int x, int y, const std::string &s, InkColor c,
            int scale = 1, int spacing = 1);

 private:
  Epaper29Tri &d_;

  static uint8_t glyph_col_(char ch, int col);
  void char_(int x, int y, char ch, InkColor c, int scale);
};
