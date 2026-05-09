#include "store.h"
#include "esp_log.h"
#include <Preferences.h>
#include <cstring>

constexpr const char* MEDIA_PREFS_NS = "media";
constexpr const char* MEDIA_PREFS_DEVICE_IDX = "devidx";

static const char* TAG = "store";

static void notify_ui(EntityStore* store) {
    if (store->ui_task) {
        xTaskNotifyGive(store->ui_task);
    }
}

static void copy_string(char* dst, size_t dst_len, const char* src) {
    if (dst_len == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

static uint8_t wifi_list_page_count(uint8_t item_count) {
    if (item_count == 0) {
        return 1;
    }
    return static_cast<uint8_t>((item_count + WIFI_NETWORKS_PER_PAGE - 1) / WIFI_NETWORKS_PER_PAGE);
}

void store_init(EntityStore* store) {
    store->mutex = xSemaphoreCreateMutex();
    store->event_group = xEventGroupCreate();
    store->last_interaction_ms = static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
    store->standby_last_refresh_ms = store->last_interaction_ms;
}

void store_set_wifi_state(EntityStore* store, ConnState state) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    ConnState previous_state = store->wifi;
    store->wifi = state;
    if (previous_state != state) {
        store->settings_revision++;
    }
    xSemaphoreGive(store->mutex);

    if (state != previous_state) {
        if (state == ConnState::Up) {
            xEventGroupSetBits(store->event_group, BIT_WIFI_UP);
        } else {
            xEventGroupClearBits(store->event_group, BIT_WIFI_UP);
        }
        notify_ui(store);
    }
}

void store_set_hass_state(EntityStore* store, ConnState state) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    ConnState previous_state = store->home_assistant;
    store->home_assistant = state;
    xSemaphoreGive(store->mutex);

    if (state != previous_state) {
        notify_ui(store);
    }
}

void store_send_media_command(EntityStore* store, CommandType type, const char* entity_id, const char* command_name) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    if (store->media_command_count == MEDIA_COMMAND_QUEUE_SIZE) {
        // Queue full: drop oldest so the latest user intent always lands.
        store->media_command_head = (store->media_command_head + 1) % MEDIA_COMMAND_QUEUE_SIZE;
        store->media_command_count--;
        ESP_LOGW(TAG, "Media command queue full, dropping oldest");
    }
    Command& slot = store->media_command_queue[store->media_command_tail];
    slot.type = type;
    slot.entity_id = entity_id;
    slot.entity_idx = UINT8_MAX;
    slot.value = 0;
    slot.command_name = command_name;
    store->media_command_tail = (store->media_command_tail + 1) % MEDIA_COMMAND_QUEUE_SIZE;
    store->media_command_count++;
    xSemaphoreGive(store->mutex);

    ESP_LOGI(TAG, "Queued media command type=%u entity=%s cmd=%s", static_cast<unsigned>(type), entity_id ? entity_id : "(null)",
             command_name ? command_name : "(null)");

    if (store->home_assistant_task) {
        xTaskNotifyGive(store->home_assistant_task);
    }
}

bool store_get_pending_command(EntityStore* store, Command* command) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    if (store->media_command_count == 0) {
        xSemaphoreGive(store->mutex);
        return false;
    }
    *command = store->media_command_queue[store->media_command_head];
    store->media_command_head = (store->media_command_head + 1) % MEDIA_COMMAND_QUEUE_SIZE;
    store->media_command_count--;
    xSemaphoreGive(store->mutex);
    return true;
}

bool store_go_home(EntityStore* store) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    bool changed = false;
    if (store->settings_mode != SettingsMode::None) {
        store->settings_mode = SettingsMode::None;
        store->settings_revision++;
        changed = true;
    }
    if (store->standby_active) {
        store->standby_active = false;
        store->standby_revision++;
        changed = true;
    }
    if (store->media_device_select_active) {
        store->media_device_select_active = false;
        store->settings_revision++;
        changed = true;
    }
    if (store->battery_status_active) {
        store->battery_status_active = false;
        store->settings_revision++;
        changed = true;
    }
    xSemaphoreGive(store->mutex);
    if (changed) {
        notify_ui(store);
    }
    return true;
}

