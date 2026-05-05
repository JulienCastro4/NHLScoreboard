#include "playbyplay_service.h"

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <ctype.h>
#include <strings.h>

#include "api_server.h"
#include "display/data_model.h"
#include "prefix_stream.h"

// ============================================================================
// CONSTANTS
// ============================================================================
static const char* NHL_PBP_URL_FMT = "https://api-web.nhle.com/v1/gamecenter/%u/play-by-play";
static const char* NHL_SCOREBOARD_URL = "https://api-web.nhle.com/v1/scoreboard/now";
static const unsigned long PBP_MIN_INTERVAL_MS = 5000;
static const unsigned long PBP_FAIL_BACKOFF_MS = 5000;
static const int PBP_MAX_RETRIES = 3;
static const unsigned long PBP_RETRY_BASE_MS = 1000;

// ============================================================================
// DATA STRUCTURES
// ============================================================================
struct PlayerEntry {
    int id;
    char name[32];
};

struct GoalInfo {
    bool isNew;
    int eventId;
    int ownerTeamId;
    int scoringPlayerId;
    int period;
    String type;
    String time;
    String scoringPlayerName;
    String shootingPlayerName;
    String assist1Name;
    String assist2Name;
    String goalieName;
    String secondaryType;
    String shotType;
};

struct PbpState {
    String lastGoodResponse;
    unsigned long lastFetchMs;
    unsigned long lastFailMs;
    uint32_t gameId;
    int lastPlaySortOrder;
    bool primed;
    bool hadEmptyFetch;
};

struct RosterCache {
    PlayerEntry players[80];
    size_t count;
    uint32_t gameId;
    
    void clear() {
        count = 0;
        gameId = 0;
    }
    
    const char* lookupName(int playerId) const {
        if (playerId == 0) return "";
        for (size_t i = 0; i < count; ++i) {
            if (players[i].id == playerId) return players[i].name;
        }
        return "";
    }
};

struct SeriesStatusCache {
    uint32_t gameId;
    char topSeedAbbrev[4];
    uint8_t topSeedWins;
    char bottomSeedAbbrev[4];
    uint8_t bottomSeedWins;
    unsigned long lastAttemptMs;
    bool valid;

    void clear() {
        gameId = 0;
        topSeedAbbrev[0] = '\0';
        topSeedWins = 0;
        bottomSeedAbbrev[0] = '\0';
        bottomSeedWins = 0;
        lastAttemptMs = 0;
        valid = false;
    }
};

// ============================================================================
// GLOBALS
// ============================================================================
static WebServer* playByPlayServer = nullptr;
static WiFiClientSecure playByPlayClient;
static PbpState state;
static RosterCache rosterCache;
static SeriesStatusCache seriesCache;
static SemaphoreHandle_t pbpResponseMutex = nullptr;

static DeserializationError fetchAndParseJson(const char* url, JsonDocument& doc, JsonDocument& filterDoc);

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

static void buildFullName(char* dest, size_t destSize, const char* part1, const char* part2) {
    if (!dest || destSize == 0) return;
    if (part1[0] && part2[0])
        snprintf(dest, destSize, "%s %s", part1, part2);
    else
        snprintf(dest, destSize, "%s%s", part1, part2);
}

static void buildRosterCache(JsonArray roster, uint32_t gameId) {
    rosterCache.clear();
    rosterCache.gameId = gameId;
    if (roster.isNull()) return;
    
    for (JsonObject p : roster) {
        if (rosterCache.count >= sizeof(rosterCache.players) / sizeof(rosterCache.players[0])) break;
        
        int id = p["playerId"] | 0;
        const char* first = p["firstName"]["default"] | "";
        const char* last = p["lastName"]["default"] | "";
        
        if (id == 0) continue;
        
        rosterCache.players[rosterCache.count].id = id;
        buildFullName(rosterCache.players[rosterCache.count].name, 
                     sizeof(rosterCache.players[rosterCache.count].name), 
                     first, last);
        rosterCache.count++;
    }
}

static String resolvePlayerName(const char* apiName, int playerId) {
    if (apiName && apiName[0]) return String(apiName);
    const char* cached = rosterCache.lookupName(playerId);
    return String(cached);
}

