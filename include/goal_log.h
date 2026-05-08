#pragma once

#include <Arduino.h>

enum class GoalLogType : uint8_t {
    Detected = 0,
    Queued = 1,
    AnimStart = 2,
    AnimEnd = 3,
    DupSkipped = 4
};

struct GoalLogEntry {
    uint32_t timestampMs;
    uint32_t eventId;
    uint32_t gameId;
    GoalLogType type;
    char scorer[24];
    uint8_t period;
};

constexpr size_t kMaxGoalLogEntries = 64;

void goalLogInit();
void goalLogAdd(GoalLogType type, uint32_t gameId, uint32_t eventId, const char* scorer, uint8_t period);
void goalLogClearIfNewGame(uint32_t gameId);
size_t goalLogCount();
const GoalLogEntry& goalLogGet(size_t index);
uint32_t goalLogGameId();