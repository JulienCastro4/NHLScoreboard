#include "api_server.h"

#include <Arduino.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

#include "schedule_service.h"
#include "playbyplay_service.h"
#include "display/data_model.h"
#include "display/display_manager.h"
static WebServer server(80);
static uint32_t selectedGameId = 0;
static const char* CONFIG_PATH = "/scoreboard.json";

static void loadSelectedGameId() {
    if (!LittleFS.exists(CONFIG_PATH)) {
        selectedGameId = 0;
        return;
    }
    File f = LittleFS.open(CONFIG_PATH, "r");
    if (!f) {
        selectedGameId = 0;
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, f)) {
        selectedGameId = 0;
        f.close();
        return;
    }
    selectedGameId = doc["gameId"] | 0;
    f.close();
}

static void saveSelectedGameId(uint32_t id) {
    File f = LittleFS.open(CONFIG_PATH, "w");
    if (!f) return;
    JsonDocument doc;
    doc["gameId"] = id;
    serializeJson(doc, f);
    f.close();
}

static void serveFile(const char* path, const char* contentType) {
    if (!LittleFS.exists(path)) {
        server.send(404, "text/plain", "Fichier non trouvé");
        return;
    }
    File f = LittleFS.open(path, "r");
    if (!f) {
        server.send(500, "text/plain", "Erreur lecture");
        return;
    }
    server.streamFile(f, contentType);
    f.close();
}

static void handleRoot() {
    serveFile("/index.html", "text/html");
}

static void handleApiSelectGame() {
    if (server.method() != HTTP_POST) {
        server.send(405, "application/json", "{\"error\":\"method\"}");
        return;
    }
    String body = server.arg("plain");
    if (body.length() == 0) {
        server.send(400, "application/json", "{\"error\":\"body\"}");
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        server.send(400, "application/json", "{\"error\":\"json\"}");
        return;
    }
    uint32_t id = doc["gameId"] | 0;
    selectedGameId = id;
    saveSelectedGameId(id);
    dataModelSetSelectedGame(id);
    Serial.printf("[api] select gameId=%u\n", (unsigned)id);

    server.send(200, "application/json", "{}");
}

static void handleApiSelectedGame() {
    JsonDocument doc;
    doc["gameId"] = selectedGameId;
    String resp;
    serializeJson(doc, resp);
    server.send(200, "application/json", resp);
}

static void handleApiDisplayPower() {
    if (server.method() == HTTP_GET) {
        JsonDocument doc;
        doc["enabled"] = displayIsEnabled();
        String resp;
        serializeJson(doc, resp);
        server.send(200, "application/json", resp);
        return;
    }
    if (server.method() != HTTP_POST) {
        server.send(405, "application/json", "{\"error\":\"method\"}");
        return;
    }
    String body = server.arg("plain");
    if (body.length() == 0) {
        server.send(400, "application/json", "{\"error\":\"body\"}");
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        server.send(400, "application/json", "{\"error\":\"json\"}");
        return;
    }
    bool enabled = doc["enabled"] | true;
    displaySetEnabled(enabled);
    server.send(200, "application/json", "{}");
}

static void handleApiPreviewGoal() {
    if (server.method() != HTTP_POST) {
        server.send(405, "application/json", "{\"error\":\"method\"}");
        return;
    }
    if (!displayTriggerGoalPreview()) {
        server.send(409, "application/json", "{\"error\":\"no_game\"}");
        return;
    }
    server.send(200, "application/json", "{}");
}
uint32_t apiServerGetSelectedGameId() {
    return selectedGameId;
}

void apiServerInit() {
    dataModelInit();
    selectedGameId = 0;
    saveSelectedGameId(0);
    dataModelSetSelectedGame(0);
    Serial.println("[api] selectedGameId reset to 0");

    server.on("/", handleRoot);
    server.on("/index.html", handleRoot);
    server.on("/api/select-game", HTTP_POST, handleApiSelectGame);
    server.on("/api/selected-game", HTTP_GET, handleApiSelectedGame);
    server.on("/api/display-power", HTTP_ANY, handleApiDisplayPower);
    server.on("/api/preview-goal", HTTP_POST, handleApiPreviewGoal);
    server.onNotFound([]() {
        server.send(404, "text/plain", "404");
    });
    server.begin();
    Serial.println("Serveur HTTP démarré.");

    scheduleServiceInit(server);
    playByPlayServiceInit(server);
}

void apiServerLoop() {
    server.handleClient();
}
