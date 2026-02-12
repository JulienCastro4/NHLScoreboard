#include "display/goal_scene.h"

#include <Arduino.h>

#include "display/logo_cache.h"
#include "display/goal_assets.h"

namespace {
    int textWidth(const char* s) {
        if (!s || !s[0]) return 0;
        return (int)strlen(s) * 6;
    }

    int miniTextWidth(const char* s) {
        if (!s || !s[0]) return 0;
        return (int)strlen(s) * 4;
    }

    void drawMiniChar(MatrixPanel_I2S_DMA& display, int x, int y, char c, uint16_t color) {
        const MiniGlyph* g = getMiniGlyph(c);
        for (int row = 0; row < 5; ++row) {
            for (int col = 0; col < 3; ++col) {
                if (g->rows[row] & (1 << (2 - col))) {
                    display.drawPixel(x + col, y + row, color);
                }
            }
        }
    }

    void drawMiniText(MatrixPanel_I2S_DMA& display, int x, int y, const char* text, uint16_t color) {
        if (!text) return;
        int cursor = x;
        for (size_t i = 0; text[i]; ++i) {
            drawMiniChar(display, cursor, y, text[i], color);
            cursor += 4;
        }
    }

    int logoColorCount(const LogoBitmap& logo, uint16_t* out, int maxColors) {
        if (!logo.pixels || logo.width == 0 || logo.height == 0 || !out || maxColors <= 0) return 0;
        const int kMax = 12;
        uint16_t colors[kMax];
        uint16_t counts[kMax];
        int unique = 0;
        const int stepX = 1;
        const int stepY = 1;
        for (int y = 0; y < logo.height; y += stepY) {
            for (int x = 0; x < logo.width; x += stepX) {
                uint16_t c = logo.pixels[y * logo.width + x];
                if (c == 0) continue; // ignore black
                bool found = false;
                for (int i = 0; i < unique; ++i) {
                    if (colors[i] == c) {
                        if (counts[i] < 0xFFFF) counts[i]++;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    if (unique < kMax) {
                        colors[unique] = c;
                        counts[unique] = 1;
                        unique++;
                    }
                }
            }
        }
        if (unique == 0) return 0;
        // sort by count desc
        for (int i = 0; i < unique - 1; ++i) {
            for (int j = i + 1; j < unique; ++j) {
                if (counts[j] > counts[i]) {
                    uint16_t tc = colors[i]; colors[i] = colors[j]; colors[j] = tc;
                    uint16_t tn = counts[i]; counts[i] = counts[j]; counts[j] = tn;
                }
            }
        }
        int take = unique;
        if (take > maxColors) take = maxColors;
        for (int i = 0; i < take; ++i) {
            out[i] = colors[i];
        }
        return take;
    }

    void drawBands(MatrixPanel_I2S_DMA& display,
                   int logoX, int logoY, int logoW, int logoH,
                   uint16_t* colors, int colorCount, uint32_t t,
                   int minY, int maxY) {
        uint16_t fallback = display.color565(255, 255, 255);
        if (!colors || colorCount <= 0) {
            colors = &fallback;
            colorCount = 1;
        }
        const int width = display.width();
        const int height = display.height();
        const int bandWidth = 5;
        const int lx = logoX;
        const int rx = logoX + logoW - 1;
        const int ly = logoY;
        const int ry = logoY + logoH - 1;
        if (minY < 0) minY = 0;
        if (maxY >= height) maxY = height - 1;
        if (minY > maxY) return;

        const int gap = 2;
        int bandCount = (width + gap) / (bandWidth + gap);
        if (bandCount < 1) bandCount = 1;
        if (bandCount > 16) bandCount = 16;
        int totalWidth = bandCount * bandWidth + (bandCount - 1) * gap;
        int startX = (width - totalWidth) / 2;
        if (startX < 0) startX = 0;

        static uint16_t offsets[16];
        static uint8_t speeds[16];
        static int heights[16];
        static int lastBandCount = 0;
        static uint32_t rng = 0x1234567u;
        int range = maxY - minY + 1;
        if (range > 1) range -= 1;
        if (bandCount != lastBandCount) {
            for (int i = 0; i < bandCount; ++i) {
                rng = rng * 1664525u + 1013904223u + (uint32_t)(i * 43 + 17);
                offsets[i] = (uint16_t)(rng % (uint32_t)(range * 2));
                speeds[i] = (uint8_t)(3 + (rng % 5)); // 3..7
                heights[i] = 1;
            }
            lastBandCount = bandCount;
        }
        uint32_t step = t / 140;
        int period = range * 2;

        for (int i = 0; i < bandCount; ++i) {
            uint32_t seed = (uint32_t)(i * 97 + 17);
            int x = startX + i * (bandWidth + gap);
            if (x > width - bandWidth) continue;
            int phase = (int)(((int)step * speeds[i] + offsets[i]) % period);
            int target = (phase <= range) ? phase : (period - phase);
            if (target < 1) target = 1;
            if (target > range) target = range;
            if (heights[i] < target) heights[i] += 1;
            else if (heights[i] > target) heights[i] -= 1;
            int heightPx = heights[i];
            int yBottom = maxY;
            int yTop = yBottom - heightPx + 1;
            if (yTop < minY) yTop = minY;
            if (yBottom > maxY) yBottom = maxY;

            int x1 = x;
            int x2 = x + bandWidth - 1;
            if (x2 < 0 || x1 >= width) continue;
            if (x1 < 0) x1 = 0;
            if (x2 >= width) x2 = width - 1;

            // Skip bands that intersect logo rectangle
            if (!(x2 < lx || x1 > rx || yBottom < ly || yTop > ry)) {
                continue;
            }

            uint16_t col = colors[i % colorCount];
            if (col == 0) col = fallback;
            display.fillRect(x1, yTop, x2 - x1 + 1, yBottom - yTop + 1, col);
        }
    }

    void drawSiren(MatrixPanel_I2S_DMA& display, int x, int y, int w, int h, uint32_t t) {
        uint16_t redBright = display.color565(255, 0, 0);
        uint16_t redDim = display.color565(140, 0, 0);
        uint16_t base = display.color565(140, 140, 140);
        bool bright = ((t / 120) % 2) == 0;
        display.fillRect(x, y + h, w, 2, base);
        display.fillRect(x, y, w, h, bright ? redBright : redDim);
        if ((t / 80) % 2 == 0) {
            int offset = ((t / 160) % 2 == 0) ? -1 : 1;
            int stripeX = x + (w / 2) + offset;
            if (stripeX < x) stripeX = x;
            if (stripeX >= x + w) stripeX = x + w - 1;
            display.drawFastVLine(stripeX, y, h, display.color565(255, 80, 80));
        }
    }

    void drawLogoScaled(MatrixPanel_I2S_DMA& display, const LogoBitmap& logo, int x, int y, int targetSize) {
        if (!logo.pixels || logo.width == 0 || logo.height == 0) return;
        if ((int)logo.width == targetSize && (int)logo.height == targetSize) {
            display.drawRGBBitmap(x, y, logo.pixels, logo.width, logo.height);
            return;
        }
        for (int yy = 0; yy < targetSize; ++yy) {
            int sy = (yy * logo.height) / targetSize;
            for (int xx = 0; xx < targetSize; ++xx) {
                int sx = (xx * logo.width) / targetSize;
                uint16_t col = logo.pixels[sy * logo.width + sx];
                display.drawPixel(x + xx, y + yy, col);
            }
        }
    }

    void drawConfetti(MatrixPanel_I2S_DMA& display, uint32_t t,
                      uint16_t* colors, int colorCount) {
        const int w = display.width();
        const int h = display.height();
        const int numParticles = 18;
        uint16_t fallback = display.color565(255, 255, 255);
        if (!colors || colorCount <= 0) {
            colors = &fallback;
            colorCount = 1;
        }
        uint32_t rng = 0xDEADBEEFu;
        for (int i = 0; i < numParticles; ++i) {
            // Deterministic pseudo-random per particle
            rng = rng * 1664525u + 1013904223u + (uint32_t)(i * 73);
            int baseX = (int)(rng % (uint32_t)w);
            rng = rng * 1664525u + 1013904223u;
            int speed = 8 + (int)(rng % 15); // pixels per second (8..22)
            rng = rng * 1664525u + 1013904223u;
            int startY = -1 - (int)(rng % 10); // start above screen
            int y = startY + (int)((t * speed) / 1000);
            // Wrap around
            int totalTravel = h + 12;
            y = y % totalTravel;
            if (y < 0) y += totalTravel;
            y -= 2; // start slightly above
            if (y < 0 || y >= h) continue;
            // Slight horizontal wobble
            int wobble = (int)((t / 200 + i * 37) % 5) - 2;
            int x = (baseX + wobble) % w;
            if (x < 0) x += w;
            uint16_t col = colors[i % colorCount];
            if (col == 0) col = fallback;
            display.drawPixel(x, y, col);
            // Some particles are 2px for visibility
            if (i % 3 == 0 && x + 1 < w) {
                display.drawPixel(x + 1, y, col);
            }
        }
    }

    void splitName(const char* full, char* first, size_t firstSize, char* last, size_t lastSize) {
        if (!first || !last || firstSize == 0 || lastSize == 0) return;
        first[0] = '\0';
        last[0] = '\0';
        if (!full || !full[0]) return;
        const char* space = strchr(full, ' ');
        if (!space) {
            strncpy(last, full, lastSize - 1);
            last[lastSize - 1] = '\0';
            return;
        }
        size_t firstLen = (size_t)(space - full);
        if (firstLen >= firstSize) firstLen = firstSize - 1;
        memcpy(first, full, firstLen);
        first[firstLen] = '\0';
        const char* lastStart = space + 1;
        strncpy(last, lastStart, lastSize - 1);
        last[lastSize - 1] = '\0';
    }
}

void GoalScene::render(MatrixPanel_I2S_DMA& display, const GameSnapshot& data, uint32_t nowMs) {
    const uint32_t elapsed = nowMs;
    const uint32_t phaseGoal = 5000;
    const uint32_t phaseFlash = 900;
    const uint32_t phaseZoom = 800;
    const uint32_t phaseCenter = 1700;
    const uint32_t phaseName = 8400;
    const uint32_t phaseAssist = 0;

    const uint32_t tGoalEnd = phaseGoal;
    const uint32_t tFlashEnd = tGoalEnd + phaseFlash;
    const uint32_t tZoomEnd = tFlashEnd + phaseZoom;
    const uint32_t tCenterEnd = tZoomEnd + phaseCenter;
    const uint32_t tNameEnd = tCenterEnd + phaseName;
    const uint32_t tAssistEnd = tNameEnd + phaseAssist;

    display.clearScreen();
    display.setTextSize(1);
    display.setTextColor(display.color565(255, 255, 255));

    const char* goalTeamAbbrev = "";
    if (data.goalOwnerTeamId && data.goalOwnerTeamId == data.home.id) {
        goalTeamAbbrev = data.home.abbrev;
    } else if (data.goalOwnerTeamId && data.goalOwnerTeamId == data.away.id) {
        goalTeamAbbrev = data.away.abbrev;
    } else {
        goalTeamAbbrev = data.home.abbrev;
    }

    LogoBitmap logo{};
    const bool hasLogo = goalTeamAbbrev[0] && logoCacheGet(goalTeamAbbrev, logo);

    if (elapsed < tGoalEnd) {
        const char* msg = "GOAL";
        const int letterCount = 4;
        const uint32_t letterDelay = 400;
        const uint32_t revealEnd = letterDelay * letterCount; // 1600ms
        int totalW = textWidth(msg);
        int baseX = (display.width() - totalW) / 2;
        int baseY = (display.height() - 8) / 2;

        // Pulse brightness after reveal
        uint16_t textColor = display.color565(255, 255, 255);
        if (elapsed >= revealEnd) {
            bool bright = ((elapsed / 300) % 2) == 0;
            textColor = bright ? display.color565(255, 255, 255) : display.color565(120, 120, 120);
        }
        display.setTextColor(textColor);

        // Draw each letter with bounce-in
        for (int i = 0; i < letterCount; ++i) {
            uint32_t letterStart = (uint32_t)i * letterDelay;
            if (elapsed < letterStart) continue;
            uint32_t lt = elapsed - letterStart;
            int bounceY = 0;
            if (lt < 100) {
                bounceY = -4 + (int)((lt * 4) / 100);
            } else if (lt < 200) {
                bounceY = (int)(((lt - 100) * 2) / 100);
            } else if (lt < 300) {
                bounceY = 2 - (int)(((lt - 200) * 2) / 100);
            }
            int lx = baseX + i * 6;
            int ly = baseY + bounceY;
            char buf[2] = { msg[i], '\0' };
            display.setCursor(lx, ly);
            display.print(buf);
        }

        int sirenY = baseY - 2;
        drawSiren(display, 4, sirenY, 9, 9, elapsed);
        drawSiren(display, display.width() - 13, sirenY, 9, 9, elapsed);
        return;
    }

    if (elapsed < tFlashEnd && hasLogo) {
        const bool firstOn = ((elapsed / 220) % 2) == 0;
        if (firstOn) {
            int x = display.width() - logo.width;
            if (x < 0) x = 0;
            display.drawRGBBitmap(x, 0, logo.pixels, logo.width, logo.height);
        } else {
            int y = display.height() - logo.height;
            if (y < 0) y = 0;
            display.drawRGBBitmap(0, y, logo.pixels, logo.width, logo.height);
        }
        return;
    }

    if (elapsed < tZoomEnd && hasLogo) {
        // Zoom-in: scale from 4px to 25px (larger than native 20px)
        uint32_t zoomT = elapsed - tFlashEnd;
        const int minSize = 4;
        const int maxSize = 25;
        int size = minSize + (int)(((uint32_t)(maxSize - minSize) * zoomT) / phaseZoom);
        if (size > maxSize) size = maxSize;
        if (size < minSize) size = minSize;
        int x = (display.width() - size) / 2;
        int y = (display.height() - size) / 2;
        drawLogoScaled(display, logo, x, y, size);
        return;
    }

    if (elapsed < tCenterEnd && hasLogo) {
        // Hold at 25x25 zoomed size
        const int size = 25;
        int x = (display.width() - size) / 2;
        int y = (display.height() - size) / 2;
        drawLogoScaled(display, logo, x, y, size);
        return;
    }

    char first[24];
    char last[24];
    splitName(data.goalScorer, first, sizeof(first), last, sizeof(last));
    if (elapsed < tNameEnd) {
        uint32_t t = elapsed - tCenterEnd;
        const uint32_t firstPhase = 1200;
        const uint32_t lastPhase = 1200;
        const uint32_t holdPhase = 5000;
        const int width = display.width();
        const int wFirst = textWidth(first);
        const int wLast = textWidth(last);
        const int yFirst = 1;
        const int yLast = 11;
        uint16_t shadow = display.color565(58, 58, 58);
        uint16_t main = display.color565(150, 150, 150);

        int xFirst = 0;
        if (first[0]) {
            if (t < firstPhase) {
                xFirst = width - (int)((t * (width + wFirst)) / firstPhase);
                if (xFirst < 0) xFirst = 0;
            }
        }

        int xLast = 0; // No offset
        if (last[0]) {
            if (t < firstPhase) {
                xLast = width;
            } else {
                uint32_t tLast = t - firstPhase;
                if (tLast < lastPhase) {
                    xLast = width - (int)((tLast * (width + wLast)) / lastPhase);
                    if (xLast < 0) xLast = 0;
                } else {
                    xLast = 0;
                }
            }
        }

        bool shadowOn = false;
        if (t >= firstPhase + lastPhase) {
            uint32_t tFull = t - (firstPhase + lastPhase);
            auto beatOn = [](uint32_t localMs) {
                // on/off/on/off every 200ms over 800ms
                if (localMs >= 800) return false;
                int phase = (int)(localMs / 200);
                return (phase % 2) == 0;
            };
            if ((tFull < 800 && beatOn(tFull)) ||
                (tFull >= 2000 && tFull < 2800 && beatOn(tFull - 2000))) {
                shadowOn = true;
            }
        }

        uint16_t colors[3];
        int colorCount = getTeamColors(goalTeamAbbrev, colors, 3);
        if (colorCount == 0 && hasLogo) {
            colorCount = logoColorCount(logo, colors, 3);
        }
        // const int bandMinY = max(yLast + 7, display.height() - 10);
        // const int bandMaxY = display.height() - 1;
        // drawBands(display, 0, 0, logo.width, logo.height, colors, colorCount, t, bandMinY, bandMaxY);

        // Confetti particles in team colors
        drawConfetti(display, t, colors, colorCount);

        // Assist names (mini font, below scorer)
        {
            char a1First[24], a1Last[24];
            char a2First[24], a2Last[24];
            splitName(data.goalAssist1, a1First, sizeof(a1First), a1Last, sizeof(a1Last));
            splitName(data.goalAssist2, a2First, sizeof(a2First), a2Last, sizeof(a2Last));
            bool hasA1 = a1Last[0] != '\0';
            bool hasA2 = a2Last[0] != '\0';

            const uint32_t assistStart = firstPhase + lastPhase + 800;
            const uint32_t assistSlide = 800;
            uint16_t assistColor = display.color565(120, 120, 120);

            if (t >= assistStart) {
                uint32_t tA = t - assistStart;
                if (!hasA1 && !hasA2) {
                    // Unassisted
                    const char* uTxt = "UNASSISTED";
                    int wU = miniTextWidth(uTxt);
                    int xU = width;
                    if (tA < assistSlide) {
                        xU = width - (int)((tA * (width + wU)) / assistSlide);
                        if (xU < 0) xU = 0;
                    } else {
                        xU = 0;
                    }
                    drawMiniText(display, xU, 24, uTxt, assistColor);
                } else if (hasA1 && !hasA2) {
                    // Single assist
                    int wA1 = miniTextWidth(a1Last);
                    int xA1 = width;
                    if (tA < assistSlide) {
                        xA1 = width - (int)((tA * (width + wA1)) / assistSlide);
                        if (xA1 < 0) xA1 = 0;
                    } else {
                        xA1 = 0;
                    }
                    drawMiniText(display, xA1, 24, a1Last, assistColor);
                } else {
                    // Two assists — both slide simultaneously
                    int wA1 = miniTextWidth(a1Last);
                    int wA2 = miniTextWidth(a2Last);
                    int xA1 = width;
                    int xA2 = width;
                    if (tA < assistSlide) {
                        xA1 = width - (int)((tA * (width + wA1)) / assistSlide);
                        if (xA1 < 0) xA1 = 0;
                        xA2 = width - (int)((tA * (width + wA2)) / assistSlide);
                        if (xA2 < 0) xA2 = 0;
                    } else {
                        xA1 = 0;
                        xA2 = 0;
                    }
                    drawMiniText(display, xA1, 21, a1Last, assistColor);
                    drawMiniText(display, xA2, 27, a2Last, assistColor);
                }
            }
        }

        if (first[0]) {
            if (shadowOn) {
                display.setTextColor(shadow);
                display.setCursor(xFirst + 1, yFirst + 1);
                display.print(first);
            }
            display.setTextColor(main);
            display.setCursor(xFirst, yFirst);
            display.print(first);
        }
        if (last[0] && t >= firstPhase) {
            if (shadowOn) {
                display.setTextColor(shadow);
                display.setCursor(xLast + 1, yLast + 1);
                display.print(last);
            }
            display.setTextColor(main);
            display.setCursor(xLast, yLast);
            display.print(last);
        }
        return;
    }

    if (elapsed < tAssistEnd) {
        return;
    }
}

