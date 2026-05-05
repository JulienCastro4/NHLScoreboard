#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <time.h>
#include <esp_task_wdt.h>
#include "secrets.h"
#include "api_server.h"
#include "display/display_manager.h"

const char* WIFI_SSID = WIFI_SSID_SECRET;
const char* WIFI_PASS = WIFI_PASS_SECRET;

static unsigned long lastWifiCheckMs = 0;
static const unsigned long WIFI_CHECK_INTERVAL_MS = 10000;
static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 30000;

void setup() {
  Serial.begin(115200);
  delay(2000);

  // Enable task watchdog (15s timeout)
  esp_task_wdt_init(15, true);
  esp_task_wdt_add(NULL); // add loop task

  if (!LittleFS.begin(true)) {
    Serial.println("Erreur LittleFS");
    return;
  }
  Serial.println("LittleFS OK");

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connexion WiFi");
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - wifiStart > WIFI_CONNECT_TIMEOUT_MS) {
      Serial.println("\nWiFi timeout - reboot");
      ESP.restart();
    }
    delay(500);
    Serial.print(".");
    esp_task_wdt_reset();
  }
  Serial.println();
  Serial.print("Connecté. IP: ");
  Serial.println(WiFi.localIP());

  // Configure NTP for time synchronization
  Serial.print("Synchronisation NTP...");
  configTime(-5 * 3600, 0, "pool.ntp.org", "time.nist.gov");

  // Wait for time to be set (max 10 seconds)
  int retry = 0;
  while (time(nullptr) < 100000 && retry < 20) {
    delay(500);
    Serial.print(".");
    retry++;
  }
  Serial.println();

  time_t now = time(nullptr);
  if (now > 100000) {
    Serial.printf("NTP OK: %s", ctime(&now));
  } else {
    Serial.println("NTP échec - temps non synchronisé");
  }

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
  esp_task_wdt_reset();

  // WiFi reconnection check
  unsigned long now = millis();
  if (now - lastWifiCheckMs >= WIFI_CHECK_INTERVAL_MS) {
    lastWifiCheckMs = now;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[wifi] disconnected, reconnecting...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
  }

  apiServerLoop();
  displayTick();
}