bool store_open_wifi_settings(EntityStore* store) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    const bool changed = store->settings_mode != SettingsMode::Wifi;
    store->settings_mode = SettingsMode::Wifi;
    uint8_t page_count = wifi_list_page_count(store->wifi_network_count);
    if (store->wifi_list_page >= page_count) {
        store->wifi_list_page = static_cast<uint8_t>(page_count - 1);
    }
    if (changed) {
        store->settings_revision++;
    }
    xSemaphoreGive(store->mutex);
    if (changed) {
        notify_ui(store);
    }
    return true;
}

bool store_open_wifi_password(EntityStore* store, const char* ssid) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    store->settings_mode = SettingsMode::WifiPassword;
    copy_string(store->wifi_target_ssid, sizeof(store->wifi_target_ssid), ssid);
    store->wifi_password_input[0] = '\0';
    store->wifi_password_symbols = false;
    store->wifi_password_shift = false;
    store->wifi_connect_error[0] = '\0';
    store->settings_revision++;
    xSemaphoreGive(store->mutex);
    notify_ui(store);
    return true;
}

bool store_open_standby(EntityStore* store, uint32_t now_ms) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    const bool can_activate = store->wifi == ConnState::Up && store->home_assistant == ConnState::Up;
    const bool was_standby = store->standby_active;
    const bool changed = can_activate && (!store->standby_active || store->settings_mode != SettingsMode::None);
    if (changed) {
        store->settings_mode = SettingsMode::None;
        store->standby_active = true;
        if (!was_standby) {
            store->standby_entered_ms = now_ms;
        }
        store->standby_last_refresh_ms = now_ms;
        store->standby_revision++;
        store->settings_revision++;
    }
    xSemaphoreGive(store->mutex);
    if (changed) {
        notify_ui(store);
    }
    return changed;
}

bool store_settings_back(EntityStore* store) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    SettingsMode next_mode = store->settings_mode;
    switch (store->settings_mode) {
    case SettingsMode::WifiPassword:
        next_mode = SettingsMode::Wifi;
        break;
    case SettingsMode::Wifi:
        next_mode = SettingsMode::None;
        break;
    default:
        next_mode = SettingsMode::None;
        break;
    }
    const bool changed = next_mode != store->settings_mode;
    store->settings_mode = next_mode;
    if (changed) {
        store->settings_revision++;
    }
    xSemaphoreGive(store->mutex);
    if (changed) {
        notify_ui(store);
    }
    return changed;
}

bool store_close_settings(EntityStore* store) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    const bool changed = store->settings_mode != SettingsMode::None;
    store->settings_mode = SettingsMode::None;
    if (changed) {
        store->settings_revision++;
    }
    xSemaphoreGive(store->mutex);
    if (changed) {
        notify_ui(store);
    }
    return changed;
}

bool store_shift_wifi_list_page(EntityStore* store, int8_t delta) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    int16_t page = static_cast<int16_t>(store->wifi_list_page) + delta;
    uint8_t pages = wifi_list_page_count(store->wifi_network_count);
    if (page < 0) {
        page = 0;
    } else if (page >= pages) {
        page = pages - 1;
    }
    if (store->wifi_list_page == static_cast<uint8_t>(page)) {
        xSemaphoreGive(store->mutex);
        return false;
    }
    store->wifi_list_page = static_cast<uint8_t>(page);
    store->settings_revision++;
    xSemaphoreGive(store->mutex);
    notify_ui(store);
    return true;
}

bool store_set_wifi_password_symbols(EntityStore* store, bool symbols) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    if (store->wifi_password_symbols == symbols) {
        xSemaphoreGive(store->mutex);
        return false;
    }
    store->wifi_password_symbols = symbols;
    store->wifi_password_shift = false;
    store->settings_revision++;
    xSemaphoreGive(store->mutex);
    notify_ui(store);
    return true;
}

bool store_toggle_wifi_password_shift(EntityStore* store) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    store->wifi_password_shift = !store->wifi_password_shift;
    store->settings_revision++;
    xSemaphoreGive(store->mutex);
    notify_ui(store);
    return true;
}