static void copyShort(char* dest, size_t destSize, const char* src) {
    if (!dest || destSize == 0) return;
    if (!src) src = "";
    strncpy(dest, src, destSize - 1);
    dest[destSize - 1] = '\0';
}

static bool fetchSeriesStatusFromScoreboard(uint32_t gameId,
    char* topAbbrev,
    uint8_t& topWins,
    char* bottomAbbrev,
    uint8_t& bottomWins) {
    JsonDocument doc;
    JsonDocument filter;
    filter["gamesByDate"][0]["games"][0]["id"] = true;
    filter["gamesByDate"][0]["games"][0]["seriesStatus"]["topSeedTeamAbbrev"] = true;
    filter["gamesByDate"][0]["games"][0]["seriesStatus"]["topSeedWins"] = true;
    filter["gamesByDate"][0]["games"][0]["seriesStatus"]["bottomSeedTeamAbbrev"] = true;
    filter["gamesByDate"][0]["games"][0]["seriesStatus"]["bottomSeedWins"] = true;

    DeserializationError err = fetchAndParseJson(NHL_SCOREBOARD_URL, doc, filter);
    if (err) {
        return false;
    }

    JsonArray days = doc["gamesByDate"];
    if (days.isNull()) return false;

    for (JsonObject day : days) {
        JsonArray games = day["games"];
        for (JsonObject game : games) {
            if ((game["id"] | 0) != gameId) continue;
            JsonObject series = game["seriesStatus"];
            if (series.isNull()) return false;
            copyShort(topAbbrev, 4, series["topSeedTeamAbbrev"] | "");
            topWins = series["topSeedWins"] | 0;
            copyShort(bottomAbbrev, 4, series["bottomSeedTeamAbbrev"] | "");
            bottomWins = series["bottomSeedWins"] | 0;
            return topAbbrev[0] && bottomAbbrev[0];
        }
    }

    return false;
}

static bool isFinalState(const char* state) {
    if (!state || !state[0]) return false;
    return (strcasecmp(state, "FINAL") == 0) || (strcasecmp(state, "OFF") == 0);
}

static bool isAllowedRecapChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == ':';
}

static void sanitizeToken(const char* in, char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    out[0] = '\0';
    if (!in || !in[0]) return;
    size_t o = 0;
    bool lastSpace = true;
    for (size_t i = 0; in[i] && o + 1 < outSize; ++i) {
        char c = (char)toupper((unsigned char)in[i]);
        if (!isAllowedRecapChar(c)) c = ' ';
        if (c == ' ') {
            if (lastSpace) continue;
            lastSpace = true;
        } else {
            lastSpace = false;
        }
        out[o++] = c;
    }
    while (o > 0 && out[o - 1] == ' ') {
        --o;
    }
    out[o] = '\0';
}

static void extractLastName(const char* fullName, char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    out[0] = '\0';
    if (!fullName || !fullName[0]) return;
    const char* last = fullName;
    for (const char* p = fullName; *p; ++p) {
        if (*p == ' ') last = p + 1;
    }
    strncpy(out, last, outSize - 1);
    out[outSize - 1] = '\0';
}

static const char* teamAbbrevForId(int teamId, int awayId, const char* awayAbbrev, int homeId, const char* homeAbbrev) {
    if (teamId == awayId) return awayAbbrev;
    if (teamId == homeId) return homeAbbrev;
    return "";
}

static void copyRecapName(const String& fullName, char* out, size_t outSize) {
    char lastName[32];
    extractLastName(fullName.c_str(), lastName, sizeof(lastName));
    sanitizeToken(lastName, out, outSize);
}

