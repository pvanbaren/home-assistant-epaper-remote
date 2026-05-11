#pragma once
#include "constants.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "ui_state.h"
#include <cstdint>

enum class CommandType : uint8_t {
    MediaVolumeUp,
    MediaVolumeDown,
    MediaVolumeMute,
    MediaSelectSource,
    RemoteSendCommand,
    CallService, // generic HA call_service from a HassAction descriptor
};

struct HassAction; // fwd-decl; full definition in config.h

enum class ConnState : uint8_t {
    Initializing,
    InvalidCredentials,
    ConnectionError,
    Up,
};

enum class SettingsMode : uint8_t {
    None,
    Wifi,
    WifiPassword,
};

// Time-since-interaction phase. See thresholds in constants.h.
enum class IdlePhase : uint8_t {
    Active,    // backlight on
    Dim,       // backlight off, UI mode unchanged
    Standby,   // backlight off, UI shows standby placeholder
    DeepSleep, // ready to enter ESP deep sleep
};

struct IdleSnapshot {
    IdlePhase phase;
    bool entered_standby; // edge: prev != Standby, current == Standby
    bool left_standby;    // edge: prev == Standby, current != Standby
};

struct WifiNetwork {
    char ssid[MAX_WIFI_SSID_LEN];
    int16_t rssi;
    bool secure;
    bool known; // has a saved password — tap to connect without re-entering
};

struct WifiSettingsSnapshot {
    ConnState wifi_state;
    bool connected;
    bool scan_in_progress;
    bool connecting;
    bool custom_profile_active;
    char connect_error[MAX_WIFI_ERROR_LEN];
    char connected_ssid[MAX_WIFI_SSID_LEN];
    char profile_ssid[MAX_WIFI_SSID_LEN];
    char ip_address[MAX_WIFI_IP_LEN];
    int16_t rssi;
    uint8_t page;
    uint8_t network_count;
    WifiNetwork networks[MAX_WIFI_NETWORKS];
};

struct WifiPasswordSnapshot {
    bool symbols;
    bool shift;
    bool connecting;
    char target_ssid[MAX_WIFI_SSID_LEN];
    char password[MAX_WIFI_PASSWORD_LEN + 1];
    char connect_error[MAX_WIFI_ERROR_LEN];
};

struct Command {
    CommandType type;
    const char* entity_id;
    uint8_t entity_idx;
    uint8_t value;
    const char* command_name; // payload for RemoteSendCommand (e.g. "Up"); nullptr otherwise
    const HassAction* action; // payload for CallService; nullptr otherwise
    uint32_t enqueued_ms;     // ms tick when enqueued; commands older than MEDIA_COMMAND_MAX_AGE_MS get dropped at dequeue
};

struct EntityStore {
    ConnState wifi = ConnState::Initializing;
    ConnState home_assistant = ConnState::Initializing;
    SettingsMode settings_mode = SettingsMode::None;
    uint32_t settings_revision = 0;

    char wifi_connected_ssid[MAX_WIFI_SSID_LEN];
    char wifi_ip_address[MAX_WIFI_IP_LEN];
    int16_t wifi_rssi = -127;
    bool wifi_connected = false;
    bool wifi_scan_in_progress = false;
    bool wifi_connecting = false;
    char wifi_connect_error[MAX_WIFI_ERROR_LEN];
    bool wifi_custom_profile_active = false;
    char wifi_profile_ssid[MAX_WIFI_SSID_LEN];
    WifiNetwork wifi_networks[MAX_WIFI_NETWORKS];
    uint8_t wifi_network_count = 0;
    uint8_t wifi_list_page = 0;
    char wifi_target_ssid[MAX_WIFI_SSID_LEN];
    char wifi_password_input[MAX_WIFI_PASSWORD_LEN + 1];
    bool wifi_password_symbols = false;
    bool wifi_password_shift = false;

    uint32_t last_interaction_ms = 0;
    IdlePhase last_idle_phase = IdlePhase::Active;
    uint32_t standby_revision = 0;

    bool battery_valid = false;
    uint8_t battery_soc_pct = 0;
    uint16_t battery_voltage_mv = 0;
    uint32_t battery_revision = 0;

