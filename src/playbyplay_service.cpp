#include "playbyplay_service.h"

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <freertos/task.h>

#include "api_server.h"
#include "display/data_model.h"
#include "prefix_stream.h"

static WebServer* playByPlayServer = nullptr;

static const char* NHL_PBP_URL_FMT = "https://api-web.nhle.com/v1/gamecenter/%u/play-by-play";

// /api/playbyplay: fetch en tache de fond quand un match est selectionne.
static String lastGoodPlayByPlay;
static unsigned long lastPbpFetchMs = 0;
static const unsigned long pbpMinIntervalMs = 10000;
static unsigned long lastPbpFailMs = 0;
static const unsigned long pbpFailBackoffMs = 20000;
static uint32_t lastPbpGameId = 0;
static int lastPbpPlaySortOrder = -1;
static bool pbpPrimed = false;
static uint32_t rosterGameId = 0;
static size_t rosterCount = 0;

struct PlayerEntry {
    int id;
    char name[32];
};
static PlayerEntry rosterCache[80];

static WiFiClientSecure playByPlayClient;

static const int pbpMaxRetries = 3;
static const unsigned long pbpRetryBaseMs = 1000;

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
}

static void buildRosterCache(JsonArray roster, uint32_t gameId) {
    rosterCount = 0;
    rosterGameId = gameId;
    if (roster.isNull()) return;
    for (JsonObject p : roster) {
        if (rosterCount >= (sizeof(rosterCache) / sizeof(rosterCache[0]))) break;
        int id = p["playerId"] | 0;
        const char* first = p["firstName"]["default"] | "";
        const char* last = p["lastName"]["default"] | "";
        if (id == 0) continue;
        rosterCache[rosterCount].id = id;
        if (first[0] && last[0])
            snprintf(rosterCache[rosterCount].name, sizeof(rosterCache[rosterCount].name), "%s %s", first, last);
        else
            snprintf(rosterCache[rosterCount].name, sizeof(rosterCache[rosterCount].name), "%s%s", first, last);
        rosterCount++;
    }
}

static const char* lookupPlayerName(int id) {
    if (id == 0) return "";
    for (size_t i = 0; i < rosterCount; ++i) {
        if (rosterCache[i].id == id) return rosterCache[i].name;
    }
    return "";
}