static uint8_t buildRecapGoals(JsonDocument& doc,
    const char* awayAbbrev,
    const char* homeAbbrev,
    RecapGoal* outGoals,
    uint8_t maxGoals) {
    if (!outGoals || maxGoals == 0) return 0;

    JsonArray plays = doc["plays"];
    uint8_t goalCount = 0;
    for (JsonObject play : plays) {
        const char* type = play["typeDescKey"] | "";
        if (strcasecmp(type, "goal") != 0) continue;
        if (goalCount >= maxGoals) break;

        RecapGoal& g = outGoals[goalCount];
        g.eventId = play["eventId"] | 0;
        const int teamId = play["details"]["eventOwnerTeamId"] | 0;
        const char* teamAbbrev = teamAbbrevForId(teamId,
            (int)(doc["awayTeam"]["id"] | 0), awayAbbrev,
            (int)(doc["homeTeam"]["id"] | 0), homeAbbrev);
        sanitizeToken(teamAbbrev, g.teamAbbrev, sizeof(g.teamAbbrev));

        String scorerName = resolvePlayerName(
            play["details"]["scoringPlayerName"]["default"] | "",
            play["details"]["scoringPlayerId"] | 0);
        copyRecapName(scorerName, g.scorer, sizeof(g.scorer));

        String assist1 = resolvePlayerName(
            play["details"]["assist1PlayerName"]["default"] | "",
            play["details"]["assist1PlayerId"] | 0);
        copyRecapName(assist1, g.assist1, sizeof(g.assist1));

        String assist2 = resolvePlayerName(
            play["details"]["assist2PlayerName"]["default"] | "",
            play["details"]["assist2PlayerId"] | 0);
        copyRecapName(assist2, g.assist2, sizeof(g.assist2));

        sanitizeToken(play["timeRemaining"] | "", g.timeRemaining, sizeof(g.timeRemaining));
        g.period = play["periodDescriptor"]["number"] | 0;

        goalCount++;
    }

    return goalCount;
}

static void parseGoalEvent(JsonObject play, GoalInfo& goal) {
    goal.isNew = true;
    goal.type = play["typeDescKey"] | "";
    goal.time = play["timeRemaining"] | "";
    goal.period = play["periodDescriptor"]["number"] | 0;
    goal.eventId = play["eventId"] | 0;
    goal.ownerTeamId = play["details"]["eventOwnerTeamId"] | 0;
    goal.scoringPlayerId = play["details"]["scoringPlayerId"] | 0;
    
    // Resolve player names (API or roster cache)
    goal.scoringPlayerName = resolvePlayerName(
        play["details"]["scoringPlayerName"]["default"] | "",
        goal.scoringPlayerId
    );
    goal.shootingPlayerName = play["details"]["shootingPlayerName"]["default"] | "";
    goal.assist1Name = resolvePlayerName(
        play["details"]["assist1PlayerName"]["default"] | "",
        play["details"]["assist1PlayerId"] | 0
    );
    goal.assist2Name = resolvePlayerName(
        play["details"]["assist2PlayerName"]["default"] | "",
        play["details"]["assist2PlayerId"] | 0
    );
    
    goal.goalieName = play["details"]["goalieInNetName"]["default"] | "";
    goal.secondaryType = play["details"]["secondaryType"] | "";
    goal.shotType = play["details"]["shotType"] | "";
}

static void detectNewGoals(JsonArray plays, GoalInfo& goal) {
    if (plays.isNull() || plays.size() == 0) {
        state.hadEmptyFetch = true;
        return;
    }

    const int lastIdx = (int)plays.size() - 1;
    JsonObject lastPlay = plays[lastIdx];
    const int lastSortOrder = lastPlay["sortOrder"] | 0;

    // Prime on first fetch — just set watermark, don't queue old goals
    if (!state.primed) {
        state.lastPlaySortOrder = lastSortOrder;
        state.primed = true;
        return;
    }
    
    if (state.lastPlaySortOrder < 0) {
        state.lastPlaySortOrder = lastSortOrder;
        return;
    }
    
    // Check for ALL new goal events (not just the first)
    for (JsonObject play : plays) {
        const int sortOrder = play["sortOrder"] | 0;
        if (sortOrder <= state.lastPlaySortOrder) continue;
        
        const char* type = play["typeDescKey"] | "";
        if (String(type).equalsIgnoreCase("goal")) {
            GoalInfo g = {};
            parseGoalEvent(play, g);
            // Push each goal to the queue
            GoalQueueEntry entry{};
            entry.eventId = (uint32_t)g.eventId;
            entry.ownerTeamId = (uint32_t)g.ownerTeamId;
            strncpy(entry.scorer, g.scoringPlayerName.c_str(), sizeof(entry.scorer) - 1);
            strncpy(entry.assist1, g.assist1Name.c_str(), sizeof(entry.assist1) - 1);
            strncpy(entry.assist2, g.assist2Name.c_str(), sizeof(entry.assist2) - 1);
            strncpy(entry.time, g.time.c_str(), sizeof(entry.time) - 1);
            entry.period = (uint8_t)g.period;
            dataModelPushGoal(entry);
            Serial.printf("[pbp] GOAL queued: scorer='%s' a1='%s' a2='%s' eventId=%d\n",
                entry.scorer, entry.assist1, entry.assist2, g.eventId);
            // Keep track of last goal for the legacy single-goal field
            goal = g;
        }
    }
    
    state.lastPlaySortOrder = lastSortOrder;
}

