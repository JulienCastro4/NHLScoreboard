#include "display/display_manager.h"

#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

#include "display/data_model.h"
#include "display/goal_scene.h"
#include "display/hub75_pins.h"
#include "display/logo_cache.h"
#include "display/recap_scene.h"
#include "display/scoreboard_scene.h"

#include <strings.h>

namespace {
    constexpr uint16_t PANEL_RES_X = 64;
    constexpr uint16_t PANEL_RES_Y = 32;
    constexpr uint8_t PANEL_CHAIN = 1;
    constexpr uint32_t FRAME_INTERVAL_MS = 33;
    constexpr uint8_t DEFAULT_BRIGHTNESS = 50;

    MatrixPanel_I2S_DMA* matrix = nullptr;
    ScoreboardScene scene;
    GoalScene goalScene;
    RecapScene recapScene;
    uint32_t lastFrameMs = 0;
    bool displayReady = false;
    bool displayEnabled = true;
    bool previewActive = false;
    GameSnapshot previewSnapshot{};
    GameSnapshot goalAnimSnapshot{};
    char lastGoalKey[64] = {0};
    bool goalAnimActive = false;
    uint32_t goalAnimStartMs = 0;
    uint32_t lastGameId = 0;

    enum class RecapMode {
        Standard,
        Recap
    };
    RecapMode recapMode = RecapMode::Standard;
    uint32_t recapModeStartMs = 0;
    constexpr uint32_t STANDARD_MS = 20000;

    void copyStr(char* dest, size_t destSize, const char* src) {
        if (!dest || destSize == 0) return;
        if (!src) src = "";
        strncpy(dest, src, destSize - 1);
        dest[destSize - 1] = '\0';
    }

    void buildGoalKey(const GameSnapshot& snap, char* out, size_t outSize) {
        snprintf(out, outSize, "%u|%u",
            (unsigned)snap.gameId,
            (unsigned)snap.goalEventId);
    }

    void startGoalAnim(const GameSnapshot& snap, uint32_t nowMs) {
        goalAnimActive = true;
        goalAnimStartMs = nowMs;
        buildGoalKey(snap, lastGoalKey, sizeof(lastGoalKey));
        goalAnimSnapshot = snap;
    }

    void renderGoalOverlay(MatrixPanel_I2S_DMA& display, const GameSnapshot& snap, uint32_t nowMs) {
        const uint32_t elapsed = nowMs - goalAnimStartMs;
        if (elapsed > 17000) {
            goalAnimActive = false;
            previewActive = false;
            return;
        }
        const GameSnapshot& frameSnap = previewActive ? previewSnapshot : goalAnimSnapshot;
        goalScene.render(display, frameSnap, elapsed);
    }

    bool isFinalState(const char* state) {
        if (!state || !state[0]) return false;
        return (strcasecmp(state, "FINAL") == 0) || (strcasecmp(state, "OFF") == 0);
    }

}

void displayInit() {
    if (displayReady) return;

    dataModelInit();
    logoCacheInit();

    HUB75_I2S_CFG::i2s_pins pins = {
        HUB75_R1_PIN, HUB75_G1_PIN, HUB75_B1_PIN,
        HUB75_R2_PIN, HUB75_G2_PIN, HUB75_B2_PIN,
        HUB75_A_PIN, HUB75_B_PIN, HUB75_C_PIN, HUB75_D_PIN, HUB75_E_PIN,
        HUB75_LAT_PIN, HUB75_OE_PIN, HUB75_CLK_PIN
    };

    HUB75_I2S_CFG config(PANEL_RES_X, PANEL_RES_Y, PANEL_CHAIN, pins);
    config.double_buff = true;
    config.clkphase = false;

    matrix = new MatrixPanel_I2S_DMA(config);
    matrix->begin();
    matrix->setBrightness8(displayEnabled ? DEFAULT_BRIGHTNESS : 0);
    matrix->setLatBlanking(3);
    matrix->clearScreen();
    displayReady = true;
    Serial.println("[display] init ok");
}

void displaySetEnabled(bool enabled) {
    displayEnabled = enabled;
    if (!displayReady || !matrix) return;
    if (displayEnabled) {
        matrix->setBrightness8(DEFAULT_BRIGHTNESS);
    } else {
        matrix->setBrightness8(0);
        matrix->clearScreen();
    }
}

bool displayIsEnabled() {
    return displayEnabled;
}

bool displayTriggerGoalPreview() {
    if (!displayReady || !matrix) return false;
    GameSnapshot snapshot{};
    dataModelGetSnapshot(snapshot);
    if (snapshot.gameId == 0) return false;
    previewSnapshot = snapshot;
    copyStr(previewSnapshot.goalScorer, sizeof(previewSnapshot.goalScorer), "Connor McDavid");
    copyStr(previewSnapshot.goalAssist1, sizeof(previewSnapshot.goalAssist1), "Nick Suzuki");
    copyStr(previewSnapshot.goalAssist2, sizeof(previewSnapshot.goalAssist2), "Juraj Slafkovsky");
    copyStr(previewSnapshot.goalTime, sizeof(previewSnapshot.goalTime), "00:00");
    previewSnapshot.goalPeriod = snapshot.period ? snapshot.period : 1;
    previewSnapshot.goalOwnerTeamId = snapshot.home.id ? snapshot.home.id : snapshot.away.id;
    previewActive = true;
    startGoalAnim(previewSnapshot, millis());
    return true;
}

void displayTick() {
    if (!displayReady || !matrix) return;
    if (!displayEnabled) return;
    uint32_t now = millis();
    if (now - lastFrameMs < FRAME_INTERVAL_MS) return;
    lastFrameMs = now;

    matrix->flipDMABuffer();

    GameSnapshot snapshot{};
    dataModelGetSnapshot(snapshot);
    if (snapshot.gameId != lastGameId) {
        lastGameId = snapshot.gameId;
        lastGoalKey[0] = '\0';
        goalAnimActive = false;
        recapMode = RecapMode::Standard;
        recapModeStartMs = now;
        logoCacheClear();
    }
    if (snapshot.goalIsNew) {
        char key[64];
        buildGoalKey(snapshot, key, sizeof(key));
        if (strcmp(key, lastGoalKey) != 0) {
            startGoalAnim(snapshot, now);
            dataModelClearGoalFlag();
        }
    }
    if (goalAnimActive) {
        renderGoalOverlay(*matrix, snapshot, now);
        return;
    }

    const bool finalState = isFinalState(snapshot.gameState);
    const bool recapAvailable = snapshot.recapReady;
    if (finalState && recapAvailable) {
        if (recapMode == RecapMode::Standard) {
            if (now - recapModeStartMs >= STANDARD_MS) {
                recapMode = RecapMode::Recap;
                recapModeStartMs = now;
                recapScene.start(now, snapshot);
            }
        }

        if (recapMode == RecapMode::Recap) {
            recapScene.render(*matrix, snapshot, now);
            if (recapScene.isComplete(now)) {
                recapMode = RecapMode::Standard;
                recapModeStartMs = now;
            }
            return;
        }

        scene.render(*matrix, snapshot, now);
        return;
    }

    recapMode = RecapMode::Standard;
    recapModeStartMs = now;
    scene.render(*matrix, snapshot, now);
}

