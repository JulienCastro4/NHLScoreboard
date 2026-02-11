#pragma once

#include "display/scene.h"

class ScoreboardScene : public Scene {
public:
    void render(MatrixPanel_I2S_DMA& display, const GameSnapshot& data, uint32_t nowMs) override;

private:
    unsigned long lastToggleMs = 0;
    bool showSOG = false;
};