// ============================================================================
// JSON FILTER SETUP
// ============================================================================

static void buildPlayByPlayFilter(JsonDocument& f) {
    f["gameType"] = true;
    f["seriesStatus"]["topSeedTeamAbbrev"] = true;
    f["seriesStatus"]["topSeedWins"] = true;
    f["seriesStatus"]["bottomSeedTeamAbbrev"] = true;
    f["seriesStatus"]["bottomSeedWins"] = true;
    f["gameState"] = true;
    f["startTimeUTC"] = true;
    f["easternUTCOffset"] = true;
    f["venueUTCOffset"] = true;
    f["periodDescriptor"]["number"] = true;
    f["clock"]["timeRemaining"] = true;
    f["clock"]["inIntermission"] = true;
    f["clock"]["running"] = true;
    f["homeTeam"]["score"] = true;
    f["homeTeam"]["sog"] = true;
    f["homeTeam"]["id"] = true;
    f["homeTeam"]["abbrev"] = true;
    f["homeTeam"]["commonName"]["default"] = true;
    f["homeTeam"]["placeName"]["default"] = true;
    f["awayTeam"]["score"] = true;
    f["awayTeam"]["sog"] = true;
    f["awayTeam"]["id"] = true;
    f["awayTeam"]["abbrev"] = true;
    f["awayTeam"]["commonName"]["default"] = true;
    f["awayTeam"]["placeName"]["default"] = true;
    f["situation"]["homeTeam"]["strength"] = true;
    f["situation"]["homeTeam"]["situationDescriptions"][0] = true;
    f["situation"]["awayTeam"]["strength"] = true;
    f["situation"]["awayTeam"]["situationDescriptions"][0] = true;
    JsonArray plays = f["plays"].to<JsonArray>();
    JsonObject p = plays.add<JsonObject>();
    p["typeDescKey"] = true;
    p["timeRemaining"] = true;
    p["periodDescriptor"]["number"] = true;
    p["eventId"] = true;
    p["sortOrder"] = true;
    JsonObject d = p["details"].to<JsonObject>();
    d["eventOwnerTeamId"] = true;
    d["scoringPlayerId"] = true;
    d["scoringPlayerName"]["default"] = true;
    d["shootingPlayerName"]["default"] = true;
    d["assist1PlayerName"]["default"] = true;
    d["assist2PlayerName"]["default"] = true;
    d["goalieInNetName"]["default"] = true;
    d["secondaryType"] = true;
    d["shotType"] = true;
    d["assist1PlayerId"] = true;
    d["assist2PlayerId"] = true;
    d["scoringPlayerId"] = true;

    JsonArray roster = f["rosterSpots"].to<JsonArray>();
    JsonObject r = roster.add<JsonObject>();
    r["playerId"] = true;
    r["firstName"]["default"] = true;
    r["lastName"]["default"] = true;
    r["lastName"]["default"] = true;
}

// ============================================================================
// HTTP REQUEST & PARSING
// ============================================================================

