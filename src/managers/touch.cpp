#include "managers/touch.h"
#include "managers/home_assistant.h"
#include "managers/wifi.h"
#include "boards.h"
#include "config.h"
#include "constants.h"
#include <Arduino.h>
#include <Wire.h>
#include <driver/gpio.h>

static const char* TAG = "touch";

static volatile TaskHandle_t s_touch_task_handle = nullptr;

static void IRAM_ATTR touch_int_isr() {
    if (s_touch_task_handle) {
        BaseType_t hpw = pdFALSE;
        vTaskNotifyGiveFromISR(s_touch_task_handle, &hpw);
        portYIELD_FROM_ISR(hpw);
    }
}

static bool i2c_read_reg8(uint8_t addr, uint8_t reg, uint8_t* buf, size_t len) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }

    if (Wire.requestFrom(static_cast<int>(addr), static_cast<int>(len)) != static_cast<int>(len)) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        buf[i] = Wire.read();
    }
    return true;
}

static bool i2c_device_present(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

static bool i2c_read_reg16(uint8_t addr, uint16_t reg, uint8_t* buf, size_t len) {
    Wire.beginTransmission(addr);
    Wire.write(static_cast<uint8_t>(reg >> 8));
    Wire.write(static_cast<uint8_t>(reg & 0xFF));
    if (Wire.endTransmission(false) != 0) {
        return false;
    }

    if (Wire.requestFrom(static_cast<int>(addr), static_cast<int>(len)) != static_cast<int>(len)) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        buf[i] = Wire.read();
    }
    return true;
}

static bool i2c_write_reg16(uint8_t addr, uint16_t reg, uint8_t value) {
    Wire.beginTransmission(addr);
    Wire.write(static_cast<uint8_t>(reg >> 8));
    Wire.write(static_cast<uint8_t>(reg & 0xFF));
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

static bool i2c_read_reg16_block(uint8_t addr, uint16_t reg, uint8_t* buf, size_t len) {
    size_t offset = 0;
    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > 16) {
            chunk = 16;
        }
        if (!i2c_read_reg16(addr, reg + offset, buf + offset, chunk)) {
            return false;
        }
        offset += chunk;
    }
    return true;
}

static bool detect_gt911_address(uint8_t* out_addr) {
    if (i2c_device_present(GT911_ADDR1)) {
        *out_addr = GT911_ADDR1;
        return true;
    }
    if (i2c_device_present(GT911_ADDR2)) {
        *out_addr = GT911_ADDR2;
        return true;
    }
    return false;
}

// GT911 INT trigger modes (MODULE_SWITCH_1 register, bits 0-1):
//   0x00 = rising edge,  0x01 = falling edge,
//   0x02 = low level query,  0x03 = high level query.
// LOW_LEVEL_QUERY holds INT low until the host reads the data — convenient for
// polling, but unworkable for light-sleep wake: the level-triggered GPIO wake
// keeps re-firing gpio_intr_service while INT stays low, faster than the
// touch task can wake and drain GT911, tripping IWDT. FALLING_EDGE pulses INT
// briefly on each event then releases it high — the pulse triggers the
// level-low wake but level returns high before gpio_intr_service can spin.
static bool configure_gt911_int_mode(uint8_t addr, uint8_t mode_bits) {
    constexpr uint16_t GT911_CFG_START_REG = 0x8047;
    constexpr uint16_t GT911_MODULE_SWITCH_1_REG = 0x804D;
    constexpr uint16_t GT911_CFG_CHECKSUM_REG = 0x80FF;
    constexpr uint16_t GT911_CFG_FRESH_REG = 0x8100;

    uint8_t module_switch = 0;
    if (!i2c_read_reg16(addr, GT911_MODULE_SWITCH_1_REG, &module_switch, 1)) {
        return false;
    }
    const uint8_t desired_mode = static_cast<uint8_t>((module_switch & 0xFC) | (mode_bits & 0x03));

    if (!i2c_write_reg16(addr, GT911_MODULE_SWITCH_1_REG, desired_mode)) {
        return false;
    }

    uint8_t config[184] = {0};
    if (!i2c_read_reg16_block(addr, GT911_CFG_START_REG, config, sizeof(config))) {
        return false;
    }
    config[GT911_MODULE_SWITCH_1_REG - GT911_CFG_START_REG] = desired_mode;

    uint8_t checksum = 0;
    for (size_t i = 0; i < sizeof(config); i++) {
        checksum = static_cast<uint8_t>(checksum + config[i]);
    }
    checksum = static_cast<uint8_t>(~checksum + 1);

    if (!i2c_write_reg16(addr, GT911_CFG_CHECKSUM_REG, checksum)) {
        return false;
    }
    if (!i2c_write_reg16(addr, GT911_CFG_FRESH_REG, 0x01)) {
        return false;
    }
    return true;
}

static bool gt911_home_button_pressed_edge(uint8_t gt911_addr) {
    static bool was_pressed = false;
    uint8_t status = 0;
    if (!i2c_read_reg16(gt911_addr, GT911_POINT_INFO, &status, 1)) {
        return false;
    }

    const bool pressed = (status & 0x10) != 0;

    const bool is_edge = pressed && !was_pressed;
    was_pressed = pressed;
    return is_edge;
}

