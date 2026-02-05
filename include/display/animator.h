#pragma once

#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

class Animator {
public:
    virtual ~Animator() = default;
    virtual void start(uint32_t nowMs) = 0;
    virtual bool tick(MatrixPanel_I2S_DMA& display, uint32_t nowMs) = 0;
};

