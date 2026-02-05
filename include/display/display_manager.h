#pragma once

#include <Arduino.h>

void displayInit();
void displayTick();
void displaySetEnabled(bool enabled);
bool displayIsEnabled();
bool displayTriggerGoalPreview();