static bool cst226_home_button_pressed_edge() {
    static bool was_pressed = false;
    uint8_t buf[28] = {0};
    if (!i2c_read_reg8(CST226_ADDR, 0x00, buf, sizeof(buf))) {
        return false;
    }

    const bool pressed = buf[0] == 0x83 && buf[1] == 0x17 && buf[5] == 0x80;
    const bool is_edge = pressed && !was_pressed;
    was_pressed = pressed;
    return is_edge;
}

static bool is_back_button_touched(const TouchEvent* touch_event) {
    return touch_event->x >= ROOM_CONTROLS_BACK_X && touch_event->x < ROOM_CONTROLS_BACK_X + ROOM_CONTROLS_BACK_W &&
           touch_event->y >= ROOM_CONTROLS_BACK_Y && touch_event->y < ROOM_CONTROLS_BACK_Y + ROOM_CONTROLS_BACK_H;
}

static bool is_wifi_scan_button_touched(const TouchEvent* touch_event) {
    return touch_event->x >= WIFI_SCAN_BUTTON_X && touch_event->x < WIFI_SCAN_BUTTON_X + WIFI_SCAN_BUTTON_W &&
           touch_event->y >= WIFI_SCAN_BUTTON_Y && touch_event->y < WIFI_SCAN_BUTTON_Y + WIFI_SCAN_BUTTON_H;
}

static bool is_wifi_default_button_touched(const TouchEvent* touch_event) {
    return touch_event->x >= WIFI_DEFAULT_BUTTON_X && touch_event->x < WIFI_DEFAULT_BUTTON_X + WIFI_DEFAULT_BUTTON_W &&
           touch_event->y >= WIFI_DEFAULT_BUTTON_Y && touch_event->y < WIFI_DEFAULT_BUTTON_Y + WIFI_DEFAULT_BUTTON_H;
}

static bool is_wifi_disc_retry_touched(const TouchEvent* touch_event) {
    return touch_event->x >= WIFI_DISC_RETRY_X && touch_event->x < WIFI_DISC_RETRY_X + WIFI_DISC_RETRY_W &&
           touch_event->y >= WIFI_DISC_BUTTON_Y && touch_event->y < WIFI_DISC_BUTTON_Y + WIFI_DISC_BUTTON_H;
}

static bool is_wifi_disc_settings_touched(const TouchEvent* touch_event) {
    return touch_event->x >= WIFI_DISC_SETTINGS_X && touch_event->x < WIFI_DISC_SETTINGS_X + WIFI_DISC_SETTINGS_W &&
           touch_event->y >= WIFI_DISC_BUTTON_Y && touch_event->y < WIFI_DISC_BUTTON_Y + WIFI_DISC_BUTTON_H;
}


static int8_t list_swipe_delta(const TouchEvent* start_touch, const TouchEvent* end_touch) {
    const int16_t dx = static_cast<int16_t>(end_touch->x) - static_cast<int16_t>(start_touch->x);
    const int16_t dy = static_cast<int16_t>(end_touch->y) - static_cast<int16_t>(start_touch->y);
    const int16_t abs_dx = dx >= 0 ? dx : -dx;
    const int16_t abs_dy = dy >= 0 ? dy : -dy;

    if (abs_dx < ROOM_LIST_SWIPE_THRESHOLD_X || abs_dx <= abs_dy) {
        return 0;
    }

    // Swipe left -> next page, swipe right -> previous page.
    return dx < 0 ? 1 : -1;
}

static int16_t wifi_network_index_from_touch(const TouchEvent* touch_event, uint8_t network_count, uint8_t page) {
    if (touch_event->x < WIFI_NETWORK_LIST_X || touch_event->x >= WIFI_NETWORK_LIST_X + WIFI_NETWORK_LIST_W) {
        return -1;
    }
    if (touch_event->y < WIFI_NETWORK_LIST_Y) {
        return -1;
    }

    const int16_t stride = WIFI_NETWORK_ROW_H + WIFI_NETWORK_ROW_GAP;
    const int16_t rel_y = touch_event->y - WIFI_NETWORK_LIST_Y;
    const int16_t row = rel_y / stride;
    if (row < 0 || row >= WIFI_NETWORKS_PER_PAGE) {
        return -1;
    }
    const int16_t in_row_y = rel_y % stride;
    if (in_row_y >= WIFI_NETWORK_ROW_H) {
        return -1;
    }

    const int16_t idx = static_cast<int16_t>(page) * WIFI_NETWORKS_PER_PAGE + row;
    return idx < network_count ? idx : -1;
}

enum class WifiPasswordActionType : uint8_t {
    None,
    Append,
    ToggleShift,
    ToggleSymbols,
    Space,
    Backspace,
    Clear,
    Connect,
};

struct WifiPasswordAction {
    WifiPasswordActionType type = WifiPasswordActionType::None;
    char ch = '\0';
};

static bool point_in_rect(const TouchEvent* touch_event, int16_t x, int16_t y, int16_t w, int16_t h) {
    return touch_event->x >= x && touch_event->x < x + w && touch_event->y >= y && touch_event->y < y + h;
}

