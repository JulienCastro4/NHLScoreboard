#include "display/logo_cache.h"

#include <LittleFS.h>
#include <strings.h>


namespace {
    struct LogoEntry {
        char abbrev[4];
        uint16_t* pixels;
        uint8_t width;
        uint8_t height;
    };

    LogoEntry cache[6];
    bool initialized = false;
    uint32_t useCounter = 0;

    struct NegativeEntry {
        char abbrev[4];
        uint32_t retryAfterMs;
    };
    NegativeEntry negative[4];

    void clearEntry(LogoEntry& entry) {
        if (entry.pixels) {
            free(entry.pixels);
        }
        entry.pixels = nullptr;
        entry.abbrev[0] = '\0';
        entry.width = 0;
        entry.height = 0;
    }

    bool sizeFromFile(size_t bytes, uint8_t& sizeOut) {
        if (bytes == 20 * 20 * 2) {
            sizeOut = 20;
            return true;
        }
        if (bytes == 25 * 25 * 2) {
            sizeOut = 25;
            return true;
        }
        return false;
    }

    uint16_t adjustForLowDepth(uint16_t c) {
#if defined(PIXEL_COLOR_DEPTH_BITS) && PIXEL_COLOR_DEPTH_BITS <= 4
        if (c == 0) return 0;
        uint8_t r = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
        uint8_t g = (uint8_t)(((c >> 5) & 0x3F) * 255 / 63);
        uint8_t b = (uint8_t)((c & 0x1F) * 255 / 31);
        uint8_t maxc = max(r, max(g, b));
        uint8_t minc = min(r, min(g, b));

        // Keep near-black and near-white neutral.
        if (maxc < 20) {
            return 0;
        }
        if (minc > 220) {
            return (uint16_t)0xFFFF;
        }

        // Reduce green tint in near-greys (common at low depth).
        if (abs((int)r - (int)b) < 12 && g > r + 8 && g > b + 8) {
            g = max(r, b);
        }

        return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
#else
        return c;
#endif
    }

    bool loadLogoAt(const char* path, LogoEntry& entry) {
        File f = LittleFS.open(path, "r");
        if (!f) return false;

        size_t sizeBytes = f.size();
        uint8_t logoSize = 0;
        if (!sizeFromFile(sizeBytes, logoSize)) {
            f.close();
            return false;
        }

        size_t pixelCount = (size_t)logoSize * (size_t)logoSize;
        uint16_t* data = (uint16_t*)malloc(pixelCount * sizeof(uint16_t));
        if (!data) {
            f.close();
            return false;
        }

        for (size_t i = 0; i < pixelCount; ++i) {
            int lo = f.read();
            int hi = f.read();
            if (lo < 0 || hi < 0) {
                free(data);
                f.close();
                return false;
            }
            uint16_t raw = (uint16_t)((hi << 8) | lo);
            data[i] = adjustForLowDepth(raw);
        }

        f.close();
        clearEntry(entry);
        entry.pixels = data;
        entry.width = logoSize;
        entry.height = logoSize;
        return true;
    }

    bool loadLogo(const char* abbrev, LogoEntry& entry) {
        if (!abbrev || !abbrev[0]) return false;
        char path[32];
        snprintf(path, sizeof(path), "/logos/%s.rgb565", abbrev);
        if (!loadLogoAt(path, entry)) {
            snprintf(path, sizeof(path), "/%s.rgb565", abbrev);
            if (!loadLogoAt(path, entry)) {
                return false;
            }
        }
        strncpy(entry.abbrev, abbrev, sizeof(entry.abbrev) - 1);
        entry.abbrev[sizeof(entry.abbrev) - 1] = '\0';
        return true;
    }

    bool negativeHit(const char* abbrev) {
        const uint32_t now = millis();
        for (auto& entry : negative) {
            if (entry.abbrev[0] && strcasecmp(entry.abbrev, abbrev) == 0) {
                return now < entry.retryAfterMs;
            }
        }
        return false;
    }

    void negativeRemember(const char* abbrev, uint32_t retryDelayMs) {
        uint32_t now = millis();
        for (auto& entry : negative) {
            if (entry.abbrev[0] && strcasecmp(entry.abbrev, abbrev) == 0) {
                entry.retryAfterMs = now + retryDelayMs;
                return;
            }
        }
        for (auto& entry : negative) {
            if (!entry.abbrev[0]) {
                strncpy(entry.abbrev, abbrev, sizeof(entry.abbrev) - 1);
                entry.abbrev[sizeof(entry.abbrev) - 1] = '\0';
                entry.retryAfterMs = now + retryDelayMs;
                return;
            }
        }
        strncpy(negative[0].abbrev, abbrev, sizeof(negative[0].abbrev) - 1);
        negative[0].abbrev[sizeof(negative[0].abbrev) - 1] = '\0';
        negative[0].retryAfterMs = now + retryDelayMs;
    }
}

void logoCacheInit() {
    if (initialized) return;
    for (auto& entry : cache) {
        entry.pixels = nullptr;
        entry.abbrev[0] = '\0';
        entry.width = 0;
        entry.height = 0;
    }
    for (auto& entry : negative) {
        entry.abbrev[0] = '\0';
        entry.retryAfterMs = 0;
    }
    initialized = true;
}

void logoCacheClear() {
    for (auto& entry : cache) {
        clearEntry(entry);
    }
    for (auto& entry : negative) {
        entry.abbrev[0] = '\0';
        entry.retryAfterMs = 0;
    }
}

bool logoCacheGet(const char* abbrev, LogoBitmap& out) {
    if (!initialized) logoCacheInit();
    useCounter++;
    for (auto& entry : cache) {
        if (entry.pixels && strcasecmp(entry.abbrev, abbrev) == 0) {
            out.pixels = entry.pixels;
            out.width = entry.width;
            out.height = entry.height;
            return true;
        }
    }

    if (negativeHit(abbrev)) {
        return false;
    }

    LogoEntry* target = nullptr;
    for (auto& entry : cache) {
        if (!entry.pixels) {
            target = &entry;
            break;
        }
    }
    if (!target) {
        target = &cache[0];
    }

    if (!loadLogo(abbrev, *target)) {
        negativeRemember(abbrev, 3000);
        return false;
    }

    out.pixels = target->pixels;
    out.width = target->width;
    out.height = target->height;
    return true;
}