static bool fetchPlayByPlayOnce(uint32_t gameId) {
    if (gameId == 0) return false;
    char url[128];
    snprintf(url, sizeof(url), NHL_PBP_URL_FMT, (unsigned)gameId);

    Serial.printf("[pbp] fetch start game=%u\n", (unsigned)gameId);
    lastPbpFetchMs = millis();
    int code = -1;
    JsonDocument doc;
    static JsonDocument filterDoc;
    static bool filterOk;
    if (!filterOk) {
        buildPlayByPlayFilter(filterDoc);
        filterOk = true;
    }
    DeserializationError err = DeserializationError::InvalidInput;

    for (int attempt = 0; attempt < pbpMaxRetries; attempt++) {
        playByPlayClient.stop();
        HTTPClient http;
        http.setTimeout(30000);
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

        if (!http.begin(playByPlayClient, url)) {
            Serial.printf("[pbp] attempt %d: http.begin failed\n", attempt + 1);
            if (attempt < pbpMaxRetries - 1) delay(pbpRetryBaseMs);
            continue;
        }
        http.addHeader("User-Agent", "Mozilla/5.0 (compatible; Scoreboard/1.0)");
        code = http.GET();
        if (code != HTTP_CODE_OK) {
            Serial.printf("[pbp] attempt %d: GET code=%d\n", attempt + 1, code);
            http.end();
            if (attempt < pbpMaxRetries - 1) delay(pbpRetryBaseMs);
            continue;
        }

        Stream& s = *http.getStreamPtr();
        uint32_t start = millis();
        int c = -1;
        size_t skipped = 0;
        while ((millis() - start) < 5000) {
            if (s.available()) {
                c = s.read();
                if (c == '{')
                    break;
                skipped++;
            } else {
                delay(1);
            }
        }
        if (c != '{') {
            err = DeserializationError::InvalidInput;
            Serial.printf("[pbp] no JSON start (skipped=%u)\n", (unsigned)skipped);
        } else {
            if (skipped > 0)
                Serial.printf("[pbp] skipped=%u before JSON\n", (unsigned)skipped);
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
        if (attempt < pbpMaxRetries - 1) delay(pbpRetryBaseMs);
    }

    if (code != HTTP_CODE_OK || err) {
        lastPbpFailMs = millis();
        return false;
    }

    JsonDocument out;
    JsonObject root = out.to<JsonObject>();
    root["gameId"] = gameId;
    root["gameState"] = doc["gameState"] | "";
    root["period"] = doc["periodDescriptor"]["number"] | 0;
    JsonObject clock = root["clock"].to<JsonObject>();
    clock["timeRemaining"] = doc["clock"]["timeRemaining"] | "";
    clock["inIntermission"] = doc["clock"]["inIntermission"] | false;
    clock["running"] = doc["clock"]["running"] | false;
    JsonObject home = root["home"].to<JsonObject>();
    home["score"] = doc["homeTeam"]["score"] | 0;
    JsonObject away = root["away"].to<JsonObject>();
    away["score"] = doc["awayTeam"]["score"] | 0;

    const char* awayPlace = doc["awayTeam"]["placeName"]["default"] | "";
    const char* awayCommon = doc["awayTeam"]["commonName"]["default"] | "";
    const char* homePlace = doc["homeTeam"]["placeName"]["default"] | "";
    const char* homeCommon = doc["homeTeam"]["commonName"]["default"] | "";
    char awayName[64];
    char homeName[64];
    if (awayPlace[0] && awayCommon[0])
        snprintf(awayName, sizeof(awayName), "%s %s", awayPlace, awayCommon);
    else
        snprintf(awayName, sizeof(awayName), "%s%s", awayPlace, awayCommon);
    if (homePlace[0] && homeCommon[0])
        snprintf(homeName, sizeof(homeName), "%s %s", homePlace, homeCommon);
    else
        snprintf(homeName, sizeof(homeName), "%s%s", homePlace, homeCommon);

    const char* utcOffset = doc["easternUTCOffset"] | doc["venueUTCOffset"] | "";

    JsonArray roster = doc["rosterSpots"];
    if (gameId != rosterGameId || rosterCount == 0) {
        buildRosterCache(roster, gameId);
    }

    JsonArray plays = doc["plays"];
    bool goalIsNew = false;
    String goalType;
    String goalTime;
    int goalPeriod = 0;
    int goalOwnerTeamId = 0;
    int goalEventId = 0;
    int goalScoringPlayerId = 0;
    String goalScoringPlayerName;
    String goalShootingPlayerName;
    String goalAssist1Name;
    String goalAssist2Name;
    String goalGoalieName;
    String goalSecondaryType;
    String goalShotType;
    bool awayPP = false;
    bool homePP = false;

    JsonObject situation = doc["situation"];
    if (!situation.isNull()) {
        const char* awayDesc = situation["awayTeam"]["situationDescriptions"][0] | "";
        const char* homeDesc = situation["homeTeam"]["situationDescriptions"][0] | "";
        awayPP = (String(awayDesc).equalsIgnoreCase("PP"));
        homePP = (String(homeDesc).equalsIgnoreCase("PP"));
    }

    if (!plays.isNull() && plays.size() > 0) {
        const int lastIdx = (int)plays.size() - 1;
        JsonObject last = plays[lastIdx];
        Serial.printf("[pbp] lastPlay type=%s period=%d time=%s\n",
            last["typeDescKey"] | "",
            (int)(last["periodDescriptor"]["number"] | 0),
            last["timeRemaining"] | "");
        JsonObject lp = root["lastPlay"].to<JsonObject>();
        lp["type"] = last["typeDescKey"] | "";
        lp["timeRemaining"] = last["timeRemaining"] | "";
        lp["period"] = last["periodDescriptor"]["number"] | 0;
        lp["eventId"] = last["eventId"] | 0;
        lp["sortOrder"] = last["sortOrder"] | 0;
        JsonObject ld = lp["details"].to<JsonObject>();
        ld["eventOwnerTeamId"] = last["details"]["eventOwnerTeamId"] | 0;
        ld["scoringPlayerId"] = last["details"]["scoringPlayerId"] | 0;
        ld["scoringPlayerName"] = last["details"]["scoringPlayerName"]["default"] | "";
        ld["shootingPlayerName"] = last["details"]["shootingPlayerName"]["default"] | "";
        ld["assist1PlayerName"] = last["details"]["assist1PlayerName"]["default"] | "";
        ld["assist2PlayerName"] = last["details"]["assist2PlayerName"]["default"] | "";
        ld["goalieInNetName"] = last["details"]["goalieInNetName"]["default"] | "";
        ld["secondaryType"] = last["details"]["secondaryType"] | "";
        ld["shotType"] = last["details"]["shotType"] | "";

        const int lastSortOrder = last["sortOrder"] | 0;
        if (!pbpPrimed) {
            lastPbpPlaySortOrder = lastSortOrder;
            pbpPrimed = true;
        } else if (lastPbpPlaySortOrder < 0) {
            lastPbpPlaySortOrder = lastSortOrder;
        } else {
            for (JsonObject p : plays) {
                const int sortOrder = p["sortOrder"] | 0;
                if (sortOrder <= lastPbpPlaySortOrder) continue;
                const char* type = p["typeDescKey"] | "";
                if (String(type).equalsIgnoreCase("goal")) {
                    goalIsNew = true;
                    goalType = type;
                    goalTime = p["timeRemaining"] | "";
                    goalPeriod = p["periodDescriptor"]["number"] | 0;
                    goalEventId = p["eventId"] | 0;
                    goalOwnerTeamId = p["details"]["eventOwnerTeamId"] | 0;
                    goalScoringPlayerId = p["details"]["scoringPlayerId"] | 0;
                    goalScoringPlayerName = p["details"]["scoringPlayerName"]["default"] | "";
                    goalShootingPlayerName = p["details"]["shootingPlayerName"]["default"] | "";
                    goalAssist1Name = p["details"]["assist1PlayerName"]["default"] | "";
                    goalAssist2Name = p["details"]["assist2PlayerName"]["default"] | "";
                    goalGoalieName = p["details"]["goalieInNetName"]["default"] | "";
                    goalSecondaryType = p["details"]["secondaryType"] | "";
                    goalShotType = p["details"]["shotType"] | "";

                    if (goalScoringPlayerName.length() == 0) {
                        const char* mapped = lookupPlayerName(goalScoringPlayerId);
                        if (mapped[0]) goalScoringPlayerName = mapped;
                    }
                    if (goalAssist1Name.length() == 0) {
                        const char* mapped = lookupPlayerName(p["details"]["assist1PlayerId"] | 0);
                        if (mapped[0]) goalAssist1Name = mapped;
                    }
                    if (goalAssist2Name.length() == 0) {
                        const char* mapped = lookupPlayerName(p["details"]["assist2PlayerId"] | 0);
                        if (mapped[0]) goalAssist2Name = mapped;
                    }
                }
            }
            lastPbpPlaySortOrder = lastSortOrder;
        }
    }

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
        goalIsNew,
        (uint32_t)goalEventId,
        (uint32_t)goalOwnerTeamId,
        goalScoringPlayerName.c_str(),
        goalAssist1Name.c_str(),
        goalAssist2Name.c_str(),
        goalTime.c_str(),
        (uint8_t)goalPeriod,
        awayPP,
        homePP);

    if (goalIsNew) {
        JsonObject lg = root["lastGoal"].to<JsonObject>();
        lg["type"] = goalType;
        lg["timeRemaining"] = goalTime;
        lg["period"] = goalPeriod;
        lg["eventOwnerTeamId"] = goalOwnerTeamId;
        lg["scoringPlayerId"] = goalScoringPlayerId;
        lg["scoringPlayerName"] = goalScoringPlayerName;
        lg["shootingPlayerName"] = goalShootingPlayerName;
        lg["assist1PlayerName"] = goalAssist1Name;
        lg["assist2PlayerName"] = goalAssist2Name;
        lg["goalieInNetName"] = goalGoalieName;
        lg["secondaryType"] = goalSecondaryType;
        lg["shotType"] = goalShotType;
    }
    root["goalIsNew"] = goalIsNew;

    serializeJson(out, lastGoodPlayByPlay);
    lastPbpFetchMs = millis();
    lastPbpFailMs = 0;
    Serial.printf("[pbp] fetch ok bytes=%u\n", (unsigned)lastGoodPlayByPlay.length());
    return true;
}

