#include "schedule_service.h"

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <freertos/task.h>

#include "api_server.h"
#include "prefix_stream.h"

// ============================================================================
// CONSTANTS
// ============================================================================
static const char* NHL_SCHEDULE_URL = "https://api-web.nhle.com/v1/scoreboard/now";
static const unsigned long SCHEDULE_MIN_INTERVAL_MS = 30000;
static const unsigned long SCHEDULE_FAIL_BACKOFF_MS = 30000;
static const int SCHEDULE_MAX_RETRIES = 5;
static const unsigned long SCHEDULE_RETRY_BASE_MS = 700;

// ============================================================================
// DATA STRUCTURES
// ============================================================================
struct ScheduleState {
    String lastGoodResponse;
    unsigned long lastFetchMs;
    unsigned long lastFailMs;
    bool paused;
    
    void reset() {
        lastGoodResponse = "";
        lastFetchMs = 0;
        lastFailMs = 0;
        paused = false;
    }
};

// ============================================================================
// GLOBALS
// ============================================================================
static WebServer* scheduleServer = nullptr;
static WiFiClientSecure scheduleClient;
static ScheduleState state;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

static void buildTeamJson(const JsonObject& out, JsonObjectConst team) {
    out["abbrev"] = team["abbrev"] | "?";
    out["place"] = team["placeNameWithPreposition"]["default"] | "?";
    out["name"] = team["name"]["default"] | team["commonName"]["default"] | "?";
    out["score"] = team["score"] | 0;
    out["sog"] = team["sog"] | 0;
}

static void buildGameJson(const JsonObject& out, JsonObjectConst game, const char* date) {
    out["id"] = game["id"];
    out["date"] = date;
    out["startTimeUTC"] = game["startTimeUTC"] | "?";
    out["easternUTCOffset"] = game["easternUTCOffset"] | "";
    out["gameState"] = game["gameState"] | "?";
    
    buildTeamJson(out["away"].to<JsonObject>(), game["awayTeam"]);
    buildTeamJson(out["home"].to<JsonObject>(), game["homeTeam"]);
    
    out["period"] = game["periodDescriptor"]["number"] | 0;
    
    if (!game["clock"].isNull()) {
        JsonObject clock = out["clock"].to<JsonObject>();
        clock["timeRemaining"] = game["clock"]["timeRemaining"] | "";
        clock["inIntermission"] = game["clock"]["inIntermission"] | false;
        clock["running"] = game["clock"]["running"] | false;
    }
}

// ============================================================================
// JSON FILTER SETUP
// ============================================================================

static void buildScheduleFilter(JsonDocument& f) {
    f["focusedDate"] = true;
    JsonArray days = f["gamesByDate"].to<JsonArray>();
    JsonObject day = days.add<JsonObject>();
    day["date"] = true;
    JsonArray ga = day["games"].to<JsonArray>();
    JsonObject g = ga.add<JsonObject>();
    g["id"] = true;
    g["startTimeUTC"] = true;
    g["easternUTCOffset"] = true;
    g["gameState"] = true;
    g["awayTeam"]["abbrev"] = true;
    g["awayTeam"]["placeNameWithPreposition"]["default"] = true;
    g["awayTeam"]["commonName"]["default"] = true;
    g["awayTeam"]["name"]["default"] = true;
    g["awayTeam"]["score"] = true;
    g["awayTeam"]["sog"] = true;
    g["homeTeam"]["abbrev"] = true;
    g["homeTeam"]["placeNameWithPreposition"]["default"] = true;
    g["homeTeam"]["commonName"]["default"] = true;
    g["homeTeam"]["name"]["default"] = true;
    g["homeTeam"]["score"] = true;
    g["homeTeam"]["sog"] = true;
    g["periodDescriptor"]["number"] = true;
    g["clock"]["timeRemaining"] = true;
    g["clock"]["inIntermission"] = true;
    g["clock"]["running"] = true;
    g["clock"]["running"] = true;
}

// ============================================================================
// HTTP REQUEST & PARSING
// ============================================================================

static DeserializationError fetchAndParseJson(JsonDocument& doc, JsonDocument& filterDoc) {
    DeserializationError err = DeserializationError::InvalidInput;
    int code = -1;
    
    for (int attempt = 0; attempt < SCHEDULE_MAX_RETRIES; attempt++) {
        scheduleClient.stop();
        HTTPClient http;
        http.setTimeout(30000);
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

        if (!http.begin(scheduleClient, NHL_SCHEDULE_URL)) {
            Serial.printf("[schedule] attempt %d: http.begin failed\n", attempt + 1);
            if (attempt < SCHEDULE_MAX_RETRIES - 1) {
                delay(SCHEDULE_RETRY_BASE_MS * (1UL << attempt));
            }
            continue;
        }
        
        http.addHeader("User-Agent", "Mozilla/5.0 (compatible; Scoreboard/1.0)");
        code = http.GET();
        
        if (code != HTTP_CODE_OK) {
            Serial.printf("[schedule] attempt %d: GET code=%d\n", attempt + 1, code);
            http.end();
            if (attempt < SCHEDULE_MAX_RETRIES - 1) {
                delay(SCHEDULE_RETRY_BASE_MS * (1UL << attempt));
            }
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
            Serial.printf("[schedule] no JSON start (skipped=%u)\n", (unsigned)skipped);
        } else {
            if (skipped > 0) {
                Serial.printf("[schedule] skipped=%u before JSON\n", (unsigned)skipped);
            }
            PrefixStream ps(s, '{');
            err = deserializeJson(doc, ps,
                DeserializationOption::Filter(filterDoc),
                DeserializationOption::NestingLimit(16));
        }
        
        http.end();
        scheduleClient.stop();
        delay(50);
        
        if (!err) break;
        Serial.printf("[schedule] attempt %d: parse %s\n", attempt + 1, err.c_str());
        if (attempt < SCHEDULE_MAX_RETRIES - 1) {
            delay(SCHEDULE_RETRY_BASE_MS * (1UL << attempt));
        }
    }
    
    return err;
}

