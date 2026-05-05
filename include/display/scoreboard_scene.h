#pragma once

#include "display/scene.h"

class ScoreboardScene : public Scene {
public:
    void render(MatrixPanel_I2S_DMA& display, const GameSnapshot& data, uint32_t nowMs) override;
    void resetFinalPhase();

private:
    unsigned long lastToggleMs = 0;
    bool showSOG = false;
    int logoSlideY = 0;
    unsigned long lastSlideMs = 0;
    int scoreSlideY = 0;
    unsigned long lastScoreSlideMs = 0;
    bool finalPhaseActive = false;
    uint32_t finalPhaseGameId = 0;
    uint32_t finalPhaseStartMs = 0;
    bool finalSeriesPhaseUnlocked = false;
    uint32_t seriesShownMs = 0;
    bool hadSeriesData = false;
};

