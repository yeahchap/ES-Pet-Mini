#!/usr/bin/env python3
"""Build ES-Pet Mini 112x80 frame-filling monochrome sprites from 200x200 dithered PNGs."""
from pathlib import Path
from PIL import Image, ImageOps

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "assets" / "pet_storybook_200"
DERIVED = ROOT / "assets" / "pet_storybook_112x80"
OUTPUT = ROOT / "src" / "pet_art.cpp"
MOODS = [
    "home", "curious", "walkies", "snoozing", "perched",
    "adventure", "discovery", "friend", "homecoming", "celebrate",
    "mission_complete", "badge_unlock", "level_up", "factory_reset",
]
W, H = 112, 80
BLACK_THRESHOLD = 170

def prepare(path: Path) -> Image.Image:
    im = Image.open(path).convert("RGB")
    if im.size != (200, 200):
        raise ValueError(f"{path}: expected 200x200 source, got {im.size}")
    # Fill the Mini art panel edge-to-edge. The square master is centre-cropped
    # to the 112:80 display aspect ratio rather than stretched.
    out = ImageOps.fit(im, (W, H), method=Image.Resampling.LANCZOS, centering=(0.5, 0.5))
    px = out.load()
    for y in range(H):
        for x in range(W):
            r, g, b = px[x, y]
            lum = 0.299*r + 0.587*g + 0.114*b
            px[x, y] = (0, 0, 0) if lum < BLACK_THRESHOLD else (255, 255, 255)
    return out

def bits_for(im: Image.Image):
    return [r == 0 and g == 0 and b == 0 for r,g,b in im.getdata()]

def pack(bits):
    out=[]; byte=0; used=0
    for bit in bits:
        byte=(byte<<1)|int(bool(bit)); used+=1
        if used==8:
            out.append(byte); byte=0; used=0
    if used: out.append(byte << (8-used))
    return out

def emit_array(name, data):
    lines=[f"static const uint8_t {name}[] = {{"]
    for i in range(0, len(data), 16):
        lines.append("  " + ", ".join(f"0x{x:02X}" for x in data[i:i+16]) + ",")
    lines.append("};\n")
    return "\n".join(lines)

def main():
    DERIVED.mkdir(parents=True, exist_ok=True)
    prepared={}
    for mood in MOODS:
        im=prepare(SOURCE / f"{mood}.png")
        im.save(DERIVED / f"{mood}.png")
        prepared[mood]=im

    out=[
        '#include "pet_art.h"',
        '#include "gfx.h"',
        '',
        'namespace {',
        f'constexpr int PET_W = {W};',
        f'constexpr int PET_H = {H};',
        '',
    ]
    for mood in MOODS:
        out.append(emit_array(f"k_{mood}_black", pack(bits_for(prepared[mood]))))
    out += [
        'static const uint8_t *sprite_for(PetArtMood mood) {',
        '  switch (mood) {',
    ]
    enums = {
        'home':'Home','curious':'Curious','walkies':'Walkies','snoozing':'Snoozing',
        'perched':'Perched','adventure':'Adventure','discovery':'Discovery','friend':'Friend',
        'homecoming':'Homecoming','celebrate':'Celebrate','mission_complete':'MissionComplete',
        'badge_unlock':'BadgeUnlock','level_up':'LevelUp','factory_reset':'FactoryReset'
    }
    for mood in MOODS:
        out.append(f'    case PetArtMood::{enums[mood]}: return k_{mood}_black;')
    out += [
        '  }',
        '  return k_home_black;',
        '}',
        '}  // namespace',
        '',
        'void draw_pet_art(TinyGfx &g, PetArtMood mood, int x, int y) {',
        '  g.bitmap_1bit(x, y, PET_W, PET_H, sprite_for(mood), InkColor::Black, 1);',
        '}',
        '',
    ]
    OUTPUT.write_text("\n".join(out))
    print(f"Wrote {OUTPUT}")
    print(f"Derived previews: {DERIVED}")

if __name__ == '__main__':
    main()
