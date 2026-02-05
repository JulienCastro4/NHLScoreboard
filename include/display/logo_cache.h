#pragma once

#include <Arduino.h>

struct LogoBitmap {
    uint16_t* pixels;
    uint8_t width;
    uint8_t height;
};

void logoCacheInit();
bool logoCacheGet(const char* abbrev, LogoBitmap& out);
void logoCacheClear();

