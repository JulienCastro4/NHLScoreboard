#include "playbyplay_service.h"

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <freertos/task.h>

#include "api_server.h"
#include "display/data_model.h"
#include "prefix_stream.h"

// ============================================================================
// CONSTANTS
// ============================================================================
static const char* NHL_PBP_URL_FMT = "https://api-web.nhle.com/v1/gamecenter/%u/play-by-play";
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

// ============================================================================
// GLOBALS
// ============================================================================
static WebServer* playByPlayServer = nullptr;
static WiFiClientSecure playByPlayClient;
static PbpState state;
static RosterCache rosterCache;

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
    if (plays.isNull() || plays.size() == 0) return;
    
    const int lastIdx = (int)plays.size() - 1;
    JsonObject lastPlay = plays[lastIdx];
    const int lastSortOrder = lastPlay["sortOrder"] | 0;
    
    // Prime on first fetch
    if (!state.primed) {
        state.lastPlaySortOrder = lastSortOrder;
        state.primed = true;
        return;
    }
    
    if (state.lastPlaySortOrder < 0) {
        state.lastPlaySortOrder = lastSortOrder;
        return;
    }
    
    // Check for new goal events
    for (JsonObject play : plays) {
        const int sortOrder = play["sortOrder"] | 0;
        if (sortOrder <= state.lastPlaySortOrder) continue;
        
        const char* type = play["typeDescKey"] | "";
        if (String(type).equalsIgnoreCase("goal")) {
            parseGoalEvent(play, goal);
            break; // Only process first new goal
        }
    }
    
    state.lastPlaySortOrder = lastSortOrder;
    state.lastPlaySortOrder = lastSortOrder;
}

// ============================================================================
// JSON FILTER SETUP
// ============================================================================

static void buildPlayByPlayFilter(JsonDocument& f) {
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
        playByPlayClient.stop();
        HTTPClient http;
        http.setTimeout(30000);
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

        if (!http.begin(playByPlayClient, url)) {
            Serial.printf("[pbp] attempt %d: http.begin failed\n", attempt + 1);
            if (attempt < PBP_MAX_RETRIES - 1) delay(PBP_RETRY_BASE_MS);
            continue;
        }
        
        http.addHeader("User-Agent", "Mozilla/5.0 (compatible; Scoreboard/1.0)");
        code = http.GET();
        
        if (code != HTTP_CODE_OK) {
            Serial.printf("[pbp] attempt %d: GET code=%d\n", attempt + 1, code);
            http.end();
            if (attempt < PBP_MAX_RETRIES - 1) delay(PBP_RETRY_BASE_MS);
            continue;
        }

        // Skip any garbage before JSON
        Stream& s = *http.getStreamPtr();
        uint32_t start = millis();
        int c = -1;
        size_t skipped = 0;
        
        while ((millis() - start) < 5000) {
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
        if (attempt < PBP_MAX_RETRIES - 1) delay(PBP_RETRY_BASE_MS);
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

    // Detect new goals
    GoalInfo goal = {};
    JsonArray plays = doc["plays"];
    detectNewGoals(plays, goal);

    // Update data model
    dataModelUpdateFromPbp(
        gameId,
        doc["gameState"] | "",
        doc["startTimeUTC"] | "",
        utcOffset,
        doc["periodDescriptor"]["number"] | 0,
        doc["clock"]["timeRemaining"] | "",
        doc["clock"]["inIntermission"] | false,
        doc["awayTeam"]["id"] | 0,
        doc["awayTeam"]["abbrev"] | "",
        awayName,
        doc["awayTeam"]["score"] | 0,
        doc["awayTeam"]["sog"] | 0,
        doc["homeTeam"]["id"] | 0,
        doc["homeTeam"]["abbrev"] | "",
        homeName,
        doc["homeTeam"]["score"] | 0,
        doc["homeTeam"]["sog"] | 0,
        goal.isNew,
        (uint32_t)goal.eventId,
        (uint32_t)goal.ownerTeamId,
        goal.scoringPlayerName.c_str(),
        goal.assist1Name.c_str(),
        goal.assist2Name.c_str(),
        goal.time.c_str(),
        (uint8_t)goal.period,
        awayPP,
        homePP
    );

    // Build API response
    JsonDocument out;
    JsonObject root = out.to<JsonObject>();
    root["gameId"] = gameId;
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

    serializeJson(out, state.lastGoodResponse);
    state.lastFetchMs = millis();
    state.lastFailMs = 0;
    Serial.printf("[pbp] fetch ok bytes=%u\n", (unsigned)state.lastGoodResponse.length());
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
        
        // New game selected - reset state
        if (gameId != state.gameId) {
            state.gameId = gameId;
            state.lastGoodResponse = "";
            state.lastFailMs = 0;
            state.lastFetchMs = 0;
            state.lastPlaySortOrder = -1;
            state.primed = false;
            rosterCache.clear();
            
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
    if (state.lastGoodResponse.length() > 0) {
        playByPlayServer->send(200, "application/json", state.lastGoodResponse);
        return;
    }
    playByPlayServer->send(503, "application/json", "{\"error\":\"warming\"}");
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void playByPlayServiceInit(WebServer& server) {
    playByPlayServer = &server;
    playByPlayClient.setInsecure();
    playByPlayClient.setTimeout(30);
    
    playByPlayServer->on("/api/playbyplay", HTTP_GET, handleApiPlayByPlay);
    
    if (xTaskCreate(playByPlayPollTask, "pbp_poll", 16384, NULL, 1, NULL) != pdPASS) {
        Serial.println("Warn: pbp_poll task creation failed");
    }
}
