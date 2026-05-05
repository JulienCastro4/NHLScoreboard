#include "display/scoreboard_scene.h"

#include <Arduino.h>
#include <strings.h>
#include <time.h>

#include "display/logo_cache.h"
namespace
{
    struct MiniGlyph
    {
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
        {'Z', {0b111, 0b001, 0b010, 0b100, 0b111}}};

    const MiniGlyph *findGlyph(char c)
    {
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 32);
        for (size_t i = 0; i < sizeof(kMiniFont) / sizeof(kMiniFont[0]); ++i)
        {
            if (kMiniFont[i].c == c)
                return &kMiniFont[i];
        }
        return &kMiniFont[0];
    }

    void drawMiniChar(MatrixPanel_I2S_DMA &display, int x, int y, char c, uint16_t color)
    {
        const MiniGlyph *g = findGlyph(c);
        for (int row = 0; row < 5; ++row)
        {
            for (int col = 0; col < 3; ++col)
            {
                if (g->rows[row] & (1 << (2 - col)))
                {
                    display.drawPixel(x + col, y + row, color);
                }
            }
        }
    }

    void drawMiniText(MatrixPanel_I2S_DMA &display, int x, int y, const char *text, uint16_t color)
    {
        if (!text)
            return;
        int cursor = x;
        for (size_t i = 0; text[i]; ++i)
        {
            drawMiniChar(display, cursor, y, text[i], color);
            cursor += 4;
        }
    }

    int miniTextWidth(const char *text)
    {
        if (!text || !text[0])
            return 0;
        return ((int)strlen(text) * 4) - 1;
    }

    int parseTwo(const char *s)
    {
        if (!s || s[0] < '0' || s[1] < '0')
            return -1;
        return (s[0] - '0') * 10 + (s[1] - '0');
    }

    int parseFour(const char *s)
    {
        if (!s ||
            s[0] < '0' || s[0] > '9' ||
            s[1] < '0' || s[1] > '9' ||
            s[2] < '0' || s[2] > '9' ||
            s[3] < '0' || s[3] > '9')
            return -1;
        return (s[0] - '0') * 1000 +
               (s[1] - '0') * 100 +
               (s[2] - '0') * 10 +
               (s[3] - '0');
    }

    // Convert civil date to days since Unix epoch (1970-01-01).
    int64_t daysFromCivil(int y, unsigned m, unsigned d)
    {
        y -= m <= 2;
        const int era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = (unsigned)(y - era * 400);
        const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + (int)doe - 719468;
    }

    bool parseUtcIsoToEpoch(const char *iso, time_t &outEpoch)
    {
        if (!iso)
            return false;
        // Expected format: YYYY-MM-DDTHH:MM:SSZ
        if (strlen(iso) < 20)
            return false;
        if (iso[4] != '-' || iso[7] != '-' || iso[10] != 'T' || iso[13] != ':' || iso[16] != ':')
            return false;

        const int year = parseFour(iso + 0);
        const int month = parseTwo(iso + 5);
        const int day = parseTwo(iso + 8);
        const int hour = parseTwo(iso + 11);
        const int minute = parseTwo(iso + 14);
        const int second = parseTwo(iso + 17);

        if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31 ||
            hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59)
            return false;

        const int64_t days = daysFromCivil(year, (unsigned)month, (unsigned)day);
        const int64_t totalSeconds = days * 86400LL + (int64_t)hour * 3600LL + (int64_t)minute * 60LL + second;
        if (totalSeconds < 0)
            return false;

        outEpoch = (time_t)totalSeconds;
        return true;
    }

    int parseOffsetMinutes(const char *offset)
    {
        if (!offset || !offset[0])
            return 0;
        if ((offset[0] != '+' && offset[0] != '-') || strlen(offset) < 6)
            return 0;
        int sign = (offset[0] == '-') ? -1 : 1;
        int hh = parseTwo(offset + 1);
        int mm = parseTwo(offset + 4);
        if (hh < 0 || mm < 0)
            return 0;
        return sign * (hh * 60 + mm);
    }

    void formatStartTime(const GameSnapshot &data, char *out, size_t outSize)
    {
        if (!out || outSize == 0)
            return;
        if (!data.startTimeUtc[0])
        {
            strncpy(out, "??:??", outSize);
            out[outSize - 1] = '\0';
            return;
        }
        const char *t = strchr(data.startTimeUtc, 'T');
        if (!t || strlen(t) < 6)
        {
            strncpy(out, "??:??", outSize);
            out[outSize - 1] = '\0';
            return;
        }
        int hh = parseTwo(t + 1);
        int mm = parseTwo(t + 4);
        if (hh < 0 || mm < 0)
        {
            strncpy(out, "??:??", outSize);
            out[outSize - 1] = '\0';
            return;
        }
        int total = hh * 60 + mm + parseOffsetMinutes(data.utcOffset);
        while (total < 0)
            total += 24 * 60;
        total %= 24 * 60;
        int outH = total / 60;
        int outM = total % 60;
        if (outM == 0)
        {
            snprintf(out, outSize, "%02dH", outH);
        }
        else
        {
            snprintf(out, outSize, "%02dH%02d", outH, outM);
        }
    }

    bool isGameSoonToStart(const GameSnapshot &data)
    {
        // Show SOON when scheduled start time is reached but game is still PRE/FUT.
        if (!data.startTimeUtc[0])
            return false;

        time_t startUtc = 0;
        if (!parseUtcIsoToEpoch(data.startTimeUtc, startUtc))
            return false;

        const time_t nowUtc = time(nullptr);
        if (nowUtc < 100000)
            return false; // NTP not ready

        return nowUtc >= startUtc;
    }

    int textWidth(const char *s)
    {
        if (!s)
            return 0;
        return (int)strlen(s) * 6;
    }

    bool isClockExpired(const char *timeRemaining)
    {
        if (!timeRemaining || !timeRemaining[0])
            return false;
        // Check if all digits are '0' (handles "00:00", "0:00", "00:00.0", etc.)
        for (const char *p = timeRemaining; *p; ++p)
        {
            if (*p >= '1' && *p <= '9')
                return false;
        }
        return true;
    }

    void truncateText(char *text, size_t maxLen)
    {
        if (!text || maxLen == 0)
            return;
        if (strlen(text) <= maxLen)
            return;
        text[maxLen] = '\0';
    }

    void buildTeamLabel(const TeamInfo &team, char *out, size_t outSize, size_t maxLen)
    {
        if (!out || outSize == 0)
            return;
        if (team.abbrev[0])
        {
            snprintf(out, outSize, "%s", team.abbrev);
        }
        else if (team.name[0])
        {
            snprintf(out, outSize, "%s", team.name);
        }
        else
        {
            snprintf(out, outSize, "?");
        }
        truncateText(out, maxLen);
    }
}

