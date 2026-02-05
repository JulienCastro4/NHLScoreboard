#include "schedule_service.h"

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <freertos/task.h>

#include "api_server.h"
#include "prefix_stream.h"

static WebServer* scheduleServer = nullptr;

static const char* NHL_SCHEDULE_URL = "https://api-web.nhle.com/v1/scoreboard/now";

// /api/schedule: fetch en tache de fond pour limiter TLS/heap.
static struct { int code; String body; } schedResult;
static String lastGoodSchedule;
static unsigned long lastScheduleFetchMs = 0;
static const unsigned long scheduleMinIntervalMs = 30000;
static unsigned long lastScheduleFailMs = 0;
static const unsigned long scheduleFailBackoffMs = 30000;

static WiFiClientSecure scheduleClient;

static const int scheduleMaxRetries = 5;
static const unsigned long scheduleRetryBaseMs = 700;

// Filtre: scoreboard/now avec champs utiles seulement.
// "gamesByDate" et "games" doivent etre des JsonArray pour allowArray()=true.
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
}

static bool fetchScheduleOnce() {
    Serial.printf("[schedule] fetch start @%lu\n", millis());
    lastScheduleFetchMs = millis();
    int code = -1;
    JsonDocument doc;
    static JsonDocument filterDoc;
    static bool filterOk;
    if (!filterOk) {
        buildScheduleFilter(filterDoc);
        filterOk = true;
    }
    DeserializationError err = DeserializationError::InvalidInput;

    for (int attempt = 0; attempt < scheduleMaxRetries; attempt++) {
        scheduleClient.stop();

        HTTPClient http;
        http.setTimeout(30000);
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

        if (!http.begin(scheduleClient, NHL_SCHEDULE_URL)) {
            Serial.printf("[schedule] attempt %d: http.begin failed\n", attempt + 1);
            if (attempt < scheduleMaxRetries - 1) delay(scheduleRetryBaseMs * (1UL << attempt));
            continue;
        }
        http.addHeader("User-Agent", "Mozilla/5.0 (compatible; Scoreboard/1.0)");

        code = http.GET();
        if (code != HTTP_CODE_OK) {
            Serial.printf("[schedule] attempt %d: GET code=%d\n", attempt + 1, code);
            http.end();
            if (attempt < scheduleMaxRetries - 1) delay(scheduleRetryBaseMs * (1UL << attempt));
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
            Serial.printf("[schedule] no JSON start (skipped=%u)\n", (unsigned)skipped);
        } else {
            if (skipped > 0)
                Serial.printf("[schedule] skipped=%u before JSON\n", (unsigned)skipped);
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
        if (attempt < scheduleMaxRetries - 1) delay(scheduleRetryBaseMs * (1UL << attempt));
    }

    if (code != HTTP_CODE_OK) {
        lastScheduleFailMs = millis();
        return false;
    }
    if (err) {
        lastScheduleFailMs = millis();
        return false;
    }

    const char* focused = doc["focusedDate"] | "";
    JsonArray gamesByDate = doc["gamesByDate"];
    if (gamesByDate.isNull()) {
        Serial.println("[schedule] gamesByDate is null");
        lastScheduleFailMs = millis();
        return false;
    }

    JsonDocument out;
    JsonObject root = out.to<JsonObject>();
    JsonArray arr = root["games"].to<JsonArray>();

    Serial.printf("[schedule] focusedDate=%s days=%u\n",
        focused[0] ? focused : "(empty)",
        (unsigned)gamesByDate.size());
    if (gamesByDate.size() > 0) {
        const char* firstDate = gamesByDate[0]["date"] | "?";
        Serial.printf("[schedule] firstDate=%s\n", firstDate);
    }

    JsonObject target;
    if (focused[0]) {
        for (JsonObject w : gamesByDate) {
            const char* date = w["date"] | "";
            if (strcmp(date, focused) == 0) {
                target = w;
                break;
            }
        }
    }
    if (target.isNull())
        target = gamesByDate[0];

    unsigned int totalGames = 0;
    if (!target.isNull()) {
        const char* date = target["date"] | "?";
        JsonArray games = target["games"];
        Serial.printf("[schedule] targetDate=%s games=%u\n",
            date, (unsigned)games.size());
        for (JsonObject g : games) {
            JsonObject o = arr.add<JsonObject>();
            o["id"] = g["id"];
            o["date"] = date;
            o["startTimeUTC"] = g["startTimeUTC"] | "?";
            o["easternUTCOffset"] = g["easternUTCOffset"] | "";
            o["gameState"] = g["gameState"] | "?";

            JsonObject at = g["awayTeam"].as<JsonObject>();
            JsonObject ht = g["homeTeam"].as<JsonObject>();

            JsonObject ao = o["away"].to<JsonObject>();
            ao["abbrev"] = at["abbrev"] | "?";
            ao["place"] = at["placeNameWithPreposition"]["default"] | "?";
            ao["name"] = at["name"]["default"] | at["commonName"]["default"] | "?";
            ao["score"] = at["score"] | 0;
            ao["sog"] = at["sog"] | 0;

            JsonObject ho = o["home"].to<JsonObject>();
            ho["abbrev"] = ht["abbrev"] | "?";
            ho["place"] = ht["placeNameWithPreposition"]["default"] | "?";
            ho["name"] = ht["name"]["default"] | ht["commonName"]["default"] | "?";
            ho["score"] = ht["score"] | 0;
            ho["sog"] = ht["sog"] | 0;

            if (!g["periodDescriptor"].isNull())
                o["period"] = g["periodDescriptor"]["number"] | 0;
            else
                o["period"] = 0;
            if (!g["clock"].isNull()) {
                JsonObject co = o["clock"].to<JsonObject>();
                co["timeRemaining"] = g["clock"]["timeRemaining"] | "";
                co["inIntermission"] = g["clock"]["inIntermission"] | false;
                co["running"] = g["clock"]["running"] | false;
            }
            totalGames++;
        }
    }
    Serial.printf("[schedule] focused=%s games=%u\n",
        focused[0] ? focused : "(empty)", totalGames);

    serializeJson(out, schedResult.body);
    schedResult.code = 200;
    lastGoodSchedule = schedResult.body;
    lastScheduleFetchMs = millis();
    lastScheduleFailMs = 0;
    Serial.printf("[schedule] fetch ok bytes=%u\n", (unsigned)lastGoodSchedule.length());
    return true;
}

static void schedulePollTask(void*) {
    bool paused = false;
    for (;;) {
        if (apiServerGetSelectedGameId() != 0) {
            if (!paused) {
                Serial.println("[schedule] paused (game selected)");
                paused = true;
            }
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }
        if (paused) {
            Serial.println("[schedule] resumed (no game selected)");
            paused = false;
        }
        if (lastScheduleFailMs > 0) {
            unsigned long now = millis();
            if (now - lastScheduleFailMs < scheduleFailBackoffMs) {
                vTaskDelay(scheduleFailBackoffMs / portTICK_PERIOD_MS);
                continue;
            }
        }
        fetchScheduleOnce();
        vTaskDelay(scheduleMinIntervalMs / portTICK_PERIOD_MS);
    }
}

static void handleApiSchedule() {
    if (lastGoodSchedule.length() > 0) {
        scheduleServer->send(200, "application/json", lastGoodSchedule);
        return;
    }
    scheduleServer->send(503, "application/json", "{\"error\":\"warming\"}");
}

void scheduleServiceInit(WebServer& server) {
    scheduleServer = &server;
    scheduleClient.setInsecure();
    scheduleClient.setTimeout(30);
    scheduleServer->on("/api/schedule", HTTP_GET, handleApiSchedule);
    if (xTaskCreate(schedulePollTask, "sched_poll", 16384, NULL, 1, NULL) != pdPASS) {
        Serial.println("Warn: sched_poll task");
    }
}