static void playByPlayPollTask(void*) {
    for (;;) {
        uint32_t gameId = apiServerGetSelectedGameId();
        if (gameId == 0) {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }
        if (gameId != lastPbpGameId) {
            lastPbpGameId = gameId;
            lastGoodPlayByPlay = "";
            lastPbpFailMs = 0;
            lastPbpFetchMs = 0;
            lastPbpPlaySortOrder = -1;
            pbpPrimed = false;
            rosterGameId = 0;
            rosterCount = 0;
            fetchPlayByPlayOnce(gameId);
            vTaskDelay(pbpMinIntervalMs / portTICK_PERIOD_MS);
            continue;
        }
        if (lastPbpFailMs > 0) {
            unsigned long now = millis();
            if (now - lastPbpFailMs < pbpFailBackoffMs) {
                vTaskDelay(pbpFailBackoffMs / portTICK_PERIOD_MS);
                continue;
            }
        }
        fetchPlayByPlayOnce(gameId);
        vTaskDelay(pbpMinIntervalMs / portTICK_PERIOD_MS);
    }
}

static void handleApiPlayByPlay() {
    if (lastGoodPlayByPlay.length() > 0) {
        playByPlayServer->send(200, "application/json", lastGoodPlayByPlay);
        return;
    }
    playByPlayServer->send(503, "application/json", "{\"error\":\"warming\"}");
}

void playByPlayServiceInit(WebServer& server) {
    playByPlayServer = &server;
    playByPlayClient.setInsecure();
    playByPlayClient.setTimeout(30);
    playByPlayServer->on("/api/playbyplay", HTTP_GET, handleApiPlayByPlay);
    if (xTaskCreate(playByPlayPollTask, "pbp_poll", 16384, NULL, 1, NULL) != pdPASS) {
        Serial.println("Warn: pbp_poll task");
    }
}