static WifiPasswordAction wifi_password_action_from_touch(const TouchEvent* touch_event, bool symbols, bool shift) {
    WifiPasswordAction action = {};
    const int16_t key_w = static_cast<int16_t>((WIFI_KEYBOARD_W - 9 * WIFI_KEY_GAP) / 10);
    const int16_t row1_y = WIFI_KEYBOARD_Y;
    const int16_t row2_y = row1_y + WIFI_KEY_H + WIFI_KEY_GAP;
    const int16_t row3_y = row2_y + WIFI_KEY_H + WIFI_KEY_GAP;
    const int16_t row4_y = row3_y + WIFI_KEY_H + WIFI_KEY_GAP;
    const int16_t row5_y = row4_y + WIFI_KEY_H + WIFI_KEY_GAP;

    auto try_row = [&](int16_t row_x, int16_t row_y, const char* keys, uint8_t count) -> bool {
        for (uint8_t i = 0; i < count; i++) {
            int16_t x = static_cast<int16_t>(row_x + i * (key_w + WIFI_KEY_GAP));
            if (point_in_rect(touch_event, x, row_y, key_w, WIFI_KEY_H)) {
                char ch = keys[i];
                if (!symbols && shift && ch >= 'a' && ch <= 'z') {
                    ch = static_cast<char>(ch - ('a' - 'A'));
                }
                action.type = WifiPasswordActionType::Append;
                action.ch = ch;
                return true;
            }
        }
        return false;
    };

    const char* row1 = symbols ? "1234567890" : "qwertyuiop";
    const char* row2 = symbols ? "!@#$%^&*()" : "asdfghjkl";
    const char* row3 = symbols ? "-_=+.,:/?" : "zxcvbnm";

    if (try_row(WIFI_KEYBOARD_X, row1_y, row1, 10)) {
        return action;
    }
    if (try_row(static_cast<int16_t>(WIFI_KEYBOARD_X + (key_w + WIFI_KEY_GAP) / 2), row2_y, row2, 9)) {
        return action;
    }
    if (try_row(static_cast<int16_t>(WIFI_KEYBOARD_X + 2 * (key_w + WIFI_KEY_GAP)), row3_y, row3, 7)) {
        return action;
    }

    const int16_t shift_w = 88;
    const int16_t symbols_w = 92;
    const int16_t space_w = 180;
    const int16_t del_w = 88;
    const int16_t clear_w = 88;
    int16_t x = WIFI_KEYBOARD_X;
    if (point_in_rect(touch_event, x, row4_y, shift_w, WIFI_KEY_H)) {
        action.type = WifiPasswordActionType::ToggleShift;
        return action;
    }
    x += shift_w + WIFI_KEY_GAP;
    if (point_in_rect(touch_event, x, row4_y, symbols_w, WIFI_KEY_H)) {
        action.type = WifiPasswordActionType::ToggleSymbols;
        return action;
    }
    x += symbols_w + WIFI_KEY_GAP;
    if (point_in_rect(touch_event, x, row4_y, space_w, WIFI_KEY_H)) {
        action.type = WifiPasswordActionType::Space;
        return action;
    }
    x += space_w + WIFI_KEY_GAP;
    if (point_in_rect(touch_event, x, row4_y, del_w, WIFI_KEY_H)) {
        action.type = WifiPasswordActionType::Backspace;
        return action;
    }
    x += del_w + WIFI_KEY_GAP;
    if (point_in_rect(touch_event, x, row4_y, clear_w, WIFI_KEY_H)) {
        action.type = WifiPasswordActionType::Clear;
        return action;
    }

    if (point_in_rect(touch_event, WIFI_KEYBOARD_X, row5_y, WIFI_KEYBOARD_W, WIFI_KEY_H)) {
        action.type = WifiPasswordActionType::Connect;
        return action;
    }

    return action;
}

enum class MediaButton : uint8_t {
    None,
    Settings,
    Title,
    Battery,
    Power, Mute, VolDown, VolUp,
    Back, Home, Info,
    Touchpad, // returned by hit-test; resolves to DpadUp/Down/Left/Right/Ok at release based on swipe
    DpadUp, DpadDown, DpadLeft, DpadRight, DpadOk,
    Rewind, InstantReplay, PlayPause, FastForward,
    Source0, Source1, Source2, Source3, Source4, Source5,
};

static bool point_in(const TouchEvent* t, int16_t x, int16_t y, int16_t w, int16_t h) {
    return t->x >= x && t->x < x + w && t->y >= y && t->y < y + h;
}

