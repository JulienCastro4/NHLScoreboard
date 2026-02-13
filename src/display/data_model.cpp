#include "display/data_model.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {
    SemaphoreHandle_t dataModelMutex = nullptr;
    GameSnapshot current;

    void copyStr(char* dest, size_t destSize, const char* src) {
        if (!dest || destSize == 0) return;
        if (!src) src = "";
        strncpy(dest, src, destSize - 1);
        dest[destSize - 1] = '\0';
    }

    void clearTeam(TeamInfo& team) {
        team.id = 0;
        copyStr(team.abbrev, sizeof(team.abbrev), "");
        copyStr(team.name, sizeof(team.name), "");
        team.score = 0;
        team.sog = 0;
    }

    void clearSnapshot(GameSnapshot& snap) {
        snap.gameId = 0;
        copyStr(snap.gameState, sizeof(snap.gameState), "");
        copyStr(snap.startTimeUtc, sizeof(snap.startTimeUtc), "");
        copyStr(snap.utcOffset, sizeof(snap.utcOffset), "");
        clearTeam(snap.away);
        clearTeam(snap.home);
        snap.period = 0;
        copyStr(snap.timeRemaining, sizeof(snap.timeRemaining), "");
        snap.inIntermission = false;
        snap.goalIsNew = false;
        snap.goalEventId = 0;
        snap.goalOwnerTeamId = 0;
        copyStr(snap.goalScorer, sizeof(snap.goalScorer), "");
        copyStr(snap.goalAssist1, sizeof(snap.goalAssist1), "");
        copyStr(snap.goalAssist2, sizeof(snap.goalAssist2), "");
        copyStr(snap.goalTime, sizeof(snap.goalTime), "");
        snap.goalPeriod = 0;
        snap.awayPP = false;
        snap.homePP = false;
        snap.recapReady = false;
        copyStr(snap.recapText, sizeof(snap.recapText), "");
        snap.recapGoalCount = 0;
        for (size_t i = 0; i < kMaxRecapGoals; ++i) {
            snap.recapGoals[i].eventId = 0;
            copyStr(snap.recapGoals[i].teamAbbrev, sizeof(snap.recapGoals[i].teamAbbrev), "");
            copyStr(snap.recapGoals[i].scorer, sizeof(snap.recapGoals[i].scorer), "");
            copyStr(snap.recapGoals[i].assist1, sizeof(snap.recapGoals[i].assist1), "");
            copyStr(snap.recapGoals[i].assist2, sizeof(snap.recapGoals[i].assist2), "");
            copyStr(snap.recapGoals[i].timeRemaining, sizeof(snap.recapGoals[i].timeRemaining), "");
            snap.recapGoals[i].period = 0;
        }
    }
}

void dataModelInit() {
    if (!dataModelMutex) {
        dataModelMutex = xSemaphoreCreateMutex();
    }
    if (dataModelMutex) {
        xSemaphoreTake(dataModelMutex, portMAX_DELAY);
        clearSnapshot(current);
        xSemaphoreGive(dataModelMutex);
    }
}

void dataModelSetSelectedGame(uint32_t gameId) {
    if (!dataModelMutex) return;
    xSemaphoreTake(dataModelMutex, portMAX_DELAY);
    if (current.gameId != gameId) {
        clearSnapshot(current);
        current.gameId = gameId;
    }
    xSemaphoreGive(dataModelMutex);
}

