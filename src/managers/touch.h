#pragma once
#include "config.h"
#include "store.h"
#include "ui_state.h"
#include <bb_captouch.h>
#include <cstdint>

struct TouchEvent {
    uint16_t x;
    uint16_t y;
};

struct TouchTaskArgs {
    SharedUIState* state;
    EntityStore* store;
    BBCapTouch* bbct;
    const Configuration* config;
};

void touch_task(void* arg);