static MediaButton media_button_from_touch(const TouchEvent* t) {
    // Wi-Fi indicator (top-left) — tap opens Wi-Fi settings.
    if (point_in(t, HOME_LEFT_BUTTON_X, HOME_LEFT_BUTTON_Y, HOME_LEFT_BUTTON_W, HOME_LEFT_BUTTON_H)) {
        return MediaButton::Settings;
    }

    // Power button (top-right).
    if (point_in(t, HOME_RIGHT_BUTTON_X, HOME_RIGHT_BUTTON_Y, HOME_RIGHT_BUTTON_W, HOME_RIGHT_BUTTON_H)) {
        return MediaButton::Power;
    }

    // Battery indicator slot — tap opens the battery status page.
    {
        constexpr int16_t batt_slot_w = 40; // covers the 16 px body and ~24 px text width
        const int16_t batt_x = HOME_LEFT_BUTTON_X + HOME_LEFT_BUTTON_W;
        if (point_in(t, batt_x, HOME_LEFT_BUTTON_Y, batt_slot_w, HOME_LEFT_BUTTON_H)) {
            return MediaButton::Battery;
        }
    }

    // Title region: between the battery slot and the Power button. Tapping
    // here opens the device selector.
    {
        constexpr int16_t batt_slot_w = 40;
        const int16_t title_x = HOME_LEFT_BUTTON_X + HOME_LEFT_BUTTON_W + batt_slot_w;
        const int16_t title_w = HOME_RIGHT_BUTTON_X - title_x;
        if (title_w > 0 && point_in(t, title_x, HOME_LEFT_BUTTON_Y, title_w, HOME_LEFT_BUTTON_H)) {
            return MediaButton::Title;
        }
    }

    // Top 3x3 grid: right column = volume; (0,0) = power; rest = sources.
    {
        const int16_t inner_w = DISPLAY_WIDTH - 2 * MEDIA_MARGIN_X;
        const int16_t btn_w = (inner_w - 2 * MEDIA_BUTTON_GAP) / 3;
        const int16_t col_x[3] = {
            MEDIA_MARGIN_X,
            MEDIA_MARGIN_X + (btn_w + MEDIA_BUTTON_GAP),
            MEDIA_MARGIN_X + 2 * (btn_w + MEDIA_BUTTON_GAP),
        };
        const int16_t row_y[3] = {MEDIA_VOLUME_ROW_Y, MEDIA_VOLUME_ROW2_Y, MEDIA_VOLUME_ROW3_Y};
        if (point_in(t, col_x[2], row_y[0], btn_w, MEDIA_BUTTON_H)) return MediaButton::VolUp;
        if (point_in(t, col_x[2], row_y[1], btn_w, MEDIA_BUTTON_H)) return MediaButton::VolDown;
        if (point_in(t, col_x[2], row_y[2], btn_w, MEDIA_BUTTON_H)) return MediaButton::Mute;
        if (point_in(t, col_x[0], row_y[0], btn_w, MEDIA_BUTTON_H)) return MediaButton::Source0;
        if (point_in(t, col_x[1], row_y[0], btn_w, MEDIA_BUTTON_H)) return MediaButton::Source1;
        if (point_in(t, col_x[0], row_y[1], btn_w, MEDIA_BUTTON_H)) return MediaButton::Source2;
        if (point_in(t, col_x[1], row_y[1], btn_w, MEDIA_BUTTON_H)) return MediaButton::Source3;
        if (point_in(t, col_x[0], row_y[2], btn_w, MEDIA_BUTTON_H)) return MediaButton::Source4;
        if (point_in(t, col_x[1], row_y[2], btn_w, MEDIA_BUTTON_H)) return MediaButton::Source5;
    }

    // Nav row
    {
        const int16_t inner_w = DISPLAY_WIDTH - 2 * MEDIA_MARGIN_X;
        const int16_t btn_w = (inner_w - 2 * MEDIA_BUTTON_GAP) / 3;
        const MediaButton ids[3] = {MediaButton::Back, MediaButton::Home, MediaButton::Info};
        for (uint8_t i = 0; i < 3; i++) {
            const int16_t x = MEDIA_MARGIN_X + i * (btn_w + MEDIA_BUTTON_GAP);
            if (point_in(t, x, MEDIA_NAV_ROW_Y, btn_w, MEDIA_BUTTON_H)) {
                return ids[i];
            }
        }
    }

    // Touchpad width matches the buttons above/below; swipe disambiguated at release.
    {
        const int16_t pad_w = DISPLAY_WIDTH - 2 * MEDIA_MARGIN_X;
        if (point_in(t, MEDIA_MARGIN_X, MEDIA_DPAD_Y, pad_w, MEDIA_DPAD_SIZE)) {
            return MediaButton::Touchpad;
        }
    }

    // Transport row
    {
        const int16_t inner_w = DISPLAY_WIDTH - 2 * MEDIA_MARGIN_X;
        const int16_t btn_w = (inner_w - 3 * MEDIA_BUTTON_GAP) / 4;
        const MediaButton ids[4] = {MediaButton::Rewind, MediaButton::InstantReplay, MediaButton::PlayPause, MediaButton::FastForward};
        for (uint8_t i = 0; i < 4; i++) {
            const int16_t x = MEDIA_MARGIN_X + i * (btn_w + MEDIA_BUTTON_GAP);
            if (point_in(t, x, MEDIA_TRANSPORT_ROW_Y, btn_w, MEDIA_BUTTON_H)) {
                return ids[i];
            }
        }
    }

    return MediaButton::None;
}