static DeserializationError fetchAndParseJson(const char* url, JsonDocument& doc, JsonDocument& filterDoc) {
    DeserializationError err = DeserializationError::InvalidInput;
    int code = -1;
    
    for (int attempt = 0; attempt < PBP_MAX_RETRIES; attempt++) {
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[pbp] WiFi not connected, aborting fetch");
            break;
        }
        playByPlayClient.stop();
        HTTPClient http;
        http.setTimeout(10000);
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

        if (!http.begin(playByPlayClient, url)) {
            Serial.printf("[pbp] attempt %d: http.begin failed\n", attempt + 1);
            if (attempt < PBP_MAX_RETRIES - 1) vTaskDelay(PBP_RETRY_BASE_MS / portTICK_PERIOD_MS);
            continue;
        }
        
        http.addHeader("User-Agent", "Mozilla/5.0 (compatible; Scoreboard/1.0)");
        code = http.GET();
        
        if (code != HTTP_CODE_OK) {
            Serial.printf("[pbp] attempt %d: GET code=%d\n", attempt + 1, code);
            http.end();
            if (attempt < PBP_MAX_RETRIES - 1) vTaskDelay(PBP_RETRY_BASE_MS / portTICK_PERIOD_MS);
            continue;
        }

        // Skip any garbage before JSON (max 256 bytes)
        Stream& s = *http.getStreamPtr();
        uint32_t start = millis();
        int c = -1;
        size_t skipped = 0;
        
        while ((millis() - start) < 5000 && skipped < 256) {
            if (s.available()) {
                c = s.read();
                if (c == '{') break;
                skipped++;
            } else {
                delay(1);
            }
        }
        
        if (c != '{') {
            err = DeserializationError::InvalidInput;
            Serial.printf("[pbp] no JSON start (skipped=%u)\n", (unsigned)skipped);
        } else {
            if (skipped > 0) {
                Serial.printf("[pbp] skipped=%u before JSON\n", (unsigned)skipped);
            }
            PrefixStream ps(s, '{');
            err = deserializeJson(doc, ps,
                DeserializationOption::Filter(filterDoc),
                DeserializationOption::NestingLimit(16));
        }
        
        http.end();
        playByPlayClient.stop();
        delay(50);
        
        if (!err) break;
        Serial.printf("[pbp] attempt %d: parse %s\n", attempt + 1, err.c_str());
        if (attempt < PBP_MAX_RETRIES - 1) vTaskDelay(PBP_RETRY_BASE_MS / portTICK_PERIOD_MS);
    }
    
    return err;
}

// ============================================================================
// MAIN FETCH & PROCESS
// ============================================================================

