#include "managers/touch.h"
#include "managers/home_assistant.h"
#include "managers/wifi.h"
#include "boards.h"
#include "config.h"
#include "constants.h"
#include <Arduino.h>
#include <Wire.h>
#include <cstring>
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

// Read the full 184-byte GT911 config block + checksum and dump it to the
// log. Decodes the well-known fields (CFG_VERSION, X_OUTPUT_MAX,
// Y_OUTPUT_MAX, TOUCH_NUMBER, MODULE_SWITCH_1/2, REFRESH_RATE, screen-touch
// thresholds) and prints the rest as a hex dump. Useful for verifying the
// chip is at expected factory defaults vs. a corrupted stored config.
static void gt911_dump_config(uint8_t addr) {
    constexpr uint16_t GT911_CFG_START_REG    = 0x8047;
    constexpr uint16_t GT911_CFG_CHECKSUM_REG = 0x80FF;
    constexpr size_t   GT911_CFG_LEN          = 184;

    uint8_t cfg[GT911_CFG_LEN] = {0};
    if (!i2c_read_reg16_block(addr, GT911_CFG_START_REG, cfg, sizeof(cfg))) {
        ESP_LOGE(TAG, "gt911_dump_config: failed to read config block");
        return;
    }
    uint8_t stored_checksum = 0;
    if (!i2c_read_reg16(addr, GT911_CFG_CHECKSUM_REG, &stored_checksum, 1)) {
        ESP_LOGE(TAG, "gt911_dump_config: failed to read checksum");
    }
    uint8_t computed = 0;
    for (size_t i = 0; i < sizeof(cfg); i++) {
        computed = static_cast<uint8_t>(computed + cfg[i]);
    }
    computed = static_cast<uint8_t>(~computed + 1);

    // Decode well-known fields. Offsets are relative to 0x8047.
    const uint8_t  cfg_version      = cfg[0x00];
    const uint16_t x_output_max     = static_cast<uint16_t>(cfg[0x01]) | (static_cast<uint16_t>(cfg[0x02]) << 8);
    const uint16_t y_output_max     = static_cast<uint16_t>(cfg[0x03]) | (static_cast<uint16_t>(cfg[0x04]) << 8);
    const uint8_t  touch_number     = cfg[0x05];
    const uint8_t  module_switch_1  = cfg[0x06];
    const uint8_t  module_switch_2  = cfg[0x07];
    const uint8_t  shake_count      = cfg[0x08];
    const uint8_t  filter           = cfg[0x09];
    const uint8_t  large_touch      = cfg[0x0A];
    const uint8_t  noise_reduction  = cfg[0x0B];
    const uint8_t  screen_touch_lvl = cfg[0x0C];
    const uint8_t  screen_leave_lvl = cfg[0x0D];
    const uint8_t  low_power_ctrl   = cfg[0x0E];
    const uint8_t  refresh_rate     = cfg[0x0F];
    const uint8_t  x_threshold      = cfg[0x10];
    const uint8_t  y_threshold      = cfg[0x11];

    ESP_LOGI(TAG, "GT911 config @ 0x%02X (len=%u, stored cksum=0x%02X computed=0x%02X %s)",
             addr, static_cast<unsigned>(sizeof(cfg)),
             stored_checksum, computed,
             (stored_checksum == computed) ? "OK" : "MISMATCH");
    ESP_LOGI(TAG, "  CFG_VERSION       = 0x%02X (%u)", cfg_version, cfg_version);
    ESP_LOGI(TAG, "  X_OUTPUT_MAX      = %u",   x_output_max);
    ESP_LOGI(TAG, "  Y_OUTPUT_MAX      = %u",   y_output_max);
    ESP_LOGI(TAG, "  TOUCH_NUMBER      = %u",   touch_number & 0x0F);
    // MODULE_SWITCH_1 (0x804D) bit layout per Goodix programming guide /
    // common GT9xx driver interpretation:
    //   b1:0 INT_TRIGGER (0=rising, 1=falling, 2=low, 3=high)
    //   b2   SITO
    //   b3   X2Y       — swap X/Y output
    //   b4   Stretch_Rank
    //   b5   reserved
    //   b6   FlipX      — X reverse / mirror
    //   b7   FlipY      — Y reverse / mirror
    // GT911 datasheets formally list b4-b7 as reserved, but firmware variants
    // on these panels commonly populate b6/b7 for axis flip, so report them.
    ESP_LOGI(TAG, "  MODULE_SWITCH_1   = 0x%02X (INT_TRIG=%u SITO=%u X2Y=%u Stretch=%u FlipX=%u FlipY=%u)",
             module_switch_1,
             static_cast<unsigned>(module_switch_1 & 0x03),
             static_cast<unsigned>((module_switch_1 >> 2) & 0x01),
             static_cast<unsigned>((module_switch_1 >> 3) & 0x01),
             static_cast<unsigned>((module_switch_1 >> 4) & 0x01),
             static_cast<unsigned>((module_switch_1 >> 6) & 0x01),
             static_cast<unsigned>((module_switch_1 >> 7) & 0x01));
    ESP_LOGI(TAG, "  MODULE_SWITCH_2   = 0x%02X", module_switch_2);
    ESP_LOGI(TAG, "  SHAKE_COUNT       = 0x%02X", shake_count);
    ESP_LOGI(TAG, "  FILTER            = 0x%02X", filter);
    ESP_LOGI(TAG, "  LARGE_TOUCH       = 0x%02X", large_touch);
    ESP_LOGI(TAG, "  NOISE_REDUCTION   = 0x%02X", noise_reduction);
    ESP_LOGI(TAG, "  SCREEN_TOUCH_LVL  = 0x%02X", screen_touch_lvl);
    ESP_LOGI(TAG, "  SCREEN_LEAVE_LVL  = 0x%02X", screen_leave_lvl);
    ESP_LOGI(TAG, "  LOW_POWER_CTRL    = 0x%02X", low_power_ctrl);
    ESP_LOGI(TAG, "  REFRESH_RATE      = 0x%02X (%u Hz)", refresh_rate, 5 + (refresh_rate & 0x0F));
    ESP_LOGI(TAG, "  X_THRESHOLD       = 0x%02X", x_threshold);
    ESP_LOGI(TAG, "  Y_THRESHOLD       = 0x%02X", y_threshold);

    // Hex dump of the whole block, 16 bytes per line, with the register
    // address at the start of each line.
    char line[64];
    for (size_t row = 0; row < sizeof(cfg); row += 16) {
        size_t pos = 0;
        pos += snprintf(line + pos, sizeof(line) - pos, "  %04X:",
                        static_cast<unsigned>(GT911_CFG_START_REG + row));
        const size_t end = (row + 16 < sizeof(cfg)) ? row + 16 : sizeof(cfg);
        for (size_t i = row; i < end; i++) {
            pos += snprintf(line + pos, sizeof(line) - pos, " %02X", cfg[i]);
        }
        ESP_LOGI(TAG, "%s", line);
    }
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

// Persistently commit MODULE_SWITCH_1 (0x804D) to the GT911's stored config:
// INT_TRIGGER (bits 0-1) := mode_bits, X2Y (bit 3) := 0, FlipX (bit 6) := 0,
// FlipY (bit 7) := 0.
// All other bits in MODULE_SWITCH_1 and the rest of the 184-byte config block
// are preserved.
// Idempotent — if the desired bits already match, returns success without
// touching the chip.
//
// Why persistent rather than runtime: GT911 config registers (0x8047-0x80FE)
// are shadow RAM; the chip only honors writes there when paired with a valid
// checksum at 0x80FF and CFG_FRESH=1 at 0x8100. Without that handshake, the
// I2C writes appear to succeed but the chip keeps running off whatever it
// loaded from non-volatile config at power-on — which is what bit us before.
//
// Safety net (we corrupted the chip the first time we tried this): the
// 184-byte config block is read twice, and we require both copies AND the
// stored checksum to all match before touching anything. CFG_VERSION is also
// bumped, since most GT911 firmwares only persist a config whose version is
// strictly greater than the stored one.
//
// INT mode rationale: LOW_LEVEL_QUERY holds INT low until the host reads the
// data — convenient for polling, but unworkable for light-sleep wake: the
// level-triggered GPIO wake keeps re-firing gpio_intr_service while INT
// stays low, faster than the touch task can wake and drain GT911, tripping
// IWDT. FALLING_EDGE pulses INT briefly on each event then releases it high
// — the pulse triggers the level-low wake but level returns high before
// gpio_intr_service can spin.
static bool configure_gt911_int_mode(uint8_t addr, uint8_t mode_bits) {
    constexpr uint16_t GT911_CFG_START_REG       = 0x8047;
    constexpr uint16_t GT911_X_OUTPUT_MAX_REG    = 0x8048; // 2 bytes, little-endian
    constexpr uint16_t GT911_Y_OUTPUT_MAX_REG    = 0x804A; // 2 bytes, little-endian
    constexpr uint16_t GT911_MODULE_SWITCH_1_REG = 0x804D;
    constexpr uint16_t GT911_CFG_CHECKSUM_REG    = 0x80FF;
    constexpr uint16_t GT911_CFG_FRESH_REG       = 0x8100;
    constexpr size_t   GT911_CFG_LEN             = 184;
    constexpr uint8_t  GT911_INT_MASK            = 0x03; // bits 0-1
    constexpr uint8_t  GT911_X2Y_BIT             = 0x08; // bit 3
    constexpr uint8_t  GT911_FLIPX_BIT           = 0x40; // bit 6
    constexpr uint8_t  GT911_FLIPY_BIT           = 0x80; // bit 7
    // Chip's stored factory defaults for this LilyGO panel — confirmed via
    // gt911_dump_config(). We don't modify these; the values are here so
    // the fast-path can detect a chip that's been reset/replaced.
    constexpr uint16_t kExpectedXMax             = 540;
    constexpr uint16_t kExpectedYMax             = 960;

    // Fast-path: read just the bits we care about (MODULE_SWITCH_1 + the
    // X/Y_OUTPUT_MAX values). If everything is already in the expected
    // state, skip the 184-byte read + checksum-recompute + commit. On
    // hardware that doesn't persist commits to flash this never matches
    // and we fall through; on hardware that does, it short-circuits the
    // commit on every boot after the first.
    {
        uint8_t  current_module_switch_1 = 0;
        uint8_t  xy_buf[4]               = {0};
        if (i2c_read_reg16(addr, GT911_MODULE_SWITCH_1_REG, &current_module_switch_1, 1) &&
            i2c_read_reg16(addr, GT911_X_OUTPUT_MAX_REG,    xy_buf,                   4)) {
            const uint16_t current_x = static_cast<uint16_t>(xy_buf[0]) | (static_cast<uint16_t>(xy_buf[1]) << 8);
            const uint16_t current_y = static_cast<uint16_t>(xy_buf[2]) | (static_cast<uint16_t>(xy_buf[3]) << 8);
            constexpr uint8_t kCheckMask = GT911_INT_MASK | GT911_X2Y_BIT | GT911_FLIPX_BIT | GT911_FLIPY_BIT;
            const uint8_t expected_controlled = static_cast<uint8_t>(mode_bits & GT911_INT_MASK);
            if ((current_module_switch_1 & kCheckMask) == expected_controlled &&
                current_x == kExpectedXMax &&
                current_y == kExpectedYMax) {
                ESP_LOGI(TAG,
                         "configure_gt911_int_mode: already configured (MODULE_SWITCH_1=0x%02X X_MAX=%u Y_MAX=%u) — skipping commit",
                         current_module_switch_1, current_x, current_y);
                return true;
            }
        }
    }

    // Two independent reads of the entire config block. If they don't agree,
    // the I2C bus glitched somewhere — bail rather than commit garbage.
    uint8_t cfg_a[GT911_CFG_LEN] = {0};
    uint8_t cfg_b[GT911_CFG_LEN] = {0};
    if (!i2c_read_reg16_block(addr, GT911_CFG_START_REG, cfg_a, sizeof(cfg_a))) {
        ESP_LOGE(TAG, "configure_gt911_int_mode: failed to read config block (1st)");
        return false;
    }
    if (!i2c_read_reg16_block(addr, GT911_CFG_START_REG, cfg_b, sizeof(cfg_b))) {
        ESP_LOGE(TAG, "configure_gt911_int_mode: failed to read config block (2nd)");
        return false;
    }
    if (memcmp(cfg_a, cfg_b, sizeof(cfg_a)) != 0) {
        ESP_LOGE(TAG, "configure_gt911_int_mode: config-block reads differ — refusing to commit");
        return false;
    }

    uint8_t stored_checksum = 0;
    if (!i2c_read_reg16(addr, GT911_CFG_CHECKSUM_REG, &stored_checksum, 1)) {
        ESP_LOGE(TAG, "configure_gt911_int_mode: failed to read stored checksum");
        return false;
    }
    uint8_t computed = 0;
    for (size_t i = 0; i < sizeof(cfg_a); i++) {
        computed = static_cast<uint8_t>(computed + cfg_a[i]);
    }
    computed = static_cast<uint8_t>(~computed + 1);
    if (computed != stored_checksum) {
        ESP_LOGE(TAG, "configure_gt911_int_mode: checksum mismatch (stored=0x%02X computed=0x%02X) — refusing to write",
                 stored_checksum, computed);
        return false;
    }

    constexpr size_t kModuleSwitch1Offset = GT911_MODULE_SWITCH_1_REG - GT911_CFG_START_REG;
    const uint8_t old_module_switch_1 = cfg_a[kModuleSwitch1Offset];
    const uint8_t new_module_switch_1 = static_cast<uint8_t>(
        (old_module_switch_1 & ~(GT911_INT_MASK | GT911_X2Y_BIT | GT911_FLIPX_BIT | GT911_FLIPY_BIT)) |
        (mode_bits & GT911_INT_MASK));
    if (old_module_switch_1 == new_module_switch_1) {
        ESP_LOGI(TAG, "configure_gt911_int_mode: MODULE_SWITCH_1 already 0x%02X, nothing to commit",
                 old_module_switch_1);
        return true;
    }
    cfg_a[kModuleSwitch1Offset] = new_module_switch_1;

    // Bump CFG_VERSION so the chip accepts the new config. Many GT911
    // firmwares silently reject same/lower versions: the runtime registers
    // get touched, but RST reloads the previous stored config.
    const uint8_t old_version = cfg_a[0];
    cfg_a[0] = static_cast<uint8_t>(old_version + 1);

    uint8_t new_checksum = 0;
    for (size_t i = 0; i < sizeof(cfg_a); i++) {
        new_checksum = static_cast<uint8_t>(new_checksum + cfg_a[i]);
    }
    new_checksum = static_cast<uint8_t>(~new_checksum + 1);

    ESP_LOGI(TAG, "configure_gt911_int_mode: MODULE_SWITCH_1 0x%02X -> 0x%02X, version 0x%02X -> 0x%02X, checksum 0x%02X -> 0x%02X",
             old_module_switch_1, new_module_switch_1,
             old_version, cfg_a[0],
             stored_checksum, new_checksum);

    // Write the whole config block in chunks small enough for the I2C tx
    // buffer (Arduino default is 32 bytes incl. address). Writing the
    // entire block (rather than just MODULE_SWITCH_1) ensures the runtime
    // config exactly matches the bytes we computed the checksum for.
    constexpr size_t kChunk = 16;
    for (size_t offset = 0; offset < sizeof(cfg_a); offset += kChunk) {
        const size_t len = (sizeof(cfg_a) - offset < kChunk) ? (sizeof(cfg_a) - offset) : kChunk;
        const uint16_t reg = static_cast<uint16_t>(GT911_CFG_START_REG + offset);
        Wire.beginTransmission(addr);
        Wire.write(static_cast<uint8_t>(reg >> 8));
        Wire.write(static_cast<uint8_t>(reg & 0xFF));
        for (size_t i = 0; i < len; i++) {
            Wire.write(cfg_a[offset + i]);
        }
        if (Wire.endTransmission() != 0) {
            ESP_LOGE(TAG, "configure_gt911_int_mode: config-block write failed at offset %u", static_cast<unsigned>(offset));
            return false;
        }
    }
    if (!i2c_write_reg16(addr, GT911_CFG_CHECKSUM_REG, new_checksum)) {
        ESP_LOGE(TAG, "configure_gt911_int_mode: failed to write checksum");
        return false;
    }
    if (!i2c_write_reg16(addr, GT911_CFG_FRESH_REG, 0x01)) {
        ESP_LOGE(TAG, "configure_gt911_int_mode: failed to write CFG_FRESH");
        return false;
    }
    // CFG_FRESH=1 triggers the chip to re-validate the checksum and apply
    // the new config. We deliberately do NOT pulse RST — RST reloads from
    // internal storage, which would clobber the runtime change since this
    // hardware doesn't actually persist the commit to flash.
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
    // Latched at the !touching → touching transition so the touchpad
    // tap-on-hold path can measure how long the contact has been held.
    uint32_t touch_start_ms = 0;
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

#if defined(GT911_DUMP_CONFIG_ON_BOOT)
    if (gt911_addr_present) {
        gt911_dump_config(gt911_addr);
    } else {
        ESP_LOGW(TAG, "GT911_DUMP_CONFIG_ON_BOOT: GT911 not detected, nothing to dump");
    }
#endif

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
            if (!touching) {
                touch_start_ms = now_ms;
            }
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
                    // path skips its own swipe/tap dispatch. If no swipe occurs
                    // within TOUCHPAD_TAP_HOLD_MS, treat the press as a tap and
                    // fire DpadOk immediately rather than waiting for finger lift.
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
                        } else if (static_cast<uint32_t>(now_ms - touch_start_ms) >= TOUCHPAD_TAP_HOLD_MS) {
                            media_dispatch_button(store, config, MediaButton::DpadOk);
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
