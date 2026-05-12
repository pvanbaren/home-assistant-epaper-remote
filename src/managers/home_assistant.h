#pragma once

#include "config.h"
#include "store.h"

struct HomeAssistantTaskArgs {
    EntityStore* store;
    Configuration* config;
};

void home_assistant_task(void* arg);

// Nudge the HA task to retry its connection probe now rather than waiting
// out HASS_RECONNECT_DELAY_MS. Safe to call before the task has started —
// it'll just be a no-op until the task records itself.
void hass_request_probe();
