#pragma once

#include <Arduino.h>

struct MiniGlyph {
    char c;
    uint8_t rows[5];
};

const MiniGlyph* getMiniGlyph(char c);
uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b);
int getTeamColors(const char* abbrev, uint16_t* out, int maxColors);