static bool fetchPlayByPlayOnce(uint32_t gameId) {
    if (gameId == 0) return false;
    
    char url[128];
    snprintf(url, sizeof(url), NHL_PBP_URL_FMT, (unsigned)gameId);

    Serial.printf("[pbp] fetch start game=%u\n", (unsigned)gameId);
    state.lastFetchMs = millis();
    
    // Prepare filter
    JsonDocument doc;
    static JsonDocument filterDoc;
    static bool filterReady = false;
    if (!filterReady) {
        buildPlayByPlayFilter(filterDoc);
        filterReady = true;
    }
    
    // Fetch and parse
    DeserializationError err = fetchAndParseJson(url, doc, filterDoc);
    if (err) {
        state.lastFailMs = millis();
        return false;
    }

    // Build roster cache if needed
    JsonArray roster = doc["rosterSpots"];
    if (gameId != rosterCache.gameId || rosterCache.count == 0) {
        buildRosterCache(roster, gameId);
    }

    // Build team names
    char awayName[64], homeName[64];
    buildFullName(awayName, sizeof(awayName),
        doc["awayTeam"]["placeName"]["default"] | "",
        doc["awayTeam"]["commonName"]["default"] | "");
    buildFullName(homeName, sizeof(homeName),
        doc["homeTeam"]["placeName"]["default"] | "",
        doc["homeTeam"]["commonName"]["default"] | "");
    
    const char* utcOffset = doc["easternUTCOffset"] | doc["venueUTCOffset"] | "";

    // Detect power play situation
    bool awayPP = false, homePP = false;
    JsonObject situation = doc["situation"];
    if (!situation.isNull()) {
        awayPP = String(situation["awayTeam"]["situationDescriptions"][0] | "").equalsIgnoreCase("PP");
        homePP = String(situation["homeTeam"]["situationDescriptions"][0] | "").equalsIgnoreCase("PP");
    }

    // Detect new goals (pushed to queue directly)
    GoalInfo goal = {};
    JsonArray plays = doc["plays"];
    detectNewGoals(plays, goal);

    const char* gameState = doc["gameState"] | "";
    const uint8_t gameType = doc["gameType"] | 0;
    char seriesTopSeedAbbrev[4];
    uint8_t seriesTopSeedWins = doc["seriesStatus"]["topSeedWins"] | 0;
    char seriesBottomSeedAbbrev[4];
    uint8_t seriesBottomSeedWins = doc["seriesStatus"]["bottomSeedWins"] | 0;
    copyShort(seriesTopSeedAbbrev, sizeof(seriesTopSeedAbbrev), doc["seriesStatus"]["topSeedTeamAbbrev"] | "");
    copyShort(seriesBottomSeedAbbrev, sizeof(seriesBottomSeedAbbrev), doc["seriesStatus"]["bottomSeedTeamAbbrev"] | "");

    // Fallback: PBP endpoint may not include seriesStatus. Fetch it from scoreboard/now.
    if (gameType == 3 && (!seriesTopSeedAbbrev[0] || !seriesBottomSeedAbbrev[0])) {
        const unsigned long now = millis();
        if (seriesCache.valid && seriesCache.gameId == gameId) {
            copyShort(seriesTopSeedAbbrev, sizeof(seriesTopSeedAbbrev), seriesCache.topSeedAbbrev);
            seriesTopSeedWins = seriesCache.topSeedWins;
            copyShort(seriesBottomSeedAbbrev, sizeof(seriesBottomSeedAbbrev), seriesCache.bottomSeedAbbrev);
            seriesBottomSeedWins = seriesCache.bottomSeedWins;
        } else if (now - seriesCache.lastAttemptMs > 30000UL) {
            seriesCache.lastAttemptMs = now;
            char top[4] = {0};
            char bottom[4] = {0};
            uint8_t topWins = 0;
            uint8_t bottomWins = 0;
            if (fetchSeriesStatusFromScoreboard(gameId, top, topWins, bottom, bottomWins)) {
                copyShort(seriesTopSeedAbbrev, sizeof(seriesTopSeedAbbrev), top);
                seriesTopSeedWins = topWins;
                copyShort(seriesBottomSeedAbbrev, sizeof(seriesBottomSeedAbbrev), bottom);
                seriesBottomSeedWins = bottomWins;

                seriesCache.gameId = gameId;
                copyShort(seriesCache.topSeedAbbrev, sizeof(seriesCache.topSeedAbbrev), top);
                seriesCache.topSeedWins = topWins;
                copyShort(seriesCache.bottomSeedAbbrev, sizeof(seriesCache.bottomSeedAbbrev), bottom);
                seriesCache.bottomSeedWins = bottomWins;
                seriesCache.valid = true;
                Serial.printf("[pbp] series fallback game=%u %s %u-%u %s\n",
                    (unsigned)gameId,
                    top,
                    (unsigned)topWins,
                    (unsigned)bottomWins,
                    bottom);
            }
        }
    }
    Serial.printf("[pbp] meta game=%u state=%s gameType=%u series=%s %u-%u %s\n",
        (unsigned)gameId,
        gameState,
        (unsigned)gameType,
        seriesTopSeedAbbrev[0] ? seriesTopSeedAbbrev : "-",
        (unsigned)seriesTopSeedWins,
        (unsigned)seriesBottomSeedWins,
        seriesBottomSeedAbbrev[0] ? seriesBottomSeedAbbrev : "-");
    const uint8_t period = doc["periodDescriptor"]["number"] | 0;
    const uint32_t awayId = doc["awayTeam"]["id"] | 0;
    const uint32_t homeId = doc["homeTeam"]["id"] | 0;
    const char* awayAbbrev = doc["awayTeam"]["abbrev"] | "";
    const char* homeAbbrev = doc["homeTeam"]["abbrev"] | "";
    const uint16_t awayScore = doc["awayTeam"]["score"] | 0;
    const uint16_t homeScore = doc["homeTeam"]["score"] | 0;
    const uint16_t awaySog = doc["awayTeam"]["sog"] | 0;
    const uint16_t homeSog = doc["homeTeam"]["sog"] | 0;

    bool recapReady = false;
    char recapText[kRecapTextMax] = {0};
    RecapGoal recapGoals[kMaxRecapGoals] = {};
    uint8_t recapGoalCount = 0;
    if (isFinalState(gameState)) {
        recapReady = true;
        recapGoalCount = buildRecapGoals(doc,
            awayAbbrev,
            homeAbbrev,
            recapGoals,
            kMaxRecapGoals);
    }

    // Update data model (goals are queued separately, not via goalIsNew)
    dataModelUpdateFromPbp(
        gameId,
        gameType,
        seriesTopSeedAbbrev,
        seriesTopSeedWins,
        seriesBottomSeedAbbrev,
        seriesBottomSeedWins,
        gameState,
        doc["startTimeUTC"] | "",
        utcOffset,
        period,
        doc["clock"]["timeRemaining"] | "",
        doc["clock"]["inIntermission"] | false,
        awayId,
        awayAbbrev,
        awayName,
        awayScore,
        awaySog,
        homeId,
        homeAbbrev,
        homeName,
        homeScore,
        homeSog,
        false, // goalIsNew — goals now go through the queue
        0,
        0,
        "",
        "",
        "",
        "",
        0,
        awayPP,
        homePP,
        recapReady,
        recapText,
        recapGoalCount,
        recapGoals
    );

    // Build API response
    JsonDocument out;
    JsonObject root = out.to<JsonObject>();
    root["gameId"] = gameId;
    root["gameType"] = gameType;
    root["gameState"] = doc["gameState"] | "";
    root["period"] = doc["periodDescriptor"]["number"] | 0;
    
    JsonObject clock = root["clock"].to<JsonObject>();
    clock["timeRemaining"] = doc["clock"]["timeRemaining"] | "";
    clock["inIntermission"] = doc["clock"]["inIntermission"] | false;
    clock["running"] = doc["clock"]["running"] | false;
    
    root["home"]["score"] = doc["homeTeam"]["score"] | 0;
    root["away"]["score"] = doc["awayTeam"]["score"] | 0;
    
    // Last play info
    if (!plays.isNull() && plays.size() > 0) {
        JsonObject lastPlay = plays[(int)plays.size() - 1];
        Serial.printf("[pbp] lastPlay type=%s period=%d time=%s\n",
            lastPlay["typeDescKey"] | "",
            (int)(lastPlay["periodDescriptor"]["number"] | 0),
            lastPlay["timeRemaining"] | "");
            
        JsonObject lp = root["lastPlay"].to<JsonObject>();
        lp["type"] = lastPlay["typeDescKey"] | "";
        lp["timeRemaining"] = lastPlay["timeRemaining"] | "";
        lp["period"] = lastPlay["periodDescriptor"]["number"] | 0;
        lp["eventId"] = lastPlay["eventId"] | 0;
        lp["sortOrder"] = lastPlay["sortOrder"] | 0;
        
        JsonObject ld = lp["details"].to<JsonObject>();
        ld["eventOwnerTeamId"] = lastPlay["details"]["eventOwnerTeamId"] | 0;
        ld["scoringPlayerId"] = lastPlay["details"]["scoringPlayerId"] | 0;
        ld["scoringPlayerName"] = lastPlay["details"]["scoringPlayerName"]["default"] | "";
        ld["shootingPlayerName"] = lastPlay["details"]["shootingPlayerName"]["default"] | "";
        ld["assist1PlayerName"] = lastPlay["details"]["assist1PlayerName"]["default"] | "";
        ld["assist2PlayerName"] = lastPlay["details"]["assist2PlayerName"]["default"] | "";
        ld["goalieInNetName"] = lastPlay["details"]["goalieInNetName"]["default"] | "";
        ld["secondaryType"] = lastPlay["details"]["secondaryType"] | "";
        ld["shotType"] = lastPlay["details"]["shotType"] | "";
    }
    
    // Goal info
    if (goal.isNew) {
        JsonObject lg = root["lastGoal"].to<JsonObject>();
        lg["type"] = goal.type;
        lg["timeRemaining"] = goal.time;
        lg["period"] = goal.period;
        lg["eventOwnerTeamId"] = goal.ownerTeamId;
        lg["scoringPlayerId"] = goal.scoringPlayerId;
        lg["scoringPlayerName"] = goal.scoringPlayerName;
        lg["shootingPlayerName"] = goal.shootingPlayerName;
        lg["assist1PlayerName"] = goal.assist1Name;
        lg["assist2PlayerName"] = goal.assist2Name;
        lg["goalieInNetName"] = goal.goalieName;
        lg["secondaryType"] = goal.secondaryType;
        lg["shotType"] = goal.shotType;
    }
    root["goalIsNew"] = goal.isNew;

    String serialized;
    serializeJson(out, serialized);
    if (pbpResponseMutex) xSemaphoreTake(pbpResponseMutex, portMAX_DELAY);
    state.lastGoodResponse = serialized;
    state.lastFetchMs = millis();
    state.lastFailMs = 0;
    if (pbpResponseMutex) xSemaphoreGive(pbpResponseMutex);
    Serial.printf("[pbp] fetch ok bytes=%u\n", (unsigned)serialized.length());
    return true;
}