bool store_append_wifi_password_char(EntityStore* store, char ch) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    size_t len = strlen(store->wifi_password_input);
    if (len >= MAX_WIFI_PASSWORD_LEN) {
        xSemaphoreGive(store->mutex);
        return false;
    }
    store->wifi_password_input[len] = ch;
    store->wifi_password_input[len + 1] = '\0';
    store->settings_revision++;
    xSemaphoreGive(store->mutex);
    notify_ui(store);
    return true;
}

bool store_backspace_wifi_password(EntityStore* store) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    size_t len = strlen(store->wifi_password_input);
    if (len == 0) {
        xSemaphoreGive(store->mutex);
        return false;
    }
    store->wifi_password_input[len - 1] = '\0';
    store->settings_revision++;
    xSemaphoreGive(store->mutex);
    notify_ui(store);
    return true;
}

bool store_clear_wifi_password(EntityStore* store) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    if (store->wifi_password_input[0] == '\0') {
        xSemaphoreGive(store->mutex);
        return false;
    }
    store->wifi_password_input[0] = '\0';
    store->settings_revision++;
    xSemaphoreGive(store->mutex);
    notify_ui(store);
    return true;
}

void store_set_wifi_connection_info(EntityStore* store, bool connected, const char* ssid, const char* ip_address, int16_t rssi) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    bool changed = false;
    if (store->wifi_connected != connected) {
        store->wifi_connected = connected;
        changed = true;
    }
    int16_t rssi_delta = static_cast<int16_t>(store->wifi_rssi - rssi);
    if (rssi_delta < 0) {
        rssi_delta = static_cast<int16_t>(-rssi_delta);
    }
    if (!connected || !store->wifi_connected || rssi_delta >= 4) {
        if (store->wifi_rssi != rssi) {
            store->wifi_rssi = rssi;
            changed = true;
        }
    }
    if (strcmp(store->wifi_connected_ssid, ssid ? ssid : "") != 0) {
        copy_string(store->wifi_connected_ssid, sizeof(store->wifi_connected_ssid), ssid);
        changed = true;
    }
    if (strcmp(store->wifi_ip_address, ip_address ? ip_address : "") != 0) {
        copy_string(store->wifi_ip_address, sizeof(store->wifi_ip_address), ip_address);
        changed = true;
    }
    if (changed) {
        store->settings_revision++;
    }
    xSemaphoreGive(store->mutex);
    if (changed) {
        notify_ui(store);
    }
}

void store_set_wifi_scan_state(EntityStore* store, bool in_progress) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    if (store->wifi_scan_in_progress == in_progress) {
        xSemaphoreGive(store->mutex);
        return;
    }
    store->wifi_scan_in_progress = in_progress;
    store->settings_revision++;
    xSemaphoreGive(store->mutex);
    notify_ui(store);
}

void store_set_wifi_scan_results(EntityStore* store, const WifiNetwork* networks, uint8_t count) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    if (count > MAX_WIFI_NETWORKS) {
        count = MAX_WIFI_NETWORKS;
    }
    memset(store->wifi_networks, 0, sizeof(store->wifi_networks));
    for (uint8_t i = 0; i < count; i++) {
        copy_string(store->wifi_networks[i].ssid, sizeof(store->wifi_networks[i].ssid), networks[i].ssid);
        store->wifi_networks[i].rssi = networks[i].rssi;
        store->wifi_networks[i].secure = networks[i].secure;
        store->wifi_networks[i].known = networks[i].known;
    }
    store->wifi_network_count = count;
    uint8_t pages = wifi_list_page_count(count);
    if (store->wifi_list_page >= pages) {
        store->wifi_list_page = static_cast<uint8_t>(pages - 1);
    }
    store->settings_revision++;
    xSemaphoreGive(store->mutex);
    notify_ui(store);
}