static JsonObject findTargetDay(JsonArray gamesByDate, const char* focusedDate) {
    if (focusedDate && focusedDate[0]) {
        for (JsonObject day : gamesByDate) {
            const char* date = day["date"] | "";
            if (strcmp(date, focusedDate) == 0) {
                return day;
            }
        }
    }
    // Fallback to first day
    return gamesByDate[0];
}

// ============================================================================
// MAIN FETCH & PROCESS
// ============================================================================

static bool fetchScheduleOnce() {
    Serial.printf("[schedule] fetch start @%lu\n", millis());
    state.lastFetchMs = millis();
    
    // Prepare filter
    JsonDocument doc;
    static JsonDocument filterDoc;
    static bool filterReady = false;
    if (!filterReady) {
        buildScheduleFilter(filterDoc);
        filterReady = true;
    }
    
    // Fetch and parse
    DeserializationError err = fetchAndParseJson(doc, filterDoc);
    if (err) {
        state.lastFailMs = millis();
        return false;
    }
    
    // Extract focused date and games
    const char* focusedDate = doc["focusedDate"] | "";
    JsonArray gamesByDate = doc["gamesByDate"];
    
    if (gamesByDate.isNull()) {
        Serial.println("[schedule] gamesByDate is null");
        state.lastFailMs = millis();
        return false;
    }
    
    Serial.printf("[schedule] focusedDate=%s days=%u\n",
        focusedDate[0] ? focusedDate : "(empty)",
        (unsigned)gamesByDate.size());
    
    if (gamesByDate.size() > 0) {
        const char* firstDate = gamesByDate[0]["date"] | "?";
        Serial.printf("[schedule] firstDate=%s\n", firstDate);
    }
    
    // Find target day (focused or first)
    JsonObject targetDay = findTargetDay(gamesByDate, focusedDate);
    if (targetDay.isNull()) {
        Serial.println("[schedule] no target day found");
        state.lastFailMs = millis();
        return false;
    }
    
    const char* targetDate = targetDay["date"] | "?";
    JsonArray games = targetDay["games"];
    
    Serial.printf("[schedule] targetDate=%s games=%u\n",
        targetDate, (unsigned)games.size());
    
    // Build output
    JsonDocument out;
    JsonArray outGames = out["games"].to<JsonArray>();
    
    for (JsonObject game : games) {
        JsonObject outGame = outGames.add<JsonObject>();
        buildGameJson(outGame, game, targetDate);
    }
    
    Serial.printf("[schedule] focused=%s games=%u\n",
        focusedDate[0] ? focusedDate : "(empty)", 
        (unsigned)games.size());
    
    serializeJson(out, state.lastGoodResponse);
    state.lastFetchMs = millis();
    state.lastFailMs = 0;
    
    Serial.printf("[schedule] fetch ok bytes=%u\n", 
        (unsigned)state.lastGoodResponse.length());
    return true;
}

// ============================================================================
// BACKGROUND TASK
// ============================================================================

static void schedulePollTask(void*) {
    for (;;) {
        // Pause when a game is selected
        if (apiServerGetSelectedGameId() != 0) {
            if (!state.paused) {
                Serial.println("[schedule] paused (game selected)");
                state.paused = true;
            }
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }
        
        if (state.paused) {
            Serial.println("[schedule] resumed (no game selected)");
            state.paused = false;
        }
        
        // Backoff after failure
        if (state.lastFailMs > 0) {
            unsigned long now = millis();
            if (now - state.lastFailMs < SCHEDULE_FAIL_BACKOFF_MS) {
                vTaskDelay(SCHEDULE_FAIL_BACKOFF_MS / portTICK_PERIOD_MS);
                continue;
            }
        }
        
        fetchScheduleOnce();
        vTaskDelay(SCHEDULE_MIN_INTERVAL_MS / portTICK_PERIOD_MS);
    }
}

// ============================================================================
// API ENDPOINT HANDLER
// ============================================================================

static void handleApiSchedule() {
    if (state.lastGoodResponse.length() > 0) {
        scheduleServer->send(200, "application/json", state.lastGoodResponse);
        return;
    }
    scheduleServer->send(503, "application/json", "{\"error\":\"warming\"}");
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void scheduleServiceInit(WebServer& server) {
    scheduleServer = &server;
    scheduleClient.setInsecure();
    scheduleClient.setTimeout(30);
    
    scheduleServer->on("/api/schedule", HTTP_GET, handleApiSchedule);
    
    if (xTaskCreate(schedulePollTask, "sched_poll", 16384, NULL, 1, NULL) != pdPASS) {
        Serial.println("Warn: sched_poll task creation failed");
    }
}