void ScoreboardScene::resetFinalPhase()
{
    finalPhaseStartMs = millis();
    finalSeriesPhaseUnlocked = false;
    scoreSlideY = 0;
}

void ScoreboardScene::render(MatrixPanel_I2S_DMA &display, const GameSnapshot &data, uint32_t nowMs)
{
    display.clearScreen();
    display.setTextWrap(false);

    if (data.gameId == 0)
    {
        LogoBitmap nhlLogo{};
        if (logoLoadStatic("/logos/nhl_logo.rgb565", nhlLogo))
        {
            int x = (display.width() - nhlLogo.width) / 2;
            int y = (display.height() - nhlLogo.height) / 2;
            if (x < 0)
                x = 0;
            if (y < 0)
                y = 0;
            display.drawRGBBitmap(x, y, nhlLogo.pixels, nhlLogo.width, nhlLogo.height);
        }
        else
        {
            display.setTextSize(1);
            display.setTextColor(display.color565(220, 220, 220));
            const char *msg = "NHL";
            int msgW = textWidth(msg);
            int msgX = (display.width() - msgW) / 2;
            if (msgX < 0)
                msgX = 0;
            display.setCursor(msgX, 12);
            display.print(msg);
        }
        return;
    }

    const char *state = data.gameState;
    const bool isPre = (strcasecmp(state, "PRE") == 0 || strcasecmp(state, "FUT") == 0);
    const bool isFinal = (strcasecmp(state, "OFF") == 0 || strcasecmp(state, "FINAL") == 0);
    const bool isLive = (strcasecmp(state, "LIVE") == 0 || strcasecmp(state, "CRIT") == 0);
    const bool isPlayoffGame = (data.gameType == 3);
    const bool hasSeriesData = data.seriesTopSeedAbbrev[0] && data.seriesBottomSeedAbbrev[0];

    if (isFinal || isPre)
    {
        if (!finalPhaseActive || finalPhaseGameId != data.gameId)
        {
            finalPhaseActive = true;
            finalPhaseGameId = data.gameId;
            finalPhaseStartMs = nowMs;
            finalSeriesPhaseUnlocked = false;
            hadSeriesData = false;
        }
    }
    else
    {
        finalPhaseActive = false;
        finalPhaseGameId = 0;
        finalSeriesPhaseUnlocked = false;
        hadSeriesData = false;
    }

    // Detect when series data first arrives and reset the 10s timer
    if ((isFinal || isPre) && isPlayoffGame && hasSeriesData && !hadSeriesData)
    {
        finalPhaseStartMs = nowMs;
        finalSeriesPhaseUnlocked = false;
    }
    hadSeriesData = hasSeriesData;

    if ((isFinal || isPre) && isPlayoffGame && hasSeriesData && finalPhaseActive && !finalSeriesPhaseUnlocked)
    {
        if (nowMs - finalPhaseStartMs >= 10000UL)
        {
            finalSeriesPhaseUnlocked = true;
            seriesShownMs = nowMs;
        }
    }

    // For PRE games, auto-cycle: after 10s of series display, reset to show normal PRE info
    if (isPre && finalSeriesPhaseUnlocked && (nowMs - seriesShownMs >= 10000UL))
    {
        finalSeriesPhaseUnlocked = false;
        finalPhaseStartMs = nowMs;
        scoreSlideY = 0;
    }

    const bool showFinalSeries = (isFinal || isPre) && isPlayoffGame && hasSeriesData && finalSeriesPhaseUnlocked;
    const bool clockExpired = (data.period > 0 && isClockExpired(data.timeRemaining));
    const bool hasStarted = (data.period > 0) || data.away.score > 0 || data.home.score > 0;
    const bool isEndOfPeriodState = !isPre && !isFinal && hasStarted && (data.inIntermission || clockExpired);

    char statusLine[32] = {0};
    char startTime[8] = {0};
    formatStartTime(data, startTime, sizeof(startTime));

    char dateLabel[6] = {0};
    if (isPre)
    {
        if (isGameSoonToStart(data))
        {
            snprintf(statusLine, sizeof(statusLine), "SOON");
        }
        else
        {
            snprintf(statusLine, sizeof(statusLine), "%s", startTime);
            // Extract DD-MM from startTimeUtc with UTC offset applied
            if (strlen(data.startTimeUtc) >= 16)
            {
                const char *tPos = strchr(data.startTimeUtc, 'T');
                if (tPos && strlen(tPos) >= 6)
                {
                    int utcHH = parseTwo(tPos + 1);
                    int utcMM = parseTwo(tPos + 4);
                    int utcDay = parseTwo(data.startTimeUtc + 8);
                    int utcMonth = parseTwo(data.startTimeUtc + 5);
                    int utcYear = (data.startTimeUtc[0] - '0') * 1000 +
                                  (data.startTimeUtc[1] - '0') * 100 +
                                  (data.startTimeUtc[2] - '0') * 10 +
                                  (data.startTimeUtc[3] - '0');
                    if (utcHH >= 0 && utcMM >= 0 && utcDay > 0 && utcMonth > 0)
                    {
                        struct tm localTm = {};
                        localTm.tm_year = utcYear - 1900;
                        localTm.tm_mon = utcMonth - 1;
                        localTm.tm_mday = utcDay;
                        localTm.tm_hour = utcHH;
                        localTm.tm_min = utcMM + parseOffsetMinutes(data.utcOffset);
                        localTm.tm_sec = 0;
                        localTm.tm_isdst = 0;
                        mktime(&localTm);
                        snprintf(dateLabel, sizeof(dateLabel), "%02d-%02d",
                                 localTm.tm_mday, localTm.tm_mon + 1);
                    }
                }
            }
        }
    }
    else if (isFinal)
    {
        snprintf(statusLine, sizeof(statusLine), "FINAL");
    }
    else if (isLive || isEndOfPeriodState)
    {
        if (isEndOfPeriodState)
        {
            if (data.period == 1)
            {
                snprintf(statusLine, sizeof(statusLine), "END 1ST");
            }
            else if (data.period == 2)
            {
                snprintf(statusLine, sizeof(statusLine), "END 2ND");
            }
            else if (data.period == 3)
            {
                snprintf(statusLine, sizeof(statusLine), "END 3RD");
            }
            else if (data.period == 4)
            {
                snprintf(statusLine, sizeof(statusLine), "END OT");
            }
            else if (data.period >= 5)
            {
                if (isPlayoffGame)
                {
                    snprintf(statusLine, sizeof(statusLine), "END OT%u", (unsigned)(data.period - 3));
                }
                else
                {
                    snprintf(statusLine, sizeof(statusLine), "END SO");
                }
            }
            else
            {
                snprintf(statusLine, sizeof(statusLine), "INT");
            }
        }
        else if (data.period > 0 && data.timeRemaining[0])
        {
            if (data.period <= 3)
            {
                snprintf(statusLine, sizeof(statusLine), "P-%u", (unsigned)data.period);
            }
            else if (data.period == 4)
            {
                snprintf(statusLine, sizeof(statusLine), "OT");
            }
            else if (isPlayoffGame)
            {
                snprintf(statusLine, sizeof(statusLine), "OT%u", (unsigned)(data.period - 3));
            }
            else
            {
                snprintf(statusLine, sizeof(statusLine), "SO");
            }
        }
        else
        {
            snprintf(statusLine, sizeof(statusLine), "LIVE");
        }
    }
    else if (state && state[0])
    {
        snprintf(statusLine, sizeof(statusLine), "%s", state);
    }

    const int panelW = display.width();

    // Text offset: +2px for PRE/FINAL, 0 for live
    const int sceneOffsetY = (isPre || isFinal) ? 2 : 0;

    // Logo base: +2px down; slides 2px more up when showing SOG
    const int logoBaseY = 2;
    {
        const int slideTarget = (showSOG && isLive) ? -2 : 0;
        unsigned long now = millis();
        if (logoSlideY != slideTarget && (now - lastSlideMs >= 60)) {
            if (logoSlideY < slideTarget) logoSlideY++;
            else if (logoSlideY > slideTarget) logoSlideY--;
            lastSlideMs = now;
        }
    }
    const int logoYOffset = logoBaseY + logoSlideY;

    // Logos
    LogoBitmap awayLogo{};
    LogoBitmap homeLogo{};
    const bool hasAway = logoCacheGet(data.away.abbrev, awayLogo);
    const bool hasHome = logoCacheGet(data.home.abbrev, homeLogo);
    if (!hasAway || !hasHome)
    {
        display.setTextSize(1);
        display.setTextColor(display.color565(200, 200, 200));
        const char *msg = "LOADING";
        int msgW = textWidth(msg);
        int msgX = (panelW - msgW) / 2;
        if (msgX < 0)
            msgX = 0;
        display.setCursor(msgX, 12);
        display.print(msg);
        return;
    }

    if ((isFinal || isPre) && isPlayoffGame && !hasSeriesData)
    {
        // Wait for series fallback data before showing playoff layout.
        int w1 = miniTextWidth("SERIES");
        int x1 = (panelW - w1) / 2;
        if (x1 < 0)
            x1 = 0;
        int w2 = miniTextWidth("LOADING");
        int x2 = (panelW - w2) / 2;
        if (x2 < 0)
            x2 = 0;
        drawMiniText(display, x1, 11, "SERIES", display.color565(180, 200, 255));
        drawMiniText(display, x2, 18, "LOADING", display.color565(160, 180, 220));
        return;
    }

    int awayLogoX = 0;
    int homeLogoX = panelW - 20;
    display.drawRGBBitmap(0, logoYOffset, awayLogo.pixels, awayLogo.width, awayLogo.height);
    int homeX = panelW - homeLogo.width;
    if (homeX < 0)
        homeX = 0;
    display.drawRGBBitmap(homeX, logoYOffset, homeLogo.pixels, homeLogo.width, homeLogo.height);
    homeLogoX = homeX;

    // Scores centered between logos (vertically and horizontally)
    char scoreLine[12];
    snprintf(scoreLine, sizeof(scoreLine), "%u-%u",
             (unsigned)data.away.score, (unsigned)data.home.score);
    display.setTextSize(1);
    display.setTextColor(display.color565(255, 255, 255));
    int scoreW = textWidth(scoreLine);
    int scoreX = (panelW - scoreW) / 2;
    if (scoreX < 0)
        scoreX = 0;
    {
        const int target = showFinalSeries ? -4 : 0;
        unsigned long now = millis();
        if (scoreSlideY != target && (now - lastScoreSlideMs >= 60))
        {
            if (scoreSlideY < target)
                scoreSlideY++;
            else if (scoreSlideY > target)
                scoreSlideY--;
            lastScoreSlideMs = now;
        }
    }
    const int scoreYOffset = sceneOffsetY + scoreSlideY;
    const int logoHeight = hasAway ? awayLogo.height : 20;
    const int scoreY = (logoHeight - 8) / 2 + scoreYOffset;
    display.setCursor(scoreX, scoreY);
    display.print(scoreLine);

    // Center status (period/time or start time) - 2px below score
    display.setTextSize(1);
    char timeLine[16] = {0};
    if (isLive && data.timeRemaining[0] && !data.inIntermission && !clockExpired)
    {
        snprintf(timeLine, sizeof(timeLine), "%s", data.timeRemaining);
    }
    const int statusBaseY = scoreY + 8 + 2; // Score height (8px) + 2px gap
    if (isLive && timeLine[0])
    {
        const int lineGap = 2;
        const int statusY = statusBaseY;
        const int timeY = statusY + 5 + lineGap;
        int statusW = miniTextWidth(statusLine);
        int statusX = (panelW - statusW) / 2;
        if (statusX < 0)
            statusX = 0;
        drawMiniText(display, statusX, statusY, statusLine, display.color565(180, 200, 255));
        int timeW = miniTextWidth(timeLine);
        int timeX = (panelW - timeW) / 2;
        if (timeX < 0)
            timeX = 0;
        drawMiniText(display, timeX, timeY, timeLine, display.color565(180, 200, 255));
    }
    else if (!showFinalSeries)
    {
        int statusW = miniTextWidth(statusLine);
        int statusX = (panelW - statusW) / 2;
        if (statusX < 0)
            statusX = 0;
        int yOffset = (strncmp(statusLine, "END", 3) == 0) ? 4 : 0;
        drawMiniText(display, statusX, statusBaseY + yOffset, statusLine, display.color565(180, 200, 255));

        // Show DD-MM date below start time for upcoming games
        if (dateLabel[0])
        {
            int dateW = miniTextWidth(dateLabel);
            int dateX = (panelW - dateW) / 2;
            if (dateX < 0)
                dateX = 0;
            drawMiniText(display, dateX, statusBaseY + yOffset + 6, dateLabel, display.color565(140, 160, 200));
        }
    }

    // Toggle SOG display every 15 seconds during live games (but not during active PP)
    const bool anyPP = data.awayPP || data.homePP;
    const bool ppVisible = !isEndOfPeriodState;
    const bool anyActivePP = anyPP && ppVisible;
    if (isLive && !anyActivePP)
    {
        unsigned long now = millis();
        if (now - lastToggleMs >= 15000)
        {
            showSOG = !showSOG;
            lastToggleMs = now;
        }
    }
    else
    {
        showSOG = false;
        lastToggleMs = millis();
    }

    // Team labels under logos
    char awayLine[8];
    char homeLine[8];

    int awayNameY = (hasAway ? awayLogo.height : 21) + logoYOffset;
    int homeNameY = (hasHome ? homeLogo.height : 21) + logoYOffset;

    if (showFinalSeries)
    {
        char line1[10] = {0};
        char line2[10] = {0};
        char line3[10] = {0};
        const uint8_t topWins = data.seriesTopSeedWins;
        const uint8_t bottomWins = data.seriesBottomSeedWins;
        snprintf(line3, sizeof(line3), "%u-%u", (unsigned)topWins, (unsigned)bottomWins);

        if (topWins == bottomWins)
        {
            snprintf(line1, sizeof(line1), "SERIES");
            snprintf(line2, sizeof(line2), "TIED");
        }
        else
        {
            const bool topLeads = topWins > bottomWins;
            const char *leadAbbrev = topLeads ? data.seriesTopSeedAbbrev : data.seriesBottomSeedAbbrev;
            const uint8_t leadWins = topLeads ? topWins : bottomWins;
            const uint8_t trailWins = topLeads ? bottomWins : topWins;
            snprintf(line1, sizeof(line1), "%s", leadAbbrev);
            if (leadWins >= 4)
            {
                snprintf(line2, sizeof(line2), "WINS");
            }
            else
            {
                snprintf(line2, sizeof(line2), "LEADS");
            }
            snprintf(line3, sizeof(line3), "%u-%u", (unsigned)leadWins, (unsigned)trailWins);
        }

        const int panelH = display.height();
        const int lineStep = 6;
        const int blockH = 5 + lineStep + lineStep;
        int y1 = (panelH - blockH) / 2 + 6;
        int y2 = y1 + 6;
        int y3 = y2 + 6;

        int w1 = miniTextWidth(line1);
        int x1 = (panelW - w1) / 2;
        if (x1 < 0)
            x1 = 0;
        int w2 = miniTextWidth(line2);
        int x2 = (panelW - w2) / 2;
        if (x2 < 0)
            x2 = 0;
        int w3 = miniTextWidth(line3);
        int x3 = (panelW - w3) / 2;
        if (x3 < 0)
            x3 = 0;

        drawMiniText(display, x1, y1, line1, display.color565(255, 255, 255));
        drawMiniText(display, x2, y2, line2, display.color565(180, 200, 255));
        drawMiniText(display, x3, y3, line3, display.color565(180, 200, 255));
    }

    if (isLive && showSOG && !anyActivePP)
    {
        // Show SOG on 2 lines: number on top, "SOG" below
        char awaySogNum[8];
        char homeSogNum[8];
        snprintf(awaySogNum, sizeof(awaySogNum), "%u", (unsigned)data.away.sog);
        snprintf(homeSogNum, sizeof(homeSogNum), "%u", (unsigned)data.home.sog);

        // Line 1: Number (centered under logo)
        int awayNumW = miniTextWidth(awaySogNum);
        int awayNumX = awayLogoX + ((hasAway ? awayLogo.width : 20) - awayNumW) / 2;
        if (awayNumX < 0)
            awayNumX = 0;
        drawMiniText(display, awayNumX, awayNameY, awaySogNum, display.color565(255, 255, 255));

        int homeNumW = miniTextWidth(homeSogNum);
        int homeNumX = homeLogoX + ((hasHome ? homeLogo.width : 20) - homeNumW) / 2;
        if (homeNumX < 0)
            homeNumX = 0;
        drawMiniText(display, homeNumX, homeNameY, homeSogNum, display.color565(255, 255, 255));

        // Line 2: "SOG" (centered under logo)
        int awaySogW = miniTextWidth("SOG");
        int awaySogX = awayLogoX + ((hasAway ? awayLogo.width : 20) - awaySogW) / 2;
        if (awaySogX < 0)
            awaySogX = 0;
        drawMiniText(display, awaySogX, awayNameY + 6, "SOG", display.color565(255, 255, 255));

        int homeSogW = miniTextWidth("SOG");
        int homeSogX = homeLogoX + ((hasHome ? homeLogo.width : 20) - homeSogW) / 2;
        if (homeSogX < 0)
            homeSogX = 0;
        drawMiniText(display, homeSogX, homeNameY + 6, "SOG", display.color565(255, 255, 255));
    }
    else
    {
        // Show team abbreviation or flashing PP
        buildTeamLabel(data.away, awayLine, sizeof(awayLine), 3);
        buildTeamLabel(data.home, homeLine, sizeof(homeLine), 3);

        const bool flash = ((millis() / 300) % 2) == 0;

        // Away label: show flashing PP if away is on power play, else abbreviation
        if (data.awayPP && ppVisible)
        {
            int ppW = miniTextWidth("PP");
            int ppX = awayLogoX + ((hasAway ? awayLogo.width : 20) - ppW) / 2;
            if (ppX < 0)
                ppX = 0;
            drawMiniText(display, ppX, awayNameY, "PP", flash ? display.color565(255, 80, 80) : display.color565(200, 200, 200));
        }
        else
        {
            int awayTextW = miniTextWidth(awayLine);
            int awayTextX = awayLogoX + ((hasAway ? awayLogo.width : 20) - awayTextW) / 2;
            if (awayTextX < 0)
                awayTextX = 0;
            drawMiniText(display, awayTextX, awayNameY, awayLine, display.color565(255, 255, 255));
        }

        // Home label: show flashing PP if home is on power play, else abbreviation
        if (data.homePP && ppVisible)
        {
            int ppW = miniTextWidth("PP");
            int ppX = homeLogoX + ((hasHome ? homeLogo.width : 20) - ppW) / 2;
            if (ppX < 0)
                ppX = 0;
            drawMiniText(display, ppX, homeNameY, "PP", flash ? display.color565(255, 80, 80) : display.color565(200, 200, 200));
        }
        else
        {
            int homeTextW = miniTextWidth(homeLine);
            int homeTextX = homeLogoX + ((hasHome ? homeLogo.width : 20) - homeTextW) / 2;
            if (homeTextX < 0)
                homeTextX = 0;
            drawMiniText(display, homeTextX, homeNameY, homeLine, display.color565(255, 255, 255));
        }
    }

}