void dataModelUpdateFromScheduleGame(JsonObjectConst game) {
    if (!dataModelMutex) return;
    uint32_t gameId = game["id"] | 0;
    if (gameId == 0) return;

    xSemaphoreTake(dataModelMutex, portMAX_DELAY);
    if (current.gameId != gameId) {
        xSemaphoreGive(dataModelMutex);
        return;
    }
    copyStr(current.gameState, sizeof(current.gameState), game["gameState"] | "");

    JsonObjectConst away = game["away"];
    JsonObjectConst home = game["home"];
    copyStr(current.away.abbrev, sizeof(current.away.abbrev), away["abbrev"] | "");
    copyStr(current.away.name, sizeof(current.away.name), away["name"] | "");
    current.away.score = away["score"] | 0;
    current.away.sog = away["sog"] | 0;

    copyStr(current.home.abbrev, sizeof(current.home.abbrev), home["abbrev"] | "");
    copyStr(current.home.name, sizeof(current.home.name), home["name"] | "");
    current.home.score = home["score"] | 0;
    current.home.sog = home["sog"] | 0;

    current.period = game["period"] | 0;
    if (!game["clock"].isNull()) {
        JsonObjectConst clock = game["clock"];
        copyStr(current.timeRemaining, sizeof(current.timeRemaining), clock["timeRemaining"] | "");
        current.inIntermission = clock["inIntermission"] | false;
    }
    xSemaphoreGive(dataModelMutex);
}

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
    const RecapGoal* recapGoals) {
    if (!dataModelMutex || gameId == 0) return;
    xSemaphoreTake(dataModelMutex, portMAX_DELAY);
    if (current.gameId != gameId) {
        xSemaphoreGive(dataModelMutex);
        return;
    }
    copyStr(current.gameState, sizeof(current.gameState), gameState);
    copyStr(current.startTimeUtc, sizeof(current.startTimeUtc), startTimeUtc);
    copyStr(current.utcOffset, sizeof(current.utcOffset), utcOffset);
    current.period = period;
    copyStr(current.timeRemaining, sizeof(current.timeRemaining), timeRemaining);
    current.inIntermission = inIntermission;
    current.away.id = awayId;
    copyStr(current.away.abbrev, sizeof(current.away.abbrev), awayAbbrev);
    copyStr(current.away.name, sizeof(current.away.name), awayName);
    current.away.score = awayScore;
    current.away.sog = awaySog;
    current.home.id = homeId;
    copyStr(current.home.abbrev, sizeof(current.home.abbrev), homeAbbrev);
    copyStr(current.home.name, sizeof(current.home.name), homeName);
    current.home.score = homeScore;
    current.home.sog = homeSog;
    // Only SET goalIsNew, never clear it — only the display thread clears it
    // via dataModelClearGoalFlag(). This prevents a subsequent fetch from
    // overwriting goalIsNew=true before the display thread reads it.
    if (goalIsNew) {
        current.goalIsNew = true;
        current.goalEventId = goalEventId;
        current.goalOwnerTeamId = goalOwnerTeamId;
        copyStr(current.goalScorer, sizeof(current.goalScorer), goalScorer);
        copyStr(current.goalAssist1, sizeof(current.goalAssist1), goalAssist1);
        copyStr(current.goalAssist2, sizeof(current.goalAssist2), goalAssist2);
        copyStr(current.goalTime, sizeof(current.goalTime), goalTime);
        current.goalPeriod = goalPeriod;
    }
    current.awayPP = awayPP;
    current.homePP = homePP;
    current.recapReady = recapReady;
    copyStr(current.recapText, sizeof(current.recapText), recapText);
    if (recapGoals && recapGoalCount > 0) {
        if (recapGoalCount > kMaxRecapGoals) recapGoalCount = kMaxRecapGoals;
        current.recapGoalCount = recapGoalCount;
        for (size_t i = 0; i < recapGoalCount; ++i) {
            current.recapGoals[i].eventId = recapGoals[i].eventId;
            copyStr(current.recapGoals[i].teamAbbrev, sizeof(current.recapGoals[i].teamAbbrev), recapGoals[i].teamAbbrev);
            copyStr(current.recapGoals[i].scorer, sizeof(current.recapGoals[i].scorer), recapGoals[i].scorer);
            copyStr(current.recapGoals[i].assist1, sizeof(current.recapGoals[i].assist1), recapGoals[i].assist1);
            copyStr(current.recapGoals[i].assist2, sizeof(current.recapGoals[i].assist2), recapGoals[i].assist2);
            copyStr(current.recapGoals[i].timeRemaining, sizeof(current.recapGoals[i].timeRemaining), recapGoals[i].timeRemaining);
            current.recapGoals[i].period = recapGoals[i].period;
        }
    } else {
        current.recapGoalCount = 0;
    }
    xSemaphoreGive(dataModelMutex);
}

bool dataModelGetSnapshot(GameSnapshot& out) {
    if (!dataModelMutex) return false;
    xSemaphoreTake(dataModelMutex, portMAX_DELAY);
    out = current;
    xSemaphoreGive(dataModelMutex);
    return out.gameId != 0;
}

void dataModelClearGoalFlag() {
    if (!dataModelMutex) return;
    xSemaphoreTake(dataModelMutex, portMAX_DELAY);
    current.goalIsNew = false;
    xSemaphoreGive(dataModelMutex);
}

