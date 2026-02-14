#include "display/recap_scene.h"

#include <Arduino.h>
#include <string.h>

#include "display/goal_assets.h"
#include "display/logo_cache.h"

namespace {
    constexpr uint32_t PAGE_MS = 6500;
    constexpr uint32_t TRANSITION_MS = 350;
    constexpr uint32_t TITLE_MS = 3000;
    constexpr int MAX_LINE_CHARS = 16;
    constexpr int MAX_STD_CHARS = 10;

    uint32_t hashRecap(const GameSnapshot& data) {
        uint32_t h = 2166136261u;
        h ^= data.gameId; h *= 16777619u;
        h ^= data.away.score; h *= 16777619u;
        h ^= data.home.score; h *= 16777619u;
        h ^= data.away.sog; h *= 16777619u;
        h ^= data.home.sog; h *= 16777619u;
        h ^= data.recapGoalCount; h *= 16777619u;
        for (uint8_t i = 0; i < data.recapGoalCount; ++i) {
            h ^= data.recapGoals[i].eventId; h *= 16777619u;
            h ^= data.recapGoals[i].period; h *= 16777619u;
        }
        return h;
    }

    int miniTextWidth(const char* s) {
        if (!s || !s[0]) return 0;
        return (int)strlen(s) * 4;
    }

    int stdTextWidth(const char* s) {
        if (!s || !s[0]) return 0;
        return (int)strlen(s) * 6;
    }

    void drawMiniChar(MatrixPanel_I2S_DMA& display, int x, int y, char c, uint16_t color) {
        const MiniGlyph* g = getMiniGlyph(c);
        const int w = display.width();
        const int h = display.height();
        for (int row = 0; row < 5; ++row) {
            int py = y + row;
            if (py < 0 || py >= h) continue;
            for (int col = 0; col < 3; ++col) {
                if (g->rows[row] & (1 << (2 - col))) {
                    int px = x + col;
                    if (px < 0 || px >= w) continue;
                    display.drawPixel(px, py, color);
                }
            }
        }
    }

