#pragma once

#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

#include "display/data_model.h"

class Scene {
public:
    virtual ~Scene() = default;
    virtual void render(MatrixPanel_I2S_DMA& display, const GameSnapshot& data, uint32_t nowMs) = 0;
};

