#include "display/scoreboard_scene.h"

#include <Arduino.h>
#include <strings.h>
#include <time.h>

#include "display/logo_cache.h"
namespace {
    struct MiniGlyph {
        char c;
        uint8_t rows[5];
    };

    const MiniGlyph kMiniFont[] = {
        {' ', {0b000, 0b000, 0b000, 0b000, 0b000}},
        {'-', {0b000, 0b000, 0b111, 0b000, 0b000}},
        {':', {0b000, 0b010, 0b000, 0b010, 0b000}},
        {'0', {0b111, 0b101, 0b101, 0b101, 0b111}},
        {'1', {0b010, 0b110, 0b010, 0b010, 0b111}},
        {'2', {0b111, 0b001, 0b111, 0b100, 0b111}},
        {'3', {0b111, 0b001, 0b111, 0b001, 0b111}},
        {'4', {0b101, 0b101, 0b111, 0b001, 0b001}},
        {'5', {0b111, 0b100, 0b111, 0b001, 0b111}},
        {'6', {0b111, 0b100, 0b111, 0b101, 0b111}},
        {'7', {0b111, 0b001, 0b010, 0b010, 0b010}},
        {'8', {0b111, 0b101, 0b111, 0b101, 0b111}},
        {'9', {0b111, 0b101, 0b111, 0b001, 0b111}},
        {'A', {0b010, 0b101, 0b111, 0b101, 0b101}},
        {'B', {0b110, 0b101, 0b110, 0b101, 0b110}},
        {'C', {0b111, 0b100, 0b100, 0b100, 0b111}},
        {'D', {0b110, 0b101, 0b101, 0b101, 0b110}},
        {'E', {0b111, 0b100, 0b110, 0b100, 0b111}},
        {'F', {0b111, 0b100, 0b110, 0b100, 0b100}},
        {'G', {0b111, 0b100, 0b101, 0b101, 0b111}},
        {'H', {0b101, 0b101, 0b111, 0b101, 0b101}},
        {'I', {0b111, 0b010, 0b010, 0b010, 0b111}},
        {'J', {0b001, 0b001, 0b001, 0b101, 0b111}},
        {'K', {0b101, 0b101, 0b110, 0b101, 0b101}},
        {'L', {0b100, 0b100, 0b100, 0b100, 0b111}},
        {'M', {0b101, 0b111, 0b111, 0b101, 0b101}},
        {'N', {0b101, 0b111, 0b111, 0b111, 0b101}},
        {'O', {0b111, 0b101, 0b101, 0b101, 0b111}},
        {'P', {0b111, 0b101, 0b111, 0b100, 0b100}},
        {'Q', {0b111, 0b101, 0b101, 0b111, 0b001}},
        {'R', {0b111, 0b101, 0b111, 0b101, 0b101}},
        {'S', {0b111, 0b100, 0b111, 0b001, 0b111}},
        {'T', {0b111, 0b010, 0b010, 0b010, 0b010}},
        {'U', {0b101, 0b101, 0b101, 0b101, 0b111}},
        {'V', {0b101, 0b101, 0b101, 0b101, 0b010}},
        {'W', {0b101, 0b101, 0b111, 0b111, 0b101}},
        {'X', {0b101, 0b101, 0b010, 0b101, 0b101}},
        {'Y', {0b101, 0b101, 0b010, 0b010, 0b010}},
        {'Z', {0b111, 0b001, 0b010, 0b100, 0b111}}
    };

    const MiniGlyph* findGlyph(char c) {
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        for (size_t i = 0; i < sizeof(kMiniFont) / sizeof(kMiniFont[0]); ++i) {
            if (kMiniFont[i].c == c) return &kMiniFont[i];
        }
        return &kMiniFont[0];
    }