void store_set_wifi_connecting(EntityStore* store, bool connecting) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    if (store->wifi_connecting == connecting) {
        xSemaphoreGive(store->mutex);
        return;
    }
    store->wifi_connecting = connecting;
    if (connecting) {
        store->wifi_connect_error[0] = '\0';
    }
    store->settings_revision++;
    xSemaphoreGive(store->mutex);
    notify_ui(store);
}

void store_set_wifi_connect_error(EntityStore* store, const char* error) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    const char* safe_error = error ? error : "";
    if (strcmp(store->wifi_connect_error, safe_error) == 0) {
        xSemaphoreGive(store->mutex);
        return;
    }
    copy_string(store->wifi_connect_error, sizeof(store->wifi_connect_error), safe_error);
    store->settings_revision++;
    xSemaphoreGive(store->mutex);
    notify_ui(store);
}

void store_set_wifi_profile(EntityStore* store, const char* ssid, bool custom_profile_active) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    bool changed = false;
    const char* safe_ssid = ssid ? ssid : "";
    if (store->wifi_custom_profile_active != custom_profile_active) {
        store->wifi_custom_profile_active = custom_profile_active;
        changed = true;
    }
    if (strcmp(store->wifi_profile_ssid, safe_ssid) != 0) {
        copy_string(store->wifi_profile_ssid, sizeof(store->wifi_profile_ssid), safe_ssid);
        changed = true;
    }
    if (changed) {
        store->settings_revision++;
    }
    xSemaphoreGive(store->mutex);
    if (changed) {
        notify_ui(store);
    }
}

void store_get_wifi_settings_snapshot(EntityStore* store, WifiSettingsSnapshot* snapshot) {
    memset(snapshot, 0, sizeof(WifiSettingsSnapshot));
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    snapshot->wifi_state = store->wifi;
    snapshot->connected = store->wifi_connected;
    snapshot->scan_in_progress = store->wifi_scan_in_progress;
    snapshot->connecting = store->wifi_connecting;
    snapshot->custom_profile_active = store->wifi_custom_profile_active;
    snapshot->rssi = store->wifi_rssi;
    snapshot->page = store->wifi_list_page;
    snapshot->network_count = store->wifi_network_count;
    copy_string(snapshot->connect_error, sizeof(snapshot->connect_error), store->wifi_connect_error);
    copy_string(snapshot->connected_ssid, sizeof(snapshot->connected_ssid), store->wifi_connected_ssid);
    copy_string(snapshot->profile_ssid, sizeof(snapshot->profile_ssid), store->wifi_profile_ssid);
    copy_string(snapshot->ip_address, sizeof(snapshot->ip_address), store->wifi_ip_address);
    for (uint8_t i = 0; i < store->wifi_network_count && i < MAX_WIFI_NETWORKS; i++) {
        copy_string(snapshot->networks[i].ssid, sizeof(snapshot->networks[i].ssid), store->wifi_networks[i].ssid);
        snapshot->networks[i].rssi = store->wifi_networks[i].rssi;
        snapshot->networks[i].secure = store->wifi_networks[i].secure;
        snapshot->networks[i].known = store->wifi_networks[i].known;
    }
    xSemaphoreGive(store->mutex);
}

bool store_get_wifi_password_snapshot(EntityStore* store, WifiPasswordSnapshot* snapshot) {
    memset(snapshot, 0, sizeof(WifiPasswordSnapshot));
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    if (store->settings_mode != SettingsMode::WifiPassword) {
        xSemaphoreGive(store->mutex);
        return false;
    }
    snapshot->symbols = store->wifi_password_symbols;
    snapshot->shift = store->wifi_password_shift;
    snapshot->connecting = store->wifi_connecting;
    copy_string(snapshot->target_ssid, sizeof(snapshot->target_ssid), store->wifi_target_ssid);
    copy_string(snapshot->password, sizeof(snapshot->password), store->wifi_password_input);
    copy_string(snapshot->connect_error, sizeof(snapshot->connect_error), store->wifi_connect_error);
    xSemaphoreGive(store->mutex);
    return true;
}

void store_note_interaction(EntityStore* store, uint32_t now_ms) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    store->last_interaction_ms = now_ms;
    xSemaphoreGive(store->mutex);
}

