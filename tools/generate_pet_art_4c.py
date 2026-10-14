#!/usr/bin/env python3
"""Build ES-PET Mini 112x80 four-colour sprites from 200x200 PNG masters.

Accepted panel palette: black, white, red, yellow.

The 200x200 source masters are deliberately allowed to contain ordinary RGB
colour because the concept art is easier to edit that way.  The final 112x80
sprite is mapped to the real four-pigment e-paper palette here:

* strong reds/oranges -> panel red
* yellows/golds      -> panel yellow
* everything else   -> ordered black/white dither based on luminance

The emitted C++ keeps three independent 1-bit masks so the existing TinyGfx
bitmap primitive can draw each pigment without changing any UI geometry.
"""
from pathlib import Path
from PIL import Image, ImageOps
import colorsys

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "assets" / "pet_storybook_200_4c"
DERIVED = ROOT / "assets" / "pet_storybook_112x80_4c"
OUTPUT = ROOT / "src" / "pet_art.cpp"
MOODS = [
    "home", "curious", "walkies", "snoozing", "perched",
    "adventure", "discovery", "friend", "homecoming", "celebrate",
    "mission_complete", "badge_unlock", "level_up", "factory_reset",
]
W, H = 112, 80

PALETTE = {
    "black": (0, 0, 0),
    "white": (255, 255, 255),
    "red": (220, 0, 0),
    "yellow": (255, 220, 0),
}

# 4x4 Bayer matrix.  Mid-tone greys become a predictable stipple instead of
# large muddy blocks.  This is friendlier to the tiny e-paper than continuous
# grey from the concept artwork.
BAYER4 = (
    (0, 8, 2, 10),
    (12, 4, 14, 6),
    (3, 11, 1, 9),
    (15, 7, 13, 5),
)


def pigment_for(rgb, x: int, y: int) -> str:
    r8, g8, b8 = rgb
    r, g, b = r8 / 255.0, g8 / 255.0, b8 / 255.0
    h, s, v = colorsys.rgb_to_hsv(r, g, b)
    hue = h * 360.0
    luma = 0.2126 * r + 0.7152 * g + 0.0722 * b

    # Preserve deliberate warm accent colours from the concept sheet.
    if (((hue < 28.0) or (hue > 340.0)) and
            s > 0.38 and v > 0.25 and r > 0.45 and luma > 0.18):
        return "red"

    if (28.0 <= hue < 72.0 and
            s > 0.28 and v > 0.42 and r > 0.50 and g > 0.35 and luma > 0.35):
        return "yellow"

    # Strong black/white anchors stay clean; mid-tones are ordered-dithered.
    if luma < 0.25:
        return "black"
    if luma > 0.88:
        return "white"

    bayer = BAYER4[y & 3][x & 3] / 16.0
    threshold = 0.46 + (bayer - 0.5) * 0.38
    return "white" if luma > threshold else "black"


def prepare(path: Path) -> Image.Image:
    im = Image.open(path).convert("RGB")
    if im.size != (200, 200):
        raise ValueError(f"{path}: expected 200x200 source, got {im.size}")

    # Same storybook crop as the monochrome Mini: a 200x200 source becomes the
    # exact 112x80 art-panel interior.  The source masters intentionally keep
    # the important composition in the central 200x143 region.
    out = ImageOps.fit(
        im, (W, H), method=Image.Resampling.LANCZOS, centering=(0.5, 0.5)
    )

    mapped = Image.new("RGB", (W, H), PALETTE["white"])
    src = out.load()
    dst = mapped.load()
    for y in range(H):
        for x in range(W):
            dst[x, y] = PALETTE[pigment_for(src[x, y], x, y)]
    return mapped


def bits_for(im: Image.Image, pigment: str):
    target = PALETTE[pigment]
    return [pix == target for pix in im.getdata()]


def pack(bits):
    out = []
    byte = 0
    used = 0
    for bit in bits:
        byte = (byte << 1) | int(bool(bit))
        used += 1
        if used == 8:
            out.append(byte)
            byte = 0
            used = 0
    if used:
        out.append(byte << (8 - used))
    return out


def emit_array(name, data):
    lines = [f"static const uint8_t {name}[] = {{"]
    for i in range(0, len(data), 16):
        lines.append("  " + ", ".join(f"0x{x:02X}" for x in data[i:i + 16]) + ",")
    lines.append("};\n")
    return "\n".join(lines)


def main():
    DERIVED.mkdir(parents=True, exist_ok=True)
    prepared = {}
    for mood in MOODS:
        source = SOURCE / f"{mood}.png"
        if not source.exists():
            raise FileNotFoundError(f"Missing four-colour source art: {source}")
        im = prepare(source)
        im.save(DERIVED / f"{mood}.png")
        prepared[mood] = im

    out = [
        '#include "pet_art.h"',
        '#include "gfx.h"',
        '',
        'namespace {',
        f'constexpr int PET_W = {W};',
        f'constexpr int PET_H = {H};',
        '',
    ]
    for mood in MOODS:
        for pigment in ("black", "yellow", "red"):
            out.append(emit_array(
                f"k_{mood}_{pigment}",
                pack(bits_for(prepared[mood], pigment))))

    enums = {
        'home': 'Home', 'curious': 'Curious', 'walkies': 'Walkies',
        'snoozing': 'Snoozing', 'perched': 'Perched',
        'adventure': 'Adventure', 'discovery': 'Discovery', 'friend': 'Friend',
        'homecoming': 'Homecoming', 'celebrate': 'Celebrate',
        'mission_complete': 'MissionComplete', 'badge_unlock': 'BadgeUnlock',
        'level_up': 'LevelUp', 'factory_reset': 'FactoryReset'
    }
    out += [
        'struct Sprite4C {',
        '  const uint8_t *black;',
        '  const uint8_t *yellow;',
        '  const uint8_t *red;',
        '};',
        '',
        'static Sprite4C sprite_for(PetArtMood mood) {',
        '  switch (mood) {',
    ]
    for mood in MOODS:
        out.append(
            f'    case PetArtMood::{enums[mood]}: '
            f'return {{k_{mood}_black, k_{mood}_yellow, k_{mood}_red}};')
    out += [
        '  }',
        '  return {k_home_black, k_home_yellow, k_home_red};',
        '}',
        '}  // namespace',
        '',
        'void draw_pet_art(TinyGfx &g, PetArtMood mood, int x, int y) {',
        '  const Sprite4C s = sprite_for(mood);',
        '  g.bitmap_1bit(x, y, PET_W, PET_H, s.black, InkColor::Black, 1);',
        '  g.bitmap_1bit(x, y, PET_W, PET_H, s.yellow, InkColor::Yellow, 1);',
        '  g.bitmap_1bit(x, y, PET_W, PET_H, s.red, InkColor::Red, 1);',
        '}',
        '',
    ]
    OUTPUT.write_text("\n".join(out))
    print(f"Wrote {OUTPUT}")
    print(f"Derived four-colour previews: {DERIVED}")

    for mood in MOODS:
        im = prepared[mood]
        counts = {name: 0 for name in PALETTE}
        for pix in im.getdata():
            for name, rgb in PALETTE.items():
                if pix == rgb:
                    counts[name] += 1
                    break
        print(
            f"{mood:16s} "
            f"B={counts['black']:4d} W={counts['white']:4d} "
            f"Y={counts['yellow']:4d} R={counts['red']:4d}"
        )


if __name__ == '__main__':
    main()
