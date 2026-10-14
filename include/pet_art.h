#pragma once

#include <cstdint>

class TinyGfx;

enum class PetArtMood : uint8_t {
  Home, Curious, Walkies, Snoozing, Perched,
  Adventure, Discovery, Friend, Homecoming, Celebrate,
  MissionComplete, BadgeUnlock, LevelUp, FactoryReset,
};

// V0.8 RC3.4 storybook refresh: 200x200 source scenes are centre-cropped to the exact 112x80 Mini art-panel interior.
void draw_pet_art(TinyGfx &g, PetArtMood mood,
                  int x = 6, int y = 24);
