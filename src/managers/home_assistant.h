#pragma once

#include "config.h"
#include "store.h"

struct HomeAssistantTaskArgs {
    EntityStore* store;
    Configuration* config;
};

void home_assistant_task(void* arg);

// Send a WebSocket close frame to the HA server and wait briefly for the
// ack. Call before deep sleep so the server doesn't hold a half-open session.
// No-op if the client hasn't been constructed yet.
void home_assistant_shutdown();