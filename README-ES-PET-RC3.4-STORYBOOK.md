# ES-Pet V0.8L RC3.4 — Mini Storybook Graphics

This tree keeps the **V0.8L RC3.4 Mini Final** behaviour and persistence model, replacing the character/event pictures with Chris's new dithered monochrome ES-Pet set.

## What changed

- All 14 pet/event scenes use the new black-and-white 200x200 artwork.
- Source artwork is stored in `assets/pet_storybook_200/`.
- `tools/generate_pet_art.py` scales each square scene to an 80x80 centre image inside the existing 108x80 firmware art slot, restores a pure black/white palette, writes previews to `assets/pet_storybook_108x80/`, and regenerates `src/pet_art.cpp`.
- `badge_unlock.png` and `level_up.png` currently share the supplied LVL-UP illustration because the source sheet provides one suitable event image for both firmware states.

## Fonts

The RC3.4 Mini fonts are intentionally retained for the first real e-paper test. The monochrome illustration set is already visually dense, so keeping the known readable type avoids changing two major visual systems at once.

## Compatibility

Internal project names, NVS namespaces, setup flow, missions, XP, FRIENDS and saved state remain unchanged. Normal app-only updating therefore preserves the existing pet.
