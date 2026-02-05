#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <ESPmDNS.h>

#include "secrets.h"
#include "api_server.h"
#include "display/display_manager.h"

const char* WIFI_SSID = WIFI_SSID_SECRET;
const char* WIFI_PASS = WIFI_PASS_SECRET;

void setup() {
    Serial.begin(115200);
    delay(2000);

    if (!LittleFS.begin(true)) {
        Serial.println("Erreur LittleFS");
        return;
    }
    Serial.println("LittleFS OK");

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("Connexion WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();
    Serial.print("Connecté. IP: ");
    Serial.println(WiFi.localIP());

    if (MDNS.begin("scoreboardapp")) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("mDNS OK → http://scoreboardapp.local");
    } else {
        Serial.println("mDNS échec");
    }

    apiServerInit();
    displayInit();
}

void loop() {
    apiServerLoop();
    displayTick();
}