    void drawMiniText(MatrixPanel_I2S_DMA& display, int x, int y, const char* text, uint16_t color) {
        if (!text || !text[0]) return;
        int cursor = x;
        for (size_t i = 0; text[i]; ++i) {
            drawMiniChar(display, cursor, y, text[i], color);
            cursor += 4;
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

    void drawLogoWithAbbrev(MatrixPanel_I2S_DMA& display, const LogoBitmap& logo,
        const char* abbrev, int x, int y, int size, uint16_t color) {
        drawLogoScaled(display, logo, x, y, size);
        if (!abbrev || !abbrev[0]) return;
        int textW = miniTextWidth(abbrev);
        int textX = x + (size - textW) / 2 + 1;
        int textY = y + size;
        drawMiniText(display, textX, textY, abbrev, color);
    }

    void clampLine(const char* src, char* out, size_t outSize) {
        if (!out || outSize == 0) return;
        out[0] = '\0';
        if (!src || !src[0]) return;
        size_t len = strlen(src);
        if (len > MAX_LINE_CHARS) len = MAX_LINE_CHARS;
        if (len >= outSize) len = outSize - 1;
        strncpy(out, src, len);
        out[len] = '\0';
    }

    void clampStdLine(const char* src, char* out, size_t outSize) {
        if (!out || outSize == 0) return;
        out[0] = '\0';
        if (!src || !src[0]) return;
        size_t len = strlen(src);
        if (len > MAX_STD_CHARS) len = MAX_STD_CHARS;
        if (len >= outSize) len = outSize - 1;
        strncpy(out, src, len);
        out[len] = '\0';
    }

    void drawTitleStd(MatrixPanel_I2S_DMA& display, const char* line1, const char* line2, int xOffset) {
        const int w = display.width();
        const uint16_t white = display.color565(255, 255, 255);
        const int lineHeight = 8;
        const int gap = line2 && line2[0] ? 2 : 0;
        const int totalHeight = lineHeight + (line2 && line2[0] ? lineHeight + gap : 0);
        int y = (display.height() - totalHeight) / 2;

        char c1[16];
        char c2[16];
        clampStdLine(line1, c1, sizeof(c1));
        clampStdLine(line2, c2, sizeof(c2));

        int w1 = stdTextWidth(c1);
        int x1 = (w - w1) / 2 + xOffset;
        display.setTextSize(1);
        display.setTextColor(white);
        display.setCursor(x1, y);
        display.print(c1);

        if (c2[0]) {
            int w2 = stdTextWidth(c2);
            int x2 = (w - w2) / 2 + xOffset;
            display.setCursor(x2, y + lineHeight + gap);
            display.print(c2);
        }
    }

    void buildAssistLine(const char* tag, const char* name, char* out, size_t outSize) {
        out[0] = '\0';
        if (name && name[0]) {
            strncat(out, tag, outSize - 1 - strlen(out));
            strncat(out, " ", outSize - 1 - strlen(out));
            strncat(out, name, outSize - 1 - strlen(out));
        } else {
            strncat(out, tag, outSize - 1 - strlen(out));
            strncat(out, " -", outSize - 1 - strlen(out));
        }
    }

    bool parseTimeRemaining(const char* timeStr, int& outSeconds) {
        outSeconds = 0;
        if (!timeStr || !timeStr[0]) return false;
        int mm = 0;
        int ss = 0;
        if (sscanf(timeStr, "%d:%d", &mm, &ss) != 2) return false;
        if (mm < 0 || ss < 0 || ss > 59) return false;
        outSeconds = mm * 60 + ss;
        return true;
    }

    void formatElapsedFromRemaining(const char* timeRemaining, char* out, size_t outSize) {
        if (!out || outSize == 0) return;
        out[0] = '\0';
        int remaining = 0;
        if (!parseTimeRemaining(timeRemaining, remaining)) {
            strncpy(out, "??:??", outSize - 1);
            out[outSize - 1] = '\0';
            return;
        }
        int elapsed = 20 * 60 - remaining;
        if (elapsed < 0) elapsed = 0;
        int mm = elapsed / 60;
        int ss = elapsed % 60;
        snprintf(out, outSize, "%d:%02d", mm, ss);
    }

    uint32_t pageDurationMs(RecapScene::PageType type) {
        switch (type) {
            case RecapScene::PageType::TitleIntro:
            case RecapScene::PageType::TitleFinal:
            case RecapScene::PageType::TitleSog:
            case RecapScene::PageType::TitleGoals:
            case RecapScene::PageType::TeamGoalsTitle:
                return TITLE_MS;
            default:
                return PAGE_MS;
        }
    }

}

void RecapScene::clearPages() {
    pageCount = 0;
    cachedPageCount = 0;
    for (int i = 0; i < kMaxPages; ++i) {
        pages[i].type = PageType::TitleFinal;
        pages[i].teamIndex = 0;
        pages[i].goalIndex = 0;
    }
}

void RecapScene::rebuildPages(const GameSnapshot& data, uint32_t nowMs) {
    clearPages();
    lastTextHash = hashRecap(data);
    startMs = nowMs;

    auto pushPage = [&](PageType type, uint8_t teamIndex, uint8_t goalIndex) {
        if (pageCount >= kMaxPages) return;
        pages[pageCount].type = type;
        pages[pageCount].teamIndex = teamIndex;
        pages[pageCount].goalIndex = goalIndex;
        pageCount++;
    };

    pushPage(PageType::TitleIntro, 0, 0);
    pushPage(PageType::TitleFinal, 0, 0);
    pushPage(PageType::Score, 0, 0);
    pushPage(PageType::TitleSog, 0, 0);
    pushPage(PageType::Sog, 0, 0);
    pushPage(PageType::TitleGoals, 0, 0);

    uint8_t awayGoals[kMaxRecapGoals] = {};
    uint8_t homeGoals[kMaxRecapGoals] = {};
    uint8_t awayCount = 0;
    uint8_t homeCount = 0;

    for (uint8_t i = 0; i < data.recapGoalCount; ++i) {
        const RecapGoal& g = data.recapGoals[i];
        if (strcasecmp(g.teamAbbrev, data.away.abbrev) == 0) {
            awayGoals[awayCount++] = i;
        } else {
            homeGoals[homeCount++] = i;
        }
    }

    uint8_t teamOrder[2] = {0, 1};
    if (data.home.score > data.away.score) {
        teamOrder[0] = 1;
        teamOrder[1] = 0;
    }

    for (uint8_t ti = 0; ti < 2; ++ti) {
        uint8_t teamIndex = teamOrder[ti];
        uint8_t count = (teamIndex == 0) ? awayCount : homeCount;
        if (count == 0) continue;
        pushPage(PageType::TeamGoalsTitle, teamIndex, 0);
        for (uint8_t gi = 0; gi < count; ++gi) {
            uint8_t goalIdx = (teamIndex == 0) ? awayGoals[gi] : homeGoals[gi];
            pushPage(PageType::GoalDetail, teamIndex, goalIdx);
        }
    }

    cachedPageCount = pageCount;
}

void RecapScene::start(uint32_t nowMs, const GameSnapshot& data) {
    if (!data.recapReady) {
        clearPages();
        return;
    }
    rebuildPages(data, nowMs);
    lastPageIndex = -1;
    previousPageIndex = -1;
    transitionStartMs = nowMs;
}

uint32_t RecapScene::totalContentDurationMs(const Page* pageList, int pageCount) {
    uint32_t total = 0;
    for (int i = 0; i < pageCount; ++i) {
        total += pageDurationMs(pageList[i].type);
    }
    return total;
}

bool RecapScene::isComplete(uint32_t nowMs) const {
    if (cachedPageCount <= 0) return true;
    const uint32_t elapsed = nowMs - startMs;
    const uint32_t total = totalContentDurationMs(pages, cachedPageCount);
    return elapsed >= (total + TRANSITION_MS);
}

void RecapScene::render(MatrixPanel_I2S_DMA& display, const GameSnapshot& data, uint32_t nowMs) {
    display.clearScreen();
    if (!data.recapReady) return;

    uint32_t h = hashRecap(data);
    if (h != lastTextHash) {
        rebuildPages(data, nowMs);
    }

    if (pageCount <= 0) return;
    const uint32_t elapsed = nowMs - startMs;
    const uint32_t contentTotal = totalContentDurationMs(pages, pageCount);
    int pageIndex = pageCount - 1;
    uint32_t acc = 0;
    for (int i = 0; i < pageCount; ++i) {
        uint32_t dur = pageDurationMs(pages[i].type);
        if (elapsed < acc + dur) {
            pageIndex = i;
            break;
        }
        acc += dur;
    }

    if (pageIndex != lastPageIndex) {
        previousPageIndex = lastPageIndex;
        lastPageIndex = pageIndex;
        transitionStartMs = nowMs;
    }

    auto renderPage = [&](const Page& page, int xOffset) {
        const int w = display.width();
        const int hgt = display.height();

        if (page.type == PageType::TitleIntro) {
            drawTitleStd(display, "GAME", "RECAP", xOffset);
            return;
        }
        if (page.type == PageType::TitleFinal) {
            drawTitleStd(display, "FINAL", "SCORE", xOffset);
            return;
        }
        if (page.type == PageType::TitleSog) {
            drawTitleStd(display, "SOG", "", xOffset);
            return;
        }
        if (page.type == PageType::TitleGoals) {
            drawTitleStd(display, "GOALS", "RECAP", xOffset);
            return;
        }

        if (page.type == PageType::Score) {
            LogoBitmap awayLogo{};
            LogoBitmap homeLogo{};
            const bool hasAway = logoCacheGet(data.away.abbrev, awayLogo);
            const bool hasHome = logoCacheGet(data.home.abbrev, homeLogo);
            if (hasAway) {
                drawLogoWithAbbrev(display, awayLogo, data.away.abbrev, xOffset + 0, 4, 20,
                    display.color565(255, 255, 255));
            }
            if (hasHome) {
                drawLogoWithAbbrev(display, homeLogo, data.home.abbrev, xOffset + w - 21, 4, 20,
                    display.color565(255, 255, 255));
            }

            char scoreLine[12];
            snprintf(scoreLine, sizeof(scoreLine), "%u-%u",
                (unsigned)data.away.score, (unsigned)data.home.score);
            display.setTextSize(1);
            display.setTextColor(display.color565(255, 255, 255));
            int scoreW = (int)strlen(scoreLine) * 6;
            int scoreX = (w - scoreW) / 2 + xOffset;
            display.setCursor(scoreX, 11);
            display.print(scoreLine);

            if (data.period > 3) {
                const char* extra = (data.period >= 5) ? "SO" : "OT";
                int extraW = miniTextWidth(extra);
                int extraX = (w - extraW) / 2 + xOffset;
                drawMiniText(display, extraX, 20, extra, display.color565(180, 200, 255));
            }
            return;
        }

        if (page.type == PageType::Sog) {
            LogoBitmap awayLogo{};
            LogoBitmap homeLogo{};
            const bool hasAway = logoCacheGet(data.away.abbrev, awayLogo);
            const bool hasHome = logoCacheGet(data.home.abbrev, homeLogo);
            if (hasAway) {
                drawLogoWithAbbrev(display, awayLogo, data.away.abbrev, xOffset + 0, 4, 20,
                    display.color565(255, 255, 255));
            }
            if (hasHome) {
                drawLogoWithAbbrev(display, homeLogo, data.home.abbrev, xOffset + w - 21, 4, 20,
                    display.color565(255, 255, 255));
            }

            char sogLine[16];
            snprintf(sogLine, sizeof(sogLine), "%u-%u",
                (unsigned)data.away.sog, (unsigned)data.home.sog);
            int sogW = miniTextWidth(sogLine);
            int sogX = (w - sogW) / 2 + xOffset;
            drawMiniText(display, sogX, 12, sogLine, display.color565(255, 255, 255));
            return;
        }

        if (page.type == PageType::TeamGoalsTitle) {
            const TeamInfo& team = (page.teamIndex == 0) ? data.away : data.home;
            LogoBitmap logo{};
            const bool hasLogo = logoCacheGet(team.abbrev, logo);
            if (hasLogo) {
                int lx = xOffset + (w - 20) / 2;
                drawLogoScaled(display, logo, lx, 2, 20);
            }
            const int abbrevY = hasLogo ? 22 : 10;
            int abbrevW = miniTextWidth(team.abbrev);
            int abbrevX = xOffset + (w - abbrevW) / 2 + 1;
            drawMiniText(display, abbrevX, abbrevY, team.abbrev, display.color565(255, 255, 255));
            return;
        }

        if (page.type == PageType::GoalDetail) {
            if (page.goalIndex >= data.recapGoalCount) return;
            const RecapGoal& goal = data.recapGoals[page.goalIndex];

            char scorer[20];
            clampLine(goal.scorer[0] ? goal.scorer : "GOAL", scorer, sizeof(scorer));

            char a1[20];
            char a2[20];
            buildAssistLine("A1", goal.assist1, a1, sizeof(a1));
            buildAssistLine("A2", goal.assist2, a2, sizeof(a2));
            char a1Clamped[20];
            char a2Clamped[20];
            clampLine(a1, a1Clamped, sizeof(a1Clamped));
            clampLine(a2, a2Clamped, sizeof(a2Clamped));

            char elapsedLine[8];
            formatElapsedFromRemaining(goal.timeRemaining, elapsedLine, sizeof(elapsedLine));
            char timeLine[16];
            snprintf(timeLine, sizeof(timeLine), "P%u %s", (unsigned)goal.period, elapsedLine);
            char timeClamped[16];
            clampLine(timeLine, timeClamped, sizeof(timeClamped));

            int y1 = 2;
            int y2 = 9;
            int y3 = 16;
            int y4 = 23;
            drawMiniText(display, (w - miniTextWidth(scorer)) / 2 + xOffset, y1, scorer, display.color565(255, 255, 255));
            drawMiniText(display, (w - miniTextWidth(a1Clamped)) / 2 + xOffset, y2, a1Clamped, display.color565(200, 200, 200));
            drawMiniText(display, (w - miniTextWidth(a2Clamped)) / 2 + xOffset, y3, a2Clamped, display.color565(200, 200, 200));
            drawMiniText(display, (w - miniTextWidth(timeClamped)) / 2 + xOffset, y4, timeClamped, display.color565(180, 200, 255));
            return;
        }
    };

    const int w = display.width();
    if (elapsed >= contentTotal) {
        uint32_t endElapsed = elapsed - contentTotal;
        if (endElapsed > TRANSITION_MS) endElapsed = TRANSITION_MS;
        int shift = (int)((endElapsed * w) / TRANSITION_MS);
        if (shift > w) shift = w;
        renderPage(pages[pageIndex], -shift);
        return;
    }

    const uint32_t transElapsed = nowMs - transitionStartMs;
    if (transElapsed < TRANSITION_MS) {
        int shift = (int)((transElapsed * w) / TRANSITION_MS);
        if (shift > w) shift = w;
        if (previousPageIndex >= 0 && previousPageIndex < pageCount) {
            renderPage(pages[previousPageIndex], -shift);
            renderPage(pages[pageIndex], w - shift);
        } else {
            renderPage(pages[pageIndex], w - shift);
        }
    } else {
        renderPage(pages[pageIndex], 0);
    }
}
