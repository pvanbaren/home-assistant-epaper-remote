#pragma once

#include "constants.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstdint>

enum class UiMode : uint8_t {
    Blank, // state after boot
    Boot,
    GenericError,
    WifiDisconnected,
    HassDisconnected,
    HassInvalidKey,
    MediaController,
    MediaDeviceSelect,
    BatteryStatus,
    Standby,
    WifiSettings,
    WifiPassword,
};

struct UIState {
    UiMode mode = UiMode::Blank;
    uint8_t wifi_list_page = 0;
    uint32_t settings_revision = 0;
    uint32_t standby_revision = 0;
    uint32_t battery_revision = 0;
    uint8_t media_device_idx = 0;
    int16_t wifi_rssi = -127;
    bool wifi_connected = false;
};

// The touch task needs to know the current state of the UI.
// This struct handles the sharing of the UIState safely.
struct SharedUIState {
    SemaphoreHandle_t mutex;
    uint32_t version;
    UIState state;
};

void ui_state_init(SharedUIState* state);
void ui_state_set(SharedUIState* state, const UIState* new_state);
void ui_state_copy(SharedUIState* state, uint32_t* local_version, UIState* local_state);
