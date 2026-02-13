#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

struct TeamInfo {
    uint32_t id;
    char abbrev[4];
    char name[32];
    uint16_t score;
    uint16_t sog;
};

constexpr size_t kMaxRecapGoals = 24;

struct RecapGoal {
    uint32_t eventId;
    char teamAbbrev[4];
    char scorer[24];
    char assist1[24];
    char assist2[24];
    char timeRemaining[8];
    uint8_t period;
};

constexpr size_t kRecapTextMax = 768;

struct GameSnapshot {
    uint32_t gameId;
    char gameState[8];
    char startTimeUtc[24];
    char utcOffset[8];
    TeamInfo away;
    TeamInfo home;
    uint8_t period;
    char timeRemaining[8];
    bool inIntermission;
    bool goalIsNew;
    uint32_t goalEventId;
    uint32_t goalOwnerTeamId;
    char goalScorer[32];
    char goalTime[8];
    uint8_t goalPeriod;
    char goalAssist1[32];
    char goalAssist2[32];
    bool awayPP;
    bool homePP;
    bool recapReady;
    char recapText[kRecapTextMax];
    uint8_t recapGoalCount;
    RecapGoal recapGoals[kMaxRecapGoals];
};

void dataModelInit();
void dataModelSetSelectedGame(uint32_t gameId);
void dataModelUpdateFromScheduleGame(JsonObjectConst game);
void dataModelUpdateFromPbp(uint32_t gameId,
    const char* gameState,
    const char* startTimeUtc,
    const char* utcOffset,
    uint8_t period,
    const char* timeRemaining,
    bool inIntermission,
    uint32_t awayId,
    const char* awayAbbrev,
    const char* awayName,
    uint16_t awayScore,
    uint16_t awaySog,
    uint32_t homeId,
    const char* homeAbbrev,
    const char* homeName,
    uint16_t homeScore,
    uint16_t homeSog,
    bool goalIsNew,
    uint32_t goalEventId,
    uint32_t goalOwnerTeamId,
    const char* goalScorer,
    const char* goalAssist1,
    const char* goalAssist2,
    const char* goalTime,
    uint8_t goalPeriod,
    bool awayPP,
    bool homePP,
    bool recapReady,
    const char* recapText,
    uint8_t recapGoalCount,
    const RecapGoal* recapGoals);
bool dataModelGetSnapshot(GameSnapshot& out);
void dataModelClearGoalFlag();