static void media_dispatch_button(EntityStore* store, const Configuration* config, MediaButton btn) {
    if (!config || config->media_device_count == 0) return;
    const uint8_t device_idx = store_get_media_device_idx(store);
    const uint8_t safe_idx = device_idx < config->media_device_count ? device_idx : 0;
    const MediaDevice& device = config->media_devices[safe_idx];
    const MediaRemoteCommands& cmds = config->media_remote_commands;
    const char* remote = device.remote_entity_id;
    const char* volume = device.volume_entity_id;

    auto remote_cmd = [&](const char* name) {
        if (remote && name) {
            store_send_media_command(store, CommandType::RemoteSendCommand, remote, name);
        }
    };

    switch (btn) {
    case MediaButton::Power:
        if (device.power_action.domain) {
            store_send_hass_action(store, &device.power_action);
        }
        break;
    case MediaButton::Mute:
        if (volume) store_send_media_command(store, CommandType::MediaVolumeMute, volume, nullptr);
        break;
    case MediaButton::VolDown:
        if (volume) store_send_media_command(store, CommandType::MediaVolumeDown, volume, nullptr);
        break;
    case MediaButton::VolUp:
        if (volume) store_send_media_command(store, CommandType::MediaVolumeUp, volume, nullptr);
        break;
    case MediaButton::Back:           remote_cmd(cmds.back); break;
    case MediaButton::Home:           remote_cmd(cmds.home); break;
    case MediaButton::Info:           remote_cmd(cmds.menu); break;
    case MediaButton::DpadUp:         remote_cmd(cmds.up); break;
    case MediaButton::DpadDown:       remote_cmd(cmds.down); break;
    case MediaButton::DpadLeft:       remote_cmd(cmds.left); break;
    case MediaButton::DpadRight:      remote_cmd(cmds.right); break;
    case MediaButton::DpadOk:         remote_cmd(cmds.select); break;
    case MediaButton::Rewind:         remote_cmd(cmds.rewind); break;
    case MediaButton::InstantReplay:  remote_cmd(cmds.previous); break;
    case MediaButton::PlayPause:      remote_cmd(cmds.play_pause); break;
    case MediaButton::FastForward:    remote_cmd(cmds.fast_forward); break;
    case MediaButton::Source0:
    case MediaButton::Source1:
    case MediaButton::Source2:
    case MediaButton::Source3:
    case MediaButton::Source4:
    case MediaButton::Source5: {
        const size_t idx = static_cast<size_t>(btn) - static_cast<size_t>(MediaButton::Source0);
        if (idx < MEDIA_SOURCE_COUNT && device.source_entity_id && device.sources[idx].source_name) {
            store_send_media_command(store, CommandType::MediaSelectSource,
                                     device.source_entity_id,
                                     device.sources[idx].source_name);
        }
        break;
    }
    default: break;
    }
}

