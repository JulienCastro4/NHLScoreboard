#pragma once

#include <Arduino.h>
#include <WebServer.h>

void scheduleServiceInit(WebServer& server);
bool scheduleServicePrimeSelectedGame(uint32_t gameId);