    uint8_t media_device_idx = 0;
    bool media_device_select_active = false;
    bool battery_status_active = false;

    // Ring buffer for fire-and-forget media/remote commands. Newest write
    // wins when full (drops oldest) so the user's most recent intent
    // always reaches HA.
    Command media_command_queue[MEDIA_COMMAND_QUEUE_SIZE] = {};
    uint8_t media_command_head = 0;
    uint8_t media_command_tail = 0;
    uint8_t media_command_count = 0;

    SemaphoreHandle_t mutex;
    TaskHandle_t home_assistant_task;
    TaskHandle_t ui_task;
    EventGroupHandle_t event_group = nullptr;
};

constexpr EventBits_t BIT_WIFI_UP = (1 << 0);

void store_init(EntityStore* store);
void store_set_wifi_state(EntityStore* store, ConnState state);
void store_set_hass_state(EntityStore* store, ConnState state);
void store_send_media_command(EntityStore* store, CommandType type, const char* entity_id, const char* command_name);
void store_send_hass_action(EntityStore* store, const HassAction* action);
bool store_get_pending_command(EntityStore* store, Command* command); // pops from the queue
bool store_go_home(EntityStore* store);
bool store_open_wifi_settings(EntityStore* store);
bool store_open_wifi_password(EntityStore* store, const char* ssid);
bool store_settings_back(EntityStore* store);
bool store_close_settings(EntityStore* store);
bool store_shift_wifi_list_page(EntityStore* store, int8_t delta);
bool store_set_wifi_password_symbols(EntityStore* store, bool symbols);
bool store_toggle_wifi_password_shift(EntityStore* store);
bool store_append_wifi_password_char(EntityStore* store, char ch);
bool store_backspace_wifi_password(EntityStore* store);
bool store_clear_wifi_password(EntityStore* store);
void store_set_wifi_connection_info(EntityStore* store, bool connected, const char* ssid, const char* ip_address, int16_t rssi);
void store_set_wifi_scan_state(EntityStore* store, bool in_progress);
void store_set_wifi_scan_results(EntityStore* store, const WifiNetwork* networks, uint8_t count);
void store_set_wifi_connecting(EntityStore* store, bool connecting);
void store_set_wifi_connect_error(EntityStore* store, const char* error);
void store_set_wifi_profile(EntityStore* store, const char* ssid, bool custom_profile_active);
void store_get_wifi_settings_snapshot(EntityStore* store, WifiSettingsSnapshot* snapshot);
bool store_get_wifi_password_snapshot(EntityStore* store, WifiPasswordSnapshot* snapshot);
void store_note_interaction(EntityStore* store, uint32_t now_ms);
uint32_t store_get_last_interaction_ms(EntityStore* store);
// Single idle-phase poll. Compares time-since-interaction against the
// thresholds in constants.h, applies network/settings gating (no
// standby while wifi/HA is down or settings UI is open), updates the
// store's cached phase, and returns the current phase plus
// edge-transition flags. Call once per loop tick.
IdleSnapshot store_poll_idle(EntityStore* store, uint32_t now_ms);
bool store_is_standby_active(EntityStore* store); // == last_idle_phase == Standby
void store_set_battery_state(EntityStore* store, bool valid, uint8_t soc_pct, uint16_t voltage_mv);
bool store_get_battery_state(EntityStore* store, uint8_t* soc_pct_out, uint16_t* voltage_mv_out);
void store_update_ui_state(EntityStore* store, UIState* ui_state);
void store_wait_for_wifi_up(EntityStore* store);
void store_flush_pending_commands(EntityStore* store);
bool store_open_media_device_select(EntityStore* store);
bool store_close_media_device_select(EntityStore* store);
bool store_set_media_device_idx(EntityStore* store, uint8_t idx, uint8_t device_count);
uint8_t store_get_media_device_idx(EntityStore* store);
void store_load_persisted_media_device_idx(EntityStore* store, uint8_t device_count);
bool store_open_battery_status(EntityStore* store);
bool store_close_battery_status(EntityStore* store);