void touch_task(void* arg) {
    TouchTaskArgs* ctx = static_cast<TouchTaskArgs*>(arg);
    BBCapTouch* bbct = ctx->bbct;
    EntityStore* store = ctx->store;
    const Configuration* config = ctx->config;

    // UI State values
    uint32_t ui_state_version = 0;
    auto* ui_state = new UIState{};

    // Touch infos
    TOUCHINFO ti;
    TouchEvent touch_event = TouchEvent{};
    TouchEvent touch_start = TouchEvent{};
    TouchEvent touch_end = TouchEvent{};
    static WifiSettingsSnapshot wifi_settings_snapshot = {};
    static WifiPasswordSnapshot wifi_password_snapshot = {};
    bool touching = false;
    bool swallow_touch_release = false;
    uint32_t last_touch_ms = 0;
    uint32_t standby_touch_ignore_until_ms = 0;
    // Auto-repeat: when a held button supports it (volume), retrigger every
    // AUTOREPEAT_INTERVAL_MS until release. None disables.
    MediaButton autorepeat_btn = MediaButton::None;
    uint32_t last_repeat_ms = 0;
    constexpr uint32_t AUTOREPEAT_INTERVAL_MS = 200;

    // Initialize touch. Wait for the panel power rail to come up before
    // poking the GT911 over I2C. epaper.einkPower(true) in setup() returns
    // before the rail is fully stable, and on a wake-from-deep-sleep boot
    // the GT911 hasn't yet released its I2C lines — bbct.init() then
    // misdetects the chip as CST226 and the bus errors with
    // ESP_ERR_INVALID_STATE.
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "Initializing touchscreen...");
    int rc = bbct->init(TOUCH_SDA, TOUCH_SCL, TOUCH_RST, TOUCH_INT);
    ESP_LOGI(TAG, "init() rc = %d", rc);
    int type = bbct->sensorType();
    ESP_LOGI(TAG, "Sensor type = %d", type);
    uint8_t gt911_addr = 0;
    const bool gt911_addr_present = detect_gt911_address(&gt911_addr);
    const bool cst226_addr_present = i2c_device_present(CST226_ADDR);
    const bool poll_gt911_home = (type == CT_TYPE_GT911) || gt911_addr_present;
    const bool poll_cst226_home = (type == CT_TYPE_CST226) || cst226_addr_present;
    ESP_LOGI(TAG, "Touch home key polling: GT911=%d CST226=%d", poll_gt911_home ? 1 : 0, poll_cst226_home ? 1 : 0);
    if (poll_gt911_home && gt911_addr_present) {
#if defined(ENABLE_PM_LIGHT_SLEEP)
        // Falling-edge so INT pulses on each event instead of holding low —
        // see configure_gt911_int_mode comment for why this matters under
        // light-sleep wake.
        constexpr uint8_t kGt911IntMode = 0x01; // FALLING_EDGE
        const char* kGt911IntModeName = "FALLING_EDGE";
#else
        constexpr uint8_t kGt911IntMode = 0x02; // LOW_LEVEL_QUERY
        const char* kGt911IntModeName = "LOW_LEVEL_QUERY";
#endif
        if (configure_gt911_int_mode(gt911_addr, kGt911IntMode)) {
            ESP_LOGI(TAG, "Configured GT911 interrupt mode to %s", kGt911IntModeName);
        } else {
            ESP_LOGW(TAG, "Failed to configure GT911 interrupt mode");
        }
#if defined(ENABLE_PM_LIGHT_SLEEP)
        gpio_wakeup_enable(static_cast<gpio_num_t>(TOUCH_INT), GPIO_INTR_LOW_LEVEL);
#endif
    }

    // Wake on TOUCH_INT instead of polling. GT911 in LOW_LEVEL_QUERY pulls INT
    // low on every touch/key event; CST226 also drives INT on touch events.
    s_touch_task_handle = xTaskGetCurrentTaskHandle();
    attachInterrupt(digitalPinToInterrupt(TOUCH_INT), touch_int_isr, FALLING);

    while (true) {
        const uint32_t now_ms = millis();
        if (!touching) {
            swallow_touch_release = false;
        }
        if (!touching) {
            bool go_home = false;
            if (poll_gt911_home && gt911_addr_present && gt911_home_button_pressed_edge(gt911_addr)) {
                ESP_LOGI(TAG, "Home button pressed (GT911 key)");
                go_home = true;
            }
            if (poll_cst226_home && cst226_home_button_pressed_edge()) {
                ESP_LOGI(TAG, "Home button pressed (CST226 key)");
                go_home = true;
            }
            if (go_home) {
                if (now_ms < standby_touch_ignore_until_ms) {
                    vTaskDelay(pdMS_TO_TICKS(25));
                    continue;
                }
                store_note_interaction(store, now_ms);
                store_go_home(store);
                vTaskDelay(pdMS_TO_TICKS(25));
                continue;
            }
        }

        if (bbct->getSamples(&ti)) {
            last_touch_ms = now_ms;
            store_note_interaction(store, last_touch_ms);
            ui_state_copy(ctx->state, &ui_state_version, ui_state);

            if (ui_state->mode == UiMode::Standby) {
                if (now_ms < standby_touch_ignore_until_ms) {
                    touching = false;
                    continue;
                }
                store_go_home(store);
                touching = false;
                continue;
            }

            if (ui_state->mode == UiMode::WifiSettings) {
                if (!touching) {
                    touch_event.x = ti.x[0];
                    touch_event.y = ti.y[0];
                    if (is_back_button_touched(&touch_event)) {
                        store_settings_back(store);
                        touch_start = touch_event;
                        touch_end = touch_event;
                        touching = true;
                        swallow_touch_release = true;
                        continue;
                    }
                    if (is_wifi_scan_button_touched(&touch_event)) {
                        wifi_request_scan();
                        touch_start = touch_event;
                        touch_end = touch_event;
                        touching = true;
                        swallow_touch_release = true;
                        continue;
                    }
                    if (is_wifi_default_button_touched(&touch_event)) {
                        store_get_wifi_settings_snapshot(store, &wifi_settings_snapshot);
                        if (wifi_settings_snapshot.custom_profile_active) {
                            wifi_reset_to_default();
                            touch_start = touch_event;
                            touch_end = touch_event;
                            touching = true;
                            swallow_touch_release = true;
                            continue;
                        }
                        // No custom profile active → button isn't drawn; let
                        // the tap fall through to the default handler.
                    }
                    touch_start = touch_event;
                    touch_end = touch_event;
                    touching = true;
                    swallow_touch_release = false;
                } else {
                    touch_end.x = ti.x[0];
                    touch_end.y = ti.y[0];
                }
                continue;
            }

            if (ui_state->mode == UiMode::WifiPassword) {
                if (!touching) {
                    touch_event.x = ti.x[0];
                    touch_event.y = ti.y[0];
                    if (is_back_button_touched(&touch_event)) {
                        store_settings_back(store);
                        touch_start = touch_event;
                        touch_end = touch_event;
                        touching = true;
                        swallow_touch_release = true;
                        continue;
                    }
                    touch_start = touch_event;
                    touch_end = touch_event;
                    touching = true;
                    swallow_touch_release = false;
                } else {
                    touch_end.x = ti.x[0];
                    touch_end.y = ti.y[0];
                }
                continue;
            }

            if (ui_state->mode == UiMode::MediaController) {
                if (!touching) {
                    touch_event.x = ti.x[0];
                    touch_event.y = ti.y[0];
                    touch_start = touch_event;
                    touch_end = touch_event;
                    touching = true;
                    autorepeat_btn = MediaButton::None;
                    // Touchpad needs swipe-vs-tap disambiguation at release.
                    // Every other button is a discrete press: fire on touch
                    // down so rapid tapping doesn't have to wait for the
                    // release pendulum.
                    const MediaButton btn = media_button_from_touch(&touch_start);
                    if (btn == MediaButton::None || btn == MediaButton::Touchpad) {
                        swallow_touch_release = false;
                    } else {
                        if (btn == MediaButton::Settings) {
                            store_open_wifi_settings(store);
                            wifi_request_scan();
                        } else if (btn == MediaButton::Title) {
                            store_open_media_device_select(store);
                        } else if (btn == MediaButton::Battery) {
                            store_open_battery_status(store);
                        } else {
                            media_dispatch_button(store, config, btn);
                        }
                        swallow_touch_release = true;
                        if (btn == MediaButton::VolUp || btn == MediaButton::VolDown) {
                            autorepeat_btn = btn;
                            last_repeat_ms = now_ms;
                        }
                    }
                } else {
                    touch_end.x = ti.x[0];
                    touch_end.y = ti.y[0];
                    // Touchpad swipe: dispatch the dominant direction as soon as
                    // motion crosses the threshold, instead of waiting for the
                    // user to lift. swallow_touch_release doubles as the
                    // "already fired" latch so we don't re-dispatch on every
                    // sample after the threshold is crossed, and so the release
                    // path skips its own swipe/tap dispatch.
                    if (!swallow_touch_release &&
                        media_button_from_touch(&touch_start) == MediaButton::Touchpad) {
                        constexpr int16_t SWIPE_THRESHOLD = 5;
                        const int16_t dx = static_cast<int16_t>(touch_end.x) - static_cast<int16_t>(touch_start.x);
                        const int16_t dy = static_cast<int16_t>(touch_end.y) - static_cast<int16_t>(touch_start.y);
                        const int16_t abs_dx = dx < 0 ? -dx : dx;
                        const int16_t abs_dy = dy < 0 ? -dy : dy;
                        if (abs_dx >= SWIPE_THRESHOLD || abs_dy >= SWIPE_THRESHOLD) {
                            const MediaButton action = abs_dx > abs_dy
                                ? (dx > 0 ? MediaButton::DpadRight : MediaButton::DpadLeft)
                                : (dy > 0 ? MediaButton::DpadDown : MediaButton::DpadUp);
                            media_dispatch_button(store, config, action);
                            swallow_touch_release = true;
                        }
                    }
                    if (autorepeat_btn != MediaButton::None &&
                        static_cast<uint32_t>(now_ms - last_repeat_ms) >= AUTOREPEAT_INTERVAL_MS) {
                        media_dispatch_button(store, config, autorepeat_btn);
                        last_repeat_ms = now_ms;
                    }
                }
                continue;
            }

            if (ui_state->mode == UiMode::MediaDeviceSelect) {
                if (!touching) {
                    touch_event.x = ti.x[0];
                    touch_event.y = ti.y[0];
                    touch_start = touch_event;
                    touch_end = touch_event;
                    touching = true;
                    swallow_touch_release = true;

                    if (is_back_button_touched(&touch_event)) {
                        store_close_media_device_select(store);
                        continue;
                    }

                    constexpr int16_t row_h = 96;
                    constexpr int16_t row_gap = 18;
                    const int16_t row_w = DISPLAY_WIDTH - 2 * MEDIA_MARGIN_X;
                    const int16_t y0 = SETTINGS_HEADER_HEIGHT + 32;
                    const uint8_t device_count = config ? static_cast<uint8_t>(config->media_device_count) : 0;
                    if (touch_event.x >= MEDIA_MARGIN_X && touch_event.x < MEDIA_MARGIN_X + row_w) {
                        for (uint8_t i = 0; i < device_count; i++) {
                            const int16_t y = y0 + static_cast<int16_t>(i) * (row_h + row_gap);
                            if (touch_event.y >= y && touch_event.y < y + row_h) {
                                store_set_media_device_idx(store, i, device_count);
                                break;
                            }
                        }
                    }
                } else {
                    touch_end.x = ti.x[0];
                    touch_end.y = ti.y[0];
                }
                continue;
            }

            if (ui_state->mode == UiMode::BatteryStatus) {
                if (!touching) {
                    touch_event.x = ti.x[0];
                    touch_event.y = ti.y[0];
                    touch_start = touch_event;
                    touch_end = touch_event;
                    touching = true;
                    swallow_touch_release = true;

                    if (is_back_button_touched(&touch_event)) {
                        store_close_battery_status(store);
                    }
                } else {
                    touch_end.x = ti.x[0];
                    touch_end.y = ti.y[0];
                }
                continue;
            }

            if (ui_state->mode == UiMode::WifiDisconnected || ui_state->mode == UiMode::HassDisconnected) {
                if (!touching) {
                    touch_event.x = ti.x[0];
                    touch_event.y = ti.y[0];
                    if (is_wifi_disc_retry_touched(&touch_event)) {
                        // Wi-Fi screen retries the link; HA screen pokes the
                        // HA task to probe now instead of bouncing Wi-Fi for
                        // no reason.
                        if (ui_state->mode == UiMode::HassDisconnected) {
                            hass_request_probe();
                        } else {
                            wifi_reconnect();
                        }
                        touch_start = touch_event;
                        touch_end = touch_event;
                        touching = true;
                        swallow_touch_release = true;
                        continue;
                    }
                    if (is_wifi_disc_settings_touched(&touch_event)) {
                        store_open_wifi_settings(store);
                        wifi_request_scan();
                        touch_start = touch_event;
                        touch_end = touch_event;
                        touching = true;
                        swallow_touch_release = true;
                        continue;
                    }
                    touch_start = touch_event;
                    touch_end = touch_event;
                    touching = true;
                    swallow_touch_release = false;
                } else {
                    touch_end.x = ti.x[0];
                    touch_end.y = ti.y[0];
                }
                continue;
            }

            // Other modes: ignore touch.
            continue;
        } else {
            if (touching) {
                ui_state_copy(ctx->state, &ui_state_version, ui_state);

                if (swallow_touch_release) {
                    if (millis() - last_touch_ms > TOUCH_RELEASE_TIMEOUT_MS) {
                        touching = false;
                        swallow_touch_release = false;
                        autorepeat_btn = MediaButton::None;
                    }
                    vTaskDelay(pdMS_TO_TICKS(TOUCH_RELEASE_POLL_MS));
                    continue;
                }

                if (ui_state->mode == UiMode::WifiSettings && millis() - last_touch_ms > TOUCH_RELEASE_TIMEOUT_MS) {
                    int8_t page_delta = list_swipe_delta(&touch_start, &touch_end);
                    if (page_delta != 0) {
                        store_shift_wifi_list_page(store, page_delta);
                    } else {
                        store_get_wifi_settings_snapshot(store, &wifi_settings_snapshot);
                        int16_t network_idx = wifi_network_index_from_touch(&touch_start, wifi_settings_snapshot.network_count,
                                                                            ui_state->wifi_list_page);
                        if (network_idx >= 0) {
                            const WifiNetwork& network = wifi_settings_snapshot.networks[network_idx];
                            if (network.known) {
                                char saved_password[MAX_WIFI_PASSWORD_LEN + 1] = {0};
                                if (wifi_find_saved_password(network.ssid, saved_password, sizeof(saved_password))) {
                                    wifi_connect_to_network(network.ssid, saved_password);
                                } else if (network.secure) {
                                    store_open_wifi_password(store, network.ssid);
                                } else {
                                    wifi_connect_to_network(network.ssid, "");
                                }
                            } else if (network.secure) {
                                store_open_wifi_password(store, network.ssid);
                            } else {
                                wifi_connect_to_network(network.ssid, "");
                            }
                        }
                    }
                    touching = false;
                    swallow_touch_release = false;
                    continue;
                }

                if (ui_state->mode == UiMode::WifiPassword && millis() - last_touch_ms > TOUCH_RELEASE_TIMEOUT_MS) {
                    if (store_get_wifi_password_snapshot(store, &wifi_password_snapshot)) {
                        WifiPasswordAction action = wifi_password_action_from_touch(
                            &touch_start, wifi_password_snapshot.symbols, wifi_password_snapshot.shift);
                        switch (action.type) {
                        case WifiPasswordActionType::Append:
                            store_append_wifi_password_char(store, action.ch);
                            if (!wifi_password_snapshot.symbols && wifi_password_snapshot.shift) {
                                store_toggle_wifi_password_shift(store);
                            }
                            break;
                        case WifiPasswordActionType::ToggleShift:
                            store_toggle_wifi_password_shift(store);
                            break;
                        case WifiPasswordActionType::ToggleSymbols:
                            store_set_wifi_password_symbols(store, !wifi_password_snapshot.symbols);
                            break;
                        case WifiPasswordActionType::Space:
                            store_append_wifi_password_char(store, ' ');
                            break;
                        case WifiPasswordActionType::Backspace:
                            store_backspace_wifi_password(store);
                            break;
                        case WifiPasswordActionType::Clear:
                            store_clear_wifi_password(store);
                            break;
                        case WifiPasswordActionType::Connect:
                            wifi_connect_to_network(wifi_password_snapshot.target_ssid, wifi_password_snapshot.password);
                            store_open_wifi_settings(store);
                            break;
                        default:
                            break;
                        }
                    }
                    touching = false;
                    swallow_touch_release = false;
                    continue;
                }

                if (ui_state->mode == UiMode::MediaController && millis() - last_touch_ms > TOUCH_RELEASE_TIMEOUT_MS) {
                    // Buttons fire on touch-down (with swallow_touch_release set);
                    // only the touchpad reaches here. A swipe past the threshold
                    // dispatches the dominant direction; anything shorter is a
                    // tap, which fires OK/Select.
                    if (media_button_from_touch(&touch_start) == MediaButton::Touchpad) {
                        constexpr int16_t SWIPE_THRESHOLD = 5;
                        const int16_t dx = static_cast<int16_t>(touch_end.x) - static_cast<int16_t>(touch_start.x);
                        const int16_t dy = static_cast<int16_t>(touch_end.y) - static_cast<int16_t>(touch_start.y);
                        const int16_t abs_dx = dx < 0 ? -dx : dx;
                        const int16_t abs_dy = dy < 0 ? -dy : dy;
                        const MediaButton action = (abs_dx >= SWIPE_THRESHOLD || abs_dy >= SWIPE_THRESHOLD)
                            ? (abs_dx > abs_dy
                                ? (dx > 0 ? MediaButton::DpadRight : MediaButton::DpadLeft)
                                : (dy > 0 ? MediaButton::DpadDown : MediaButton::DpadUp))
                            : MediaButton::DpadOk;
                        media_dispatch_button(store, config, action);
                    }
                    touching = false;
                    swallow_touch_release = false;
                    continue;
                }

                if ((ui_state->mode == UiMode::WifiDisconnected || ui_state->mode == UiMode::HassDisconnected) &&
                    millis() - last_touch_ms > TOUCH_RELEASE_TIMEOUT_MS) {
                    touching = false;
                    swallow_touch_release = false;
                    continue;
                }

                if (millis() - last_touch_ms > TOUCH_RELEASE_TIMEOUT_MS) {
                    ESP_LOGI(TAG, "End of touch");
                    touching = false;
                    swallow_touch_release = false;
                }
                vTaskDelay(pdMS_TO_TICKS(TOUCH_RELEASE_POLL_MS));
            } else {
                // Idle-phase polling lives in main.cpp's loop now, so this
                // task just blocks on the touch INT ISR notification (with
                // a 2 s safety timeout).
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
            }
        }
    }
}
