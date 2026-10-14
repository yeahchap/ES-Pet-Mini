#include "gfx.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cmath>

TinyGfx::TinyGfx(Epaper29Tri &display) : d_(display) {}

void TinyGfx::pixel(int x, int y, InkColor c) {
  d_.set_pixel(x, y, c);
}

void TinyGfx::line(int x0, int y0, int x1, int y1, InkColor c) {
  int dx = std::abs(x1 - x0);
  int sx = x0 < x1 ? 1 : -1;
  int dy = -std::abs(y1 - y0);
  int sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;

  while (true) {
    pixel(x0, y0, c);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

void TinyGfx::rect(int x, int y, int w, int h, InkColor c) {
  if (w <= 0 || h <= 0) return;
  line(x, y, x + w - 1, y, c);
  line(x, y + h - 1, x + w - 1, y + h - 1, c);
  line(x, y, x, y + h - 1, c);
  line(x + w - 1, y, x + w - 1, y + h - 1, c);
}

void TinyGfx::fill_rect(int x, int y, int w, int h, InkColor c) {
  for (int yy = 0; yy < h; ++yy) {
    line(x, y + yy, x + w - 1, y + yy, c);
  }
}

void TinyGfx::circle(int cx, int cy, int r, InkColor c) {
  int x = r;
  int y = 0;
  int err = 0;

  while (x >= y) {
    pixel(cx + x, cy + y, c);
    pixel(cx + y, cy + x, c);
    pixel(cx - y, cy + x, c);
    pixel(cx - x, cy + y, c);
    pixel(cx - x, cy - y, c);
    pixel(cx - y, cy - x, c);
    pixel(cx + y, cy - x, c);
    pixel(cx + x, cy - y, c);

    ++y;
    if (err <= 0) {
      err += 2 * y + 1;
    }
    if (err > 0) {
      --x;
      err -= 2 * x + 1;
    }
  }
}

void TinyGfx::fill_circle(int cx, int cy, int r, InkColor c) {
  for (int y = -r; y <= r; ++y) {
    int x = static_cast<int>(std::sqrt(
        static_cast<double>(r * r - y * y)));
    line(cx - x, cy + y, cx + x, cy + y, c);
  }
}

void TinyGfx::bitmap_1bit(int x, int y, int w, int h,
                          const uint8_t *data, InkColor c,
                          int scale) {
  if (!data || w <= 0 || h <= 0) return;
  if (scale < 1) scale = 1;

  const int total_bits = w * h;
  for (int bit = 0; bit < total_bits; ++bit) {
    const uint8_t byte = data[bit >> 3];
    const uint8_t mask = static_cast<uint8_t>(0x80u >> (bit & 7));
    if ((byte & mask) == 0) continue;

    const int sx = bit % w;
    const int sy = bit / w;
    if (scale == 1) pixel(x + sx, y + sy, c);
    else fill_rect(x + sx * scale, y + sy * scale, scale, scale, c);
  }
}


void TinyGfx::icon_wifi(int x, int y, InkColor c) {
  // Familiar compact Wi-Fi fan, 11x9.
  line(x,      y + 2, x + 5, y,     c);
  line(x + 5,  y,     x + 10, y + 2, c);

  line(x + 2,  y + 5, x + 5, y + 3, c);
  line(x + 5,  y + 3, x + 8, y + 5, c);

  line(x + 4,  y + 7, x + 5, y + 6, c);
  line(x + 5,  y + 6, x + 6, y + 7, c);

  fill_circle(x + 5, y + 8, 1, c);
}

void TinyGfx::icon_bluetooth(int x, int y, InkColor c) {
  // Cleaner, slightly taller Bluetooth rune, 9x12.
  line(x + 4, y,      x + 4, y + 11, c);
  line(x + 4, y,      x + 8, y + 3,  c);
  line(x + 8, y + 3,  x + 4, y + 6,  c);
  line(x + 4, y + 6,  x + 8, y + 9,  c);
  line(x + 8, y + 9,  x + 4, y + 11, c);

  line(x,     y + 2,  x + 8, y + 9,  c);
  line(x,     y + 9,  x + 8, y + 3,  c);
}

void TinyGfx::icon_steps(int x, int y, InkColor c) {
  // Two recognisable little footprints rather than the V0.7 diamonds.
  // Left sole + toes.
  fill_circle(x + 3, y + 7, 2, c);
  line(x + 2, y + 5, x + 3, y + 2, c);
  pixel(x + 1, y + 1, c);
  pixel(x + 3, y,     c);
  pixel(x + 5, y + 1, c);

  // Right sole, offset forward, + toes.
  fill_circle(x + 10, y + 4, 2, c);
  line(x + 10, y + 3, x + 9, y, c);
  pixel(x + 7,  y,     c);
  pixel(x + 9,  y - 1, c);
  pixel(x + 11, y,     c);
}

void TinyGfx::icon_friend(int x, int y, InkColor c) {
  // Two tiny buddies side by side.
  circle(x + 3, y + 4, 3, c);
  circle(x + 9, y + 4, 3, c);

  pixel(x + 2, y + 3, c);
  pixel(x + 4, y + 3, c);
  pixel(x + 8, y + 3, c);
  pixel(x + 10, y + 3, c);

  line(x + 2, y + 5, x + 3, y + 6, c);
  line(x + 3, y + 6, x + 4, y + 5, c);
  line(x + 8, y + 5, x + 9, y + 6, c);
  line(x + 9, y + 6, x + 10, y + 5, c);
}

void TinyGfx::icon_star(int x, int y, InkColor c) {
  // Chunkier XP sparkle.
  line(x + 5, y,      x + 5, y + 10, c);
  line(x,     y + 5,  x + 10, y + 5, c);
  line(x + 2, y + 2,  x + 8, y + 8,  c);
  line(x + 8, y + 2,  x + 2, y + 8,  c);
  fill_circle(x + 5, y + 5, 1, c);
}

void TinyGfx::icon_badge(int x, int y, InkColor c) {
  // Medal + ribbon tails.
  circle(x + 5, y + 4, 4, c);
  fill_circle(x + 5, y + 4, 1, c);

  line(x + 2, y + 7,  x + 2, y + 12, c);
  line(x + 2, y + 12, x + 5, y + 9,  c);

  line(x + 8, y + 7,  x + 8, y + 12, c);
  line(x + 8, y + 12, x + 5, y + 9,  c);
}

void TinyGfx::icon_heart(int x, int y, InkColor c) {
  fill_circle(x + 3, y + 3, 2, c);
  fill_circle(x + 7, y + 3, 2, c);
  line(x + 1, y + 4, x + 5, y + 9, c);
  line(x + 9, y + 4, x + 5, y + 9, c);
  line(x + 2, y + 5, x + 8, y + 5, c);
  line(x + 3, y + 6, x + 7, y + 6, c);
  line(x + 4, y + 7, x + 6, y + 7, c);
  pixel(x + 5, y + 8, c);
}

void TinyGfx::icon_house(int x, int y, InkColor c) {
  line(x,      y + 5, x + 6,  y,     c);
  line(x + 6,  y,     x + 12, y + 5, c);
  rect(x + 2,  y + 5, 9,      7,     c);
  rect(x + 6,  y + 8, 3,      4,     c);
}

void TinyGfx::icon_spark(int x, int y, InkColor c) {
  line(x + 3, y,     x + 3, y + 6, c);
  line(x,     y + 3, x + 6, y + 3, c);
  pixel(x + 1, y + 1, c);
  pixel(x + 5, y + 1, c);
  pixel(x + 1, y + 5, c);
  pixel(x + 5, y + 5, c);
}

// Compact home-grown 5x7 font. Each return value is one vertical column,
// bit 0 = top pixel, bit 6 = bottom pixel.
uint8_t TinyGfx::glyph_col_(char ch, int col) {
  if (col < 0 || col > 4) return 0;

  // 5 bytes per glyph, A-Z / 0-9 / selected punctuation.
  // The patterns are intentionally simple and readable on e-paper.
#define G(a,b,c,d,e) do { static const uint8_t v[5]={a,b,c,d,e}; return v[col]; } while(0)

  switch (ch) {
    case 'A': G(0x7E,0x09,0x09,0x09,0x7E);
    case 'B': G(0x7F,0x49,0x49,0x49,0x36);
    case 'C': G(0x3E,0x41,0x41,0x41,0x22);
    case 'D': G(0x7F,0x41,0x41,0x22,0x1C);
    case 'E': G(0x7F,0x49,0x49,0x49,0x41);
    case 'F': G(0x7F,0x09,0x09,0x09,0x01);
    case 'G': G(0x3E,0x41,0x49,0x49,0x7A);
    case 'H': G(0x7F,0x08,0x08,0x08,0x7F);
    case 'I': G(0x00,0x41,0x7F,0x41,0x00);
    case 'J': G(0x20,0x40,0x41,0x3F,0x01);
    case 'K': G(0x7F,0x08,0x14,0x22,0x41);
    case 'L': G(0x7F,0x40,0x40,0x40,0x40);
    case 'M': G(0x7F,0x02,0x0C,0x02,0x7F);
    case 'N': G(0x7F,0x04,0x08,0x10,0x7F);
    case 'O': G(0x3E,0x41,0x41,0x41,0x3E);
    case 'P': G(0x7F,0x09,0x09,0x09,0x06);
    case 'Q': G(0x3E,0x41,0x51,0x21,0x5E);
    case 'R': G(0x7F,0x09,0x19,0x29,0x46);
    case 'S': G(0x46,0x49,0x49,0x49,0x31);
    case 'T': G(0x01,0x01,0x7F,0x01,0x01);
    case 'U': G(0x3F,0x40,0x40,0x40,0x3F);
    case 'V': G(0x1F,0x20,0x40,0x20,0x1F);
    case 'W': G(0x3F,0x40,0x38,0x40,0x3F);
    case 'X': G(0x63,0x14,0x08,0x14,0x63);
    case 'Y': G(0x07,0x08,0x70,0x08,0x07);
    case 'Z': G(0x61,0x51,0x49,0x45,0x43);

    // Lowercase is intentionally supported from V0.6.2 onward. The setup
    // Wi-Fi password is case-sensitive, so rendering it as uppercase was a
    // genuine usability bug rather than just a style choice.
    case 'a': G(0x20,0x54,0x54,0x54,0x78);
    case 'b': G(0x7F,0x48,0x44,0x44,0x38);
    case 'c': G(0x38,0x44,0x44,0x44,0x20);
    case 'd': G(0x38,0x44,0x44,0x48,0x7F);
    case 'e': G(0x38,0x54,0x54,0x54,0x18);
    case 'f': G(0x08,0x7E,0x09,0x01,0x02);
    case 'g': G(0x0C,0x52,0x52,0x52,0x3E);
    case 'h': G(0x7F,0x08,0x04,0x04,0x78);
    case 'i': G(0x00,0x44,0x7D,0x40,0x00);
    case 'j': G(0x20,0x40,0x44,0x3D,0x00);
    case 'k': G(0x7F,0x10,0x28,0x44,0x00);
    case 'l': G(0x00,0x41,0x7F,0x40,0x00);
    case 'm': G(0x7C,0x04,0x18,0x04,0x78);
    case 'n': G(0x7C,0x08,0x04,0x04,0x78);
    case 'o': G(0x38,0x44,0x44,0x44,0x38);
    case 'p': G(0x7C,0x14,0x14,0x14,0x08);
    case 'q': G(0x08,0x14,0x14,0x18,0x7C);
    case 'r': G(0x7C,0x08,0x04,0x04,0x08);
    case 's': G(0x48,0x54,0x54,0x54,0x20);
    case 't': G(0x04,0x3F,0x44,0x40,0x20);
    case 'u': G(0x3C,0x40,0x40,0x20,0x7C);
    case 'v': G(0x1C,0x20,0x40,0x20,0x1C);
    case 'w': G(0x3C,0x40,0x30,0x40,0x3C);
    case 'x': G(0x44,0x28,0x10,0x28,0x44);
    case 'y': G(0x0C,0x50,0x50,0x50,0x3C);
    case 'z': G(0x44,0x64,0x54,0x4C,0x44);

    case '0': G(0x3E,0x51,0x49,0x45,0x3E);
    case '1': G(0x00,0x42,0x7F,0x40,0x00);
    case '2': G(0x62,0x51,0x49,0x49,0x46);
    case '3': G(0x22,0x41,0x49,0x49,0x36);
    case '4': G(0x18,0x14,0x12,0x7F,0x10);
    case '5': G(0x2F,0x49,0x49,0x49,0x31);
    case '6': G(0x3E,0x49,0x49,0x49,0x32);
    case '7': G(0x01,0x71,0x09,0x05,0x03);
    case '8': G(0x36,0x49,0x49,0x49,0x36);
    case '9': G(0x26,0x49,0x49,0x49,0x3E);

    case '-': G(0x08,0x08,0x08,0x08,0x08);
    case '.': G(0x00,0x60,0x60,0x00,0x00);
    case ':': G(0x00,0x36,0x36,0x00,0x00);
    case '/': G(0x20,0x10,0x08,0x04,0x02);
    case '+': G(0x08,0x08,0x3E,0x08,0x08);
    case '!': G(0x00,0x00,0x5F,0x00,0x00);
    case '?': G(0x02,0x01,0x51,0x09,0x06);
    case '\'': G(0x00,0x00,0x07,0x03,0x00);
    case '%': G(0x23,0x13,0x08,0x64,0x62);
    case ' ': G(0,0,0,0,0);
    default:  G(0x7F,0x41,0x5D,0x41,0x7F);
  }

#undef G
}

void TinyGfx::char_(int x, int y, char ch, InkColor c, int scale) {
  if (scale < 1) scale = 1;

  for (int col = 0; col < 5; ++col) {
    uint8_t bits = glyph_col_(ch, col);
    for (int row = 0; row < 7; ++row) {
      if (bits & (1u << row)) {
        if (scale == 1) {
          pixel(x + col, y + row, c);
        } else {
          fill_rect(x + col * scale, y + row * scale,
                    scale, scale, c);
        }
      }
    }
  }
}

void TinyGfx::text(int x, int y, const std::string &s, InkColor c,
                   int scale, int spacing) {
  if (scale < 1) scale = 1;
  int cursor = x;
  const int advance = 5 * scale + spacing;

  for (char ch : s) {
    char_(cursor, y, ch, c, scale);
    cursor += advance;
  }
}