// ============================================================================
// BACKGROUND TASK
// ============================================================================

static void playByPlayPollTask(void*) {
    for (;;) {
        uint32_t gameId = apiServerGetSelectedGameId();
        
        if (gameId == 0) {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }

        // Don't attempt fetch if WiFi is down
        if (WiFi.status() != WL_CONNECTED) {
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            continue;
        }
        
        // New game selected - reset state
        if (gameId != state.gameId) {
            state.gameId = gameId;
            state.lastGoodResponse = "";
            state.lastFailMs = 0;
            state.lastFetchMs = 0;
            state.lastPlaySortOrder = -1;
            state.primed = false;
            state.hadEmptyFetch = false;
            rosterCache.clear();
            seriesCache.clear();
            
            fetchPlayByPlayOnce(gameId);
            vTaskDelay(PBP_MIN_INTERVAL_MS / portTICK_PERIOD_MS);
            continue;
        }
        
        // Backoff after failure
        if (state.lastFailMs > 0) {
            unsigned long now = millis();
            if (now - state.lastFailMs < PBP_FAIL_BACKOFF_MS) {
                vTaskDelay(PBP_FAIL_BACKOFF_MS / portTICK_PERIOD_MS);
                continue;
            }
        }
        
        fetchPlayByPlayOnce(gameId);
        vTaskDelay(PBP_MIN_INTERVAL_MS / portTICK_PERIOD_MS);
    }
}

// ============================================================================
// API ENDPOINT HANDLER
// ============================================================================

static void handleApiPlayByPlay() {
    if (pbpResponseMutex) xSemaphoreTake(pbpResponseMutex, pdMS_TO_TICKS(100));
    if (state.lastGoodResponse.length() > 0) {
        playByPlayServer->send(200, "application/json", state.lastGoodResponse);
        if (pbpResponseMutex) xSemaphoreGive(pbpResponseMutex);
        return;
    }
    if (pbpResponseMutex) xSemaphoreGive(pbpResponseMutex);
    playByPlayServer->send(503, "application/json", "{\"error\":\"warming\"}");
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void playByPlayServiceInit(WebServer& server) {
    playByPlayServer = &server;
    playByPlayClient.setInsecure();
    playByPlayClient.setTimeout(10);
    seriesCache.clear();
    if (!pbpResponseMutex) pbpResponseMutex = xSemaphoreCreateMutex();
    
    playByPlayServer->on("/api/playbyplay", HTTP_GET, handleApiPlayByPlay);
    
    if (xTaskCreate(playByPlayPollTask, "pbp_poll", 20480, NULL, 1, NULL) != pdPASS) {
        Serial.println("Warn: pbp_poll task creation failed");
    }
}