void store_poll_standby_timeout(EntityStore* store, uint32_t now_ms) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);

    const bool can_activate = store->settings_mode == SettingsMode::None && store->wifi == ConnState::Up &&
                              store->home_assistant == ConnState::Up;
    const bool idle_timed_out = static_cast<uint32_t>(now_ms - store->last_interaction_ms) >= STANDBY_IDLE_TIMEOUT_MS;
    bool changed = false;
    bool entered_standby = false;
    bool left_standby = false;

    if (store->standby_active) {
        if (!can_activate) {
            store->standby_active = false;
            store->standby_revision++;
            changed = true;
            left_standby = true;
        }
    } else if (can_activate && idle_timed_out) {
        store->standby_active = true;
        store->standby_entered_ms = now_ms;
        store->standby_last_refresh_ms = now_ms;
        store->standby_revision++;
        changed = true;
        entered_standby = true;
    }

    xSemaphoreGive(store->mutex);
    if (entered_standby) {
        ESP_LOGI(TAG, "Idle timeout reached, entering standby");
    } else if (left_standby) {
        ESP_LOGI(TAG, "Connection dropped, leaving standby");
    }
    if (changed) {
        notify_ui(store);
    }
}

bool store_is_standby_active(EntityStore* store) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    const bool active = store->standby_active;
    xSemaphoreGive(store->mutex);
    return active;
}

bool store_should_deep_sleep(EntityStore* store, uint32_t now_ms) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    const bool should = store->standby_active &&
                        static_cast<uint32_t>(now_ms - store->standby_entered_ms) >= DEEP_SLEEP_AFTER_STANDBY_MS;
    xSemaphoreGive(store->mutex);
    return should;
}

void store_set_battery_state(EntityStore* store, bool valid, uint8_t soc_pct, uint16_t voltage_mv) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    bool changed = false;
    if (store->battery_valid != valid) {
        store->battery_valid = valid;
        changed = true;
    }
    if (valid) {
        // Hysteresis: only invalidate the header on >=5% SoC change to avoid
        // refresh churn from sample noise.
        const int16_t delta = static_cast<int16_t>(soc_pct) - static_cast<int16_t>(store->battery_soc_pct);
        const int16_t abs_delta = delta < 0 ? -delta : delta;
        if (abs_delta >= 5) {
            store->battery_soc_pct = soc_pct;
            changed = true;
        }
        store->battery_voltage_mv = voltage_mv;
    } else {
        store->battery_soc_pct = 0;
        store->battery_voltage_mv = 0;
    }
    if (changed) {
        store->battery_revision++;
    }
    xSemaphoreGive(store->mutex);
    if (changed) {
        notify_ui(store);
    }
}

bool store_get_battery_state(EntityStore* store, uint8_t* soc_pct_out, uint16_t* voltage_mv_out) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    const bool valid = store->battery_valid;
    if (valid) {
        if (soc_pct_out) *soc_pct_out = store->battery_soc_pct;
        if (voltage_mv_out) *voltage_mv_out = store->battery_voltage_mv;
    }
    xSemaphoreGive(store->mutex);
    return valid;
}

void store_update_ui_state(EntityStore* store, UIState* ui_state) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);

    ui_state->wifi_list_page = store->wifi_list_page;
    ui_state->settings_revision = store->settings_revision;
    ui_state->standby_revision = store->standby_revision;
    ui_state->battery_revision = store->battery_revision;

    ui_state->media_device_idx = store->media_device_idx;
    ui_state->wifi_rssi = store->wifi_rssi;
    ui_state->wifi_connected = store->wifi_connected;

    if (store->settings_mode != SettingsMode::None) {
        switch (store->settings_mode) {
        case SettingsMode::Wifi:
            ui_state->mode = UiMode::WifiSettings;
            break;
        case SettingsMode::WifiPassword:
            ui_state->mode = UiMode::WifiPassword;
            break;
        default:
            ui_state->mode = UiMode::WifiSettings;
            break;
        }
    } else if (store->standby_active) {
        ui_state->mode = UiMode::Standby;
    } else if (store->wifi == ConnState::InvalidCredentials || store->wifi == ConnState::ConnectionError) {
        ui_state->mode = UiMode::WifiDisconnected;
    } else if (store->home_assistant == ConnState::InvalidCredentials) {
        ui_state->mode = UiMode::HassInvalidKey;
    } else if (store->home_assistant == ConnState::ConnectionError) {
        ui_state->mode = UiMode::HassDisconnected;
    } else if (store->media_device_select_active) {
        ui_state->mode = UiMode::MediaDeviceSelect;
    } else if (store->battery_status_active) {
        ui_state->mode = UiMode::BatteryStatus;
    } else {
        // Default: draw the media controller immediately. While wifi or HA
        // are still Initializing, the UI is up but commands queue until
        // the WS comes online.
        ui_state->mode = UiMode::MediaController;
    }

    xSemaphoreGive(store->mutex);
}

