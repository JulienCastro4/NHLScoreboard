#include "goal_log.h"

static GoalLogEntry goalLogBuffer[kMaxGoalLogEntries];
static size_t goalLogHead = 0;
static size_t goalLogUsed = 0;
static uint32_t currentGameId = 0;

void goalLogInit() {
    goalLogHead = 0;
    goalLogUsed = 0;
    currentGameId = 0;
}

void goalLogClearIfNewGame(uint32_t gameId) {
    if (gameId != 0 && gameId != currentGameId) {
        goalLogHead = 0;
        goalLogUsed = 0;
        currentGameId = gameId;
    }
}

void goalLogAdd(GoalLogType type, uint32_t gameId, uint32_t eventId, const char* scorer, uint8_t period) {
    // Auto-reset if game changed
    if (gameId != 0 && gameId != currentGameId) {
        goalLogHead = 0;
        goalLogUsed = 0;
        currentGameId = gameId;
    }

    GoalLogEntry& e = goalLogBuffer[goalLogHead];
    e.timestampMs = millis();
    e.eventId = eventId;
    e.gameId = gameId;
    e.type = type;
    e.period = period;
    if (scorer) {
        strncpy(e.scorer, scorer, sizeof(e.scorer) - 1);
        e.scorer[sizeof(e.scorer) - 1] = '\0';
    } else {
        e.scorer[0] = '\0';
    }
    goalLogHead = (goalLogHead + 1) % kMaxGoalLogEntries;
    if (goalLogUsed < kMaxGoalLogEntries) goalLogUsed++;
}

size_t goalLogCount() {
    return goalLogUsed;
}

const GoalLogEntry& goalLogGet(size_t index) {
    // index 0 = oldest entry
    size_t start = (goalLogUsed < kMaxGoalLogEntries) ? 0 : goalLogHead;
    size_t pos = (start + index) % kMaxGoalLogEntries;
    return goalLogBuffer[pos];
}

uint32_t goalLogGameId() {
    return currentGameId;
}