    void drawMiniChar(MatrixPanel_I2S_DMA& display, int x, int y, char c, uint16_t color) {
        const MiniGlyph* g = findGlyph(c);
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

    int miniTextWidth(const char* text) {
        if (!text || !text[0]) return 0;
        return ((int)strlen(text) * 4) - 1;
    }


    int parseTwo(const char* s) {
        if (!s || s[0] < '0' || s[1] < '0') return -1;
        return (s[0] - '0') * 10 + (s[1] - '0');
    }

    int parseOffsetMinutes(const char* offset) {
        if (!offset || !offset[0]) return 0;
        if ((offset[0] != '+' && offset[0] != '-') || strlen(offset) < 6) return 0;
        int sign = (offset[0] == '-') ? -1 : 1;
        int hh = parseTwo(offset + 1);
        int mm = parseTwo(offset + 4);
        if (hh < 0 || mm < 0) return 0;
        return sign * (hh * 60 + mm);
    }

    void formatStartTime(const GameSnapshot& data, char* out, size_t outSize) {
        if (!out || outSize == 0) return;
        if (!data.startTimeUtc[0]) {
            strncpy(out, "??:??", outSize);
            out[outSize - 1] = '\0';
            return;
        }
        const char* t = strchr(data.startTimeUtc, 'T');
        if (!t || strlen(t) < 6) {
            strncpy(out, "??:??", outSize);
            out[outSize - 1] = '\0';
            return;
        }
        int hh = parseTwo(t + 1);
        int mm = parseTwo(t + 4);
        if (hh < 0 || mm < 0) {
            strncpy(out, "??:??", outSize);
            out[outSize - 1] = '\0';
            return;
        }
        int total = hh * 60 + mm + parseOffsetMinutes(data.utcOffset);
        while (total < 0) total += 24 * 60;
        total %= 24 * 60;
        int outH = total / 60;
        int outM = total % 60;
        if (outM == 0) {
            snprintf(out, outSize, "%02dH", outH);
        } else {
            snprintf(out, outSize, "%02dH%02d", outH, outM);
        }
    }

    bool isGameSoonToStart(const GameSnapshot& data) {
        // Check if game start time has passed but game hasn't started
        if (!data.startTimeUtc[0]) return false;
        
        // Parse start time: format is "YYYY-MM-DDTHH:MM:SSZ"
        // Example: "2026-02-11T15:30:00Z"
        if (strlen(data.startTimeUtc) < 16) return false;
        
        int startYear = (data.startTimeUtc[0] - '0') * 1000 + (data.startTimeUtc[1] - '0') * 100 +
                        (data.startTimeUtc[2] - '0') * 10 + (data.startTimeUtc[3] - '0');
        int startMonth = parseTwo(data.startTimeUtc + 5);
        int startDay = parseTwo(data.startTimeUtc + 8);
        
        const char* t = strchr(data.startTimeUtc, 'T');
        if (!t || strlen(t) < 6) return false;
        
        int startHH = parseTwo(t + 1);
        int startMM = parseTwo(t + 4);
        if (startHH < 0 || startMM < 0 || startMonth < 0 || startDay < 0) return false;
        
        // Get current time (ESP32 has current time from NTP)
        time_t now;
        time(&now);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        
        // Apply UTC offset to start time
        int startTotalMinutes = startHH * 60 + startMM + parseOffsetMinutes(data.utcOffset);
        int startDayAdjust = 0;
        while (startTotalMinutes < 0) {
            startTotalMinutes += 24 * 60;
            startDayAdjust = -1;
        }
        if (startTotalMinutes >= 24 * 60) {
            startTotalMinutes -= 24 * 60;
            startDayAdjust = 1;
        }
        int adjustedDay = startDay + startDayAdjust;
        
        // Compare dates first (year, month, day)
        int currentYear = timeinfo.tm_year + 1900;
        int currentMonth = timeinfo.tm_mon + 1;  // tm_mon is 0-11
        int currentDay = timeinfo.tm_mday;
        
        // If not the same day, not "soon"
        if (startYear != currentYear || startMonth != currentMonth || adjustedDay != currentDay) {
            return false;
        }
        
        // Same day - check if current time >= start time
        int currentTotalMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
        return (currentTotalMinutes >= startTotalMinutes);
    }

    int textWidth(const char* s) {
        if (!s) return 0;
        return (int)strlen(s) * 6;
    }

    void truncateText(char* text, size_t maxLen) {
        if (!text || maxLen == 0) return;
        if (strlen(text) <= maxLen) return;
        text[maxLen] = '\0';
    }

    void buildTeamLabel(const TeamInfo& team, char* out, size_t outSize, size_t maxLen) {
        if (!out || outSize == 0) return;
        if (team.abbrev[0]) {
            snprintf(out, outSize, "%s", team.abbrev);
        } else if (team.name[0]) {
            snprintf(out, outSize, "%s", team.name);
        } else {
            snprintf(out, outSize, "?");
        }
        truncateText(out, maxLen);
    }
}

void ScoreboardScene::render(MatrixPanel_I2S_DMA& display, const GameSnapshot& data, uint32_t) {
    display.clearScreen();
    display.setTextWrap(false);

    if (data.gameId == 0) {
        display.setTextSize(1);
        display.setTextColor(display.color565(220, 220, 220));
        const char* msg = "NO GAME";
        int msgW = textWidth(msg);
        int msgX = (display.width() - msgW) / 2;
        if (msgX < 0) msgX = 0;
        display.setCursor(msgX, 12);
        display.print(msg);
        return;
    }

    const char* state = data.gameState;
    const bool isPre = (strcasecmp(state, "PRE") == 0 || strcasecmp(state, "FUT") == 0);
    const bool isFinal = (strcasecmp(state, "OFF") == 0 || strcasecmp(state, "FINAL") == 0);
    const bool isLive = (strcasecmp(state, "LIVE") == 0 || strcasecmp(state, "CRIT") == 0);

    char statusLine[32] = {0};
    char startTime[8] = {0};
    formatStartTime(data, startTime, sizeof(startTime));

    if (isPre) {
        if (isGameSoonToStart(data)) {
            snprintf(statusLine, sizeof(statusLine), "SOON");
        } else {
            snprintf(statusLine, sizeof(statusLine), "%s", startTime);
        }
    } else if (isFinal) {
        snprintf(statusLine, sizeof(statusLine), "FINAL");
    } else if (isLive) {
        if (data.inIntermission) {
            if (data.period == 1) {
                snprintf(statusLine, sizeof(statusLine), "END 1ST");
            } else if (data.period == 2) {
                snprintf(statusLine, sizeof(statusLine), "END 2ND");
            } else if (data.period == 3) {
                snprintf(statusLine, sizeof(statusLine), "END 3RD");
            } else {
                snprintf(statusLine, sizeof(statusLine), "INT");
            }
        } else if (data.period > 0 && data.timeRemaining[0]) {
            snprintf(statusLine, sizeof(statusLine), "P-%u", (unsigned)data.period);
        } else {
            snprintf(statusLine, sizeof(statusLine), "LIVE");
        }
    } else if (state && state[0]) {
        snprintf(statusLine, sizeof(statusLine), "%s", state);
    }

    const int panelW = display.width();

    // Logos
    LogoBitmap awayLogo{};
    LogoBitmap homeLogo{};
    const bool hasAway = logoCacheGet(data.away.abbrev, awayLogo);
    const bool hasHome = logoCacheGet(data.home.abbrev, homeLogo);
    if (!hasAway || !hasHome) {
        display.setTextSize(1);
        display.setTextColor(display.color565(200, 200, 200));
        const char* msg = "LOADING";
        int msgW = textWidth(msg);
        int msgX = (panelW - msgW) / 2;
        if (msgX < 0) msgX = 0;
        display.setCursor(msgX, 12);
        display.print(msg);
        return;
    }

    int awayLogoX = 0;
    int homeLogoX = panelW - 20;
    display.drawRGBBitmap(0, 0, awayLogo.pixels, awayLogo.width, awayLogo.height);
    int homeX = panelW - homeLogo.width;
    if (homeX < 0) homeX = 0;
    display.drawRGBBitmap(homeX, 0, homeLogo.pixels, homeLogo.width, homeLogo.height);
    homeLogoX = homeX;

    // Scores in center (standard font)
    char scoreLine[12];
    snprintf(scoreLine, sizeof(scoreLine), "%u-%u",
        (unsigned)data.away.score, (unsigned)data.home.score);
    display.setTextSize(1);
    display.setTextColor(display.color565(255, 255, 255));
    int scoreW = textWidth(scoreLine);
    int scoreX = (panelW - scoreW) / 2;
    if (scoreX < 0) scoreX = 0;
    display.setCursor(scoreX, 5);
    display.print(scoreLine);

    // Center status (period/time or start time)
    display.setTextSize(1);
    char timeLine[16] = {0};
    if (isLive && data.timeRemaining[0] && !data.inIntermission) {
        snprintf(timeLine, sizeof(timeLine), "%s", data.timeRemaining);
    }
    if (isLive && timeLine[0]) {
        const int lineGap = 2;
        const int statusY = 14;
        const int timeY = statusY + 5 + lineGap;
        int statusW = miniTextWidth(statusLine);
        int statusX = (panelW - statusW) / 2;
        if (statusX < 0) statusX = 0;
        drawMiniText(display, statusX, statusY, statusLine, display.color565(180, 200, 255));
        int timeW = miniTextWidth(timeLine);
        int timeX = (panelW - timeW) / 2;
        if (timeX < 0) timeX = 0;
        drawMiniText(display, timeX, timeY, timeLine, display.color565(180, 200, 255));
    } else {
        int statusW = miniTextWidth(statusLine);
        int statusX = (panelW - statusW) / 2;
        if (statusX < 0) statusX = 0;
        drawMiniText(display, statusX, 16, statusLine, display.color565(180, 200, 255));
    }

    // Team labels under logos (centered)
    char awayLine[8];
    char homeLine[8];
    buildTeamLabel(data.away, awayLine, sizeof(awayLine), 3);
    buildTeamLabel(data.home, homeLine, sizeof(homeLine), 3);
    int awayNameY = hasAway ? (awayLogo.height + 1) : 22;
    int homeNameY = hasHome ? (homeLogo.height + 1) : 22;
    int awayTextW = miniTextWidth(awayLine);
    int awayTextX = awayLogoX + ((hasAway ? awayLogo.width : 20) - awayTextW) / 2;
    if (awayTextX < 0) awayTextX = 0;
    int homeTextW = miniTextWidth(homeLine);
    int homeTextX = homeLogoX + ((hasHome ? homeLogo.width : 20) - homeTextW) / 2;
    if (homeTextX < 0) homeTextX = 0;
    drawMiniText(display, awayTextX, awayNameY, awayLine, display.color565(255, 255, 255));
    drawMiniText(display, homeTextX, homeNameY, homeLine, display.color565(255, 255, 255));

    // PP under team name, aligned to first letter
    if (data.awayPP) {
        const bool flash = ((millis() / 300) % 2) == 0;
        int ppY = awayNameY + 6;
        int ppW = miniTextWidth("PP");
        int ppX = awayTextX + (awayTextW - ppW) / 2;
        if (ppX < 0) ppX = 0;
        drawMiniText(display, ppX, ppY, "PP", flash ? display.color565(255, 80, 80) : display.color565(200, 200, 200));
    }
    if (data.homePP) {
        const bool flash = ((millis() / 300) % 2) == 0;
        int ppY = homeNameY + 6;
        int ppW = miniTextWidth("PP");
        int ppX = homeTextX + (homeTextW - ppW) / 2;
        if (ppX < 0) ppX = 0;
        drawMiniText(display, ppX, ppY, "PP", flash ? display.color565(255, 80, 80) : display.color565(200, 200, 200));
    }
}

