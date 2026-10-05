#pragma once

#include <vitacompanion_input.h>

#include <psp2/touch.h>
#include <stdint.h>

typedef struct {
    int active;
    uint16_t x;
    uint16_t y;
} vitacompanion_touch_point;

unsigned int vitacompanion_patch_touch_data(unsigned int port,
    SceTouchData *data, unsigned int count,
    const vitacompanion_touch_point points[VITACOMPANION_TOUCH_SLOTS]);