bool store_open_media_device_select(EntityStore* store) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    const bool changed = !store->media_device_select_active;
    store->media_device_select_active = true;
    if (changed) {
        store->settings_revision++;
    }
    xSemaphoreGive(store->mutex);
    if (changed) {
        notify_ui(store);
    }
    return changed;
}

bool store_close_media_device_select(EntityStore* store) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    const bool changed = store->media_device_select_active;
    store->media_device_select_active = false;
    if (changed) {
        store->settings_revision++;
    }
    xSemaphoreGive(store->mutex);
    if (changed) {
        notify_ui(store);
    }
    return changed;
}

bool store_set_media_device_idx(EntityStore* store, uint8_t idx, uint8_t device_count) {
    if (device_count == 0 || idx >= device_count) {
        return false;
    }
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    const bool changed = store->media_device_idx != idx;
    store->media_device_idx = idx;
    const bool was_selecting = store->media_device_select_active;
    store->media_device_select_active = false;
    if (changed || was_selecting) {
        store->settings_revision++;
    }
    xSemaphoreGive(store->mutex);

    if (changed) {
        Preferences prefs;
        if (prefs.begin(MEDIA_PREFS_NS, false)) {
            prefs.putUChar(MEDIA_PREFS_DEVICE_IDX, idx);
            prefs.end();
        }
    }
    if (changed || was_selecting) {
        notify_ui(store);
    }
    return changed;
}

uint8_t store_get_media_device_idx(EntityStore* store) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    const uint8_t idx = store->media_device_idx;
    xSemaphoreGive(store->mutex);
    return idx;
}

bool store_open_battery_status(EntityStore* store) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    const bool changed = !store->battery_status_active;
    store->battery_status_active = true;
    if (changed) {
        store->settings_revision++;
    }
    xSemaphoreGive(store->mutex);
    if (changed) {
        notify_ui(store);
    }
    return changed;
}

bool store_close_battery_status(EntityStore* store) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    const bool changed = store->battery_status_active;
    store->battery_status_active = false;
    if (changed) {
        store->settings_revision++;
    }
    xSemaphoreGive(store->mutex);
    if (changed) {
        notify_ui(store);
    }
    return changed;
}

void store_load_persisted_media_device_idx(EntityStore* store, uint8_t device_count) {
    if (device_count == 0) {
        return;
    }
    Preferences prefs;
    // RW open (despite this being a read-only path) so a fresh NVS auto-
    // creates the namespace instead of logging "nvs_open failed: NOT_FOUND".
    if (!prefs.begin(MEDIA_PREFS_NS, false)) {
        return;
    }
    const uint8_t saved = prefs.getUChar(MEDIA_PREFS_DEVICE_IDX, 0);
    prefs.end();
    const uint8_t clamped = saved < device_count ? saved : 0;
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    store->media_device_idx = clamped;
    xSemaphoreGive(store->mutex);
}

void store_wait_for_wifi_up(EntityStore* store) {
    xEventGroupWaitBits(store->event_group, BIT_WIFI_UP, pdFALSE, pdTRUE, portMAX_DELAY);
}

void store_flush_pending_commands(EntityStore* store) {
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    store->media_command_head = 0;
    store->media_command_tail = 0;
    store->media_command_count = 0;
    xSemaphoreGive(store->mutex);
}
