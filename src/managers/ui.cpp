#include "ui.h"
#include "assets/Roboto_Condensed_Regular_10.h"
#include "assets/Roboto_Condensed_Regular_16.h"
#include "assets/Roboto_Condensed_Regular_20.h"
#include "assets/Montserrat_Regular_26.h"
#include "assets/icons.h"
#include "boards.h"
#include "constants.h"
#include "draw.h"
#include "managers/battery.h"
#include "pm_lock.h"
#include "store.h"
#include <cstdio>
#include <cstring>

static const char* TAG = "ui";
static const char* const TEXT_BOOT[] = {"Home Assistant", "e-paper remote", nullptr};
static const char* const TEXT_WIFI_DISCONNECTED[] = {"Not connected", "to Wi-Fi", nullptr};
static const char* const TEXT_HASS_DISCONNECTED[] = {"Not connected", "to Home Assistant", nullptr};
static const char* const TEXT_HASS_INVALID_KEY[] = {"Cannot connect", "to Home Assistant:", "invalid token", nullptr};
static const char* const TEXT_GENERIC_ERROR[] = {"Unknown error", nullptr};

static BB_RECT get_text_box(FASTEPD* epaper, const char* text) {
    BB_RECT rect = {};
    epaper->getStringBox(text, &rect);
    return rect;
}

static void draw_text_at(FASTEPD* epaper, int16_t x, int16_t y, const char* text, bool reinforce = false) {
    epaper->setCursor(x, y);
    epaper->write(text);
    if (reinforce) {
        epaper->setCursor(x + 1, y);
        epaper->write(text);
    }
}

static void ui_draw_connection_recovery_buttons(FASTEPD* epaper) {
    BB_RECT rect;
    epaper->setFont(Roboto_Condensed_Regular_20);
    // Montserrat sets the cursor at the baseline, so y = top + (h+rect.h)/2 - 4
    // (matches ui_draw_media_button).

    // Retry button (filled)
    epaper->fillRoundRect(WIFI_DISC_RETRY_X, WIFI_DISC_BUTTON_Y, WIFI_DISC_RETRY_W, WIFI_DISC_BUTTON_H, 12, BBEP_BLACK);
    epaper->setTextColor(BBEP_WHITE);
    const char* retry_label = "Retry";
    epaper->getStringBox(retry_label, &rect);
    draw_text_at(epaper, WIFI_DISC_RETRY_X + (WIFI_DISC_RETRY_W - rect.w) / 2,
                 WIFI_DISC_BUTTON_Y + (WIFI_DISC_BUTTON_H + rect.h) / 2 - 4, retry_label);

    // Wi-Fi Settings button (outlined)
    epaper->fillRoundRect(WIFI_DISC_SETTINGS_X, WIFI_DISC_BUTTON_Y, WIFI_DISC_SETTINGS_W, WIFI_DISC_BUTTON_H, 12, BBEP_WHITE);
    epaper->drawRoundRect(WIFI_DISC_SETTINGS_X, WIFI_DISC_BUTTON_Y, WIFI_DISC_SETTINGS_W, WIFI_DISC_BUTTON_H, 12, BBEP_BLACK);
    epaper->setTextColor(BBEP_BLACK);
    const char* settings_label = "Wi-Fi";
    epaper->getStringBox(settings_label, &rect);
    draw_text_at(epaper, WIFI_DISC_SETTINGS_X + (WIFI_DISC_SETTINGS_W - rect.w) / 2,
                 WIFI_DISC_BUTTON_Y + (WIFI_DISC_BUTTON_H + rect.h) / 2 - 4, settings_label);
}

static void ui_draw_wifi_disconnected(FASTEPD* epaper) {
    drawCenteredIconWithText(epaper, wifi_off, TEXT_WIFI_DISCONNECTED, 30, 100);
    ui_draw_connection_recovery_buttons(epaper);
}

static void ui_draw_hass_disconnected(FASTEPD* epaper) {
    drawCenteredIconWithText(epaper, server_network_off, TEXT_HASS_DISCONNECTED, 30, 100);
    ui_draw_connection_recovery_buttons(epaper);
}

void ui_show_message(UiMode mode, FASTEPD* epaper) {
    const uint8_t* icon = alert_circle;
    const char* const* text_lines = TEXT_GENERIC_ERROR;

    switch (mode) {
    case UiMode::Boot:
        icon = home_assistant;
        text_lines = TEXT_BOOT;
        break;
    case UiMode::WifiDisconnected:
        ui_draw_wifi_disconnected(epaper);
        return;
    case UiMode::HassDisconnected:
        ui_draw_hass_disconnected(epaper);
        return;
    case UiMode::HassInvalidKey:
        icon = lock_alert_outline;
        text_lines = TEXT_HASS_INVALID_KEY;
        break;
    default:
        break;
    }

    drawCenteredIconWithText(epaper, icon, text_lines, 30, 100);
}

static void truncate_with_ellipsis(FASTEPD* epaper, char* line, size_t line_len, int16_t max_w) {
    if (line[0] == '\0') {
        return;
    }

    BB_RECT rect = get_text_box(epaper, line);
    if (rect.w <= max_w) {
        return;
    }

    if (line_len < 4) {
        line[0] = '\0';
        return;
    }

    char candidate[64];
    size_t current_len = strlen(line);
    while (current_len > 0) {
        current_len--;
        size_t keep = current_len;
        if (keep > sizeof(candidate) - 4) {
            keep = sizeof(candidate) - 4;
        }

        memcpy(candidate, line, keep);
        candidate[keep] = '.';
        candidate[keep + 1] = '.';
        candidate[keep + 2] = '.';
        candidate[keep + 3] = '\0';

        rect = get_text_box(epaper, candidate);
        if (rect.w <= max_w) {
            strncpy(line, candidate, line_len - 1);
            line[line_len - 1] = '\0';
            return;
        }
    }

    strncpy(line, "...", line_len - 1);
    line[line_len - 1] = '\0';
}

static void ui_draw_back_icon(FASTEPD* epaper) {
    const int16_t center_x = ROOM_CONTROLS_BACK_X + ROOM_CONTROLS_BACK_W / 2;
    const int16_t center_y = ROOM_CONTROLS_BACK_Y + ROOM_CONTROLS_BACK_H / 2;
    const int16_t tip_x = center_x - 20;
    const int16_t shaft_end_x = center_x + 14;
    const int16_t wing_dx = 12;
    const int16_t wing_dy = 12;

    for (int8_t t = -1; t <= 1; t++) {
        epaper->drawLine(tip_x, center_y + t, tip_x + wing_dx, center_y - wing_dy + t, BBEP_BLACK);
        epaper->drawLine(tip_x, center_y + t, tip_x + wing_dx, center_y + wing_dy + t, BBEP_BLACK);
        epaper->drawLine(tip_x, center_y + t, shaft_end_x, center_y + t, BBEP_BLACK);
    }
}

// Bin RSSI into 0-4 bars. -127 (or disconnected) maps to 0.
static uint8_t wifi_bars_from_rssi(int16_t rssi, bool connected) {
    if (!connected || rssi <= -100) return 0;
    if (rssi >= -55) return 4;
    if (rssi >= -65) return 3;
    if (rssi >= -75) return 2;
    if (rssi >= -85) return 1;
    return 0;
}

// 4-bar Wi-Fi indicator centered in the given rect. Filled bar count tracks
// signal strength; empty bars are drawn as outlines so the icon shape is
// always recognizable. A small slash overlays the icon when disconnected.
static void ui_draw_wifi_indicator(FASTEPD* epaper, int16_t cx, int16_t cy, int16_t rssi, bool connected) {
    constexpr int16_t bar_w = 6;
    constexpr int16_t bar_gap = 3;
    constexpr int16_t heights[4] = {8, 14, 20, 26};
    constexpr int16_t total_w = 4 * bar_w + 3 * bar_gap;
    const int16_t baseline_y = cy + heights[3] / 2;
    const int16_t x0 = cx - total_w / 2;
    const uint8_t filled = wifi_bars_from_rssi(rssi, connected);

    for (uint8_t i = 0; i < 4; i++) {
        const int16_t x = x0 + i * (bar_w + bar_gap);
        const int16_t y = baseline_y - heights[i];
        if (i < filled) {
            epaper->fillRect(x, y, bar_w, heights[i], BBEP_BLACK);
        } else {
            epaper->drawRect(x, y, bar_w, heights[i], BBEP_BLACK);
        }
    }

    if (!connected) {
        // Diagonal slash from top-right to bottom-left (3 px thick).
        const int16_t x1 = x0 + total_w;
        const int16_t y1 = baseline_y - heights[3];
        const int16_t x2 = x0;
        const int16_t y2 = baseline_y;
        for (int8_t t = -1; t <= 1; t++) {
            epaper->drawLine(x1 + t, y1, x2 + t, y2, BBEP_BLACK);
        }
    }
}

static void ui_draw_back_button(FASTEPD* epaper) {
    epaper->fillRoundRect(ROOM_CONTROLS_BACK_X, ROOM_CONTROLS_BACK_Y, ROOM_CONTROLS_BACK_W, ROOM_CONTROLS_BACK_H, 14, BBEP_WHITE);
    epaper->drawRoundRect(ROOM_CONTROLS_BACK_X, ROOM_CONTROLS_BACK_Y, ROOM_CONTROLS_BACK_W, ROOM_CONTROLS_BACK_H, 14, BBEP_BLACK);
    ui_draw_back_icon(epaper);
}

static void ui_draw_battery_indicator(FASTEPD* epaper, int16_t right_edge_x, int16_t center_y, uint8_t soc_pct) {
    constexpr int16_t body_w = 16;
    constexpr int16_t body_h = 30;
    constexpr int16_t nub_w = 8;
    constexpr int16_t nub_h = 3;
    constexpr int16_t text_gap = 3;

    char pct_text[8];
    snprintf(pct_text, sizeof(pct_text), "%u", static_cast<unsigned>(soc_pct));

    epaper->setFont(Roboto_Condensed_Regular_10);
    const BB_RECT text_rect = get_text_box(epaper, pct_text);

    // Stack vertically: nub on top, body, text below; right edge of the icon
    // hugs right_edge_x with the text horizontally centered under the body.
    const int16_t body_x = right_edge_x - body_w;
    const int16_t total_h = nub_h + body_h + text_gap + text_rect.h;
    const int16_t body_y = center_y - total_h / 2 + nub_h;

    epaper->fillRect(body_x + (body_w - nub_w) / 2, body_y - nub_h, nub_w, nub_h, BBEP_BLACK);
    epaper->drawRoundRect(body_x, body_y, body_w, body_h, 2, BBEP_BLACK);

    const int16_t fill_max = body_h - 4;
    const int16_t fill_h = static_cast<int16_t>((static_cast<int32_t>(fill_max) * soc_pct) / 100);
    if (fill_h > 0) {
        epaper->fillRect(body_x + 2, body_y + body_h - 2 - fill_h, body_w - 4, fill_h, BBEP_BLACK);
    }

    // Montserrat sets the cursor at the baseline, so add text_rect.h to land
    // the top of the glyphs at body_y + body_h + text_gap.
    const int16_t text_x = body_x + (body_w - text_rect.w) / 2;
    const int16_t text_y = body_y + body_h + text_gap + text_rect.h;
    draw_text_at(epaper, text_x, text_y, pct_text, true);
}

static void ui_draw_media_button(FASTEPD* epaper, int16_t x, int16_t y, int16_t w, int16_t h, const char* label) {
    epaper->fillRoundRect(x, y, w, h, MEDIA_BUTTON_RADIUS, BBEP_WHITE);
    epaper->drawRoundRect(x, y, w, h, MEDIA_BUTTON_RADIUS, BBEP_BLACK);
    if (label && label[0] != '\0') {
        epaper->setFont(Roboto_Condensed_Regular_20);
        const BB_RECT rect = get_text_box(epaper, label);
        const int16_t tx = x + (w - rect.w) / 2;
        const int16_t ty = y + (h + rect.h) / 2 - 4;
        draw_text_at(epaper, tx, ty, label, true);
    }
}

static void ui_draw_media_icon_button(FASTEPD* epaper, int16_t x, int16_t y, int16_t w, int16_t h, const uint8_t* icon) {
    epaper->fillRoundRect(x, y, w, h, MEDIA_BUTTON_RADIUS, BBEP_WHITE);
    epaper->drawRoundRect(x, y, w, h, MEDIA_BUTTON_RADIUS, BBEP_BLACK);
    if (icon) {
        // Icons-buttons assets are 64x64 BMPs.
        const int16_t cx = x + w / 2;
        const int16_t cy = y + h / 2;
        epaper->loadBMP(icon, cx - 32, cy - 32, BBEP_WHITE, BBEP_BLACK);
    }
}

// Reserve a fixed-width slot for the battery indicator so the title bounds
// stay stable as the SoC text width changes ("5" vs "100"). The battery
// hugs the Wi-Fi button on the left; a wider gap separates it from the
// title on the right so the cluster reads as one group.
constexpr int16_t HEADER_BATTERY_SLOT_W = 24;
constexpr int16_t HEADER_BATTERY_GAP_LEFT = 0;
constexpr int16_t HEADER_BATTERY_GAP_RIGHT = 16;

static void ui_draw_media_header(FASTEPD* epaper, const char* title, bool battery_valid, uint8_t battery_soc_pct,
                                 int16_t wifi_rssi, bool wifi_connected) {
    // Wi-Fi indicator (top-left), framed like a button — tapping it opens
    // Wi-Fi settings.
    epaper->fillRoundRect(HOME_LEFT_BUTTON_X, HOME_LEFT_BUTTON_Y, HOME_LEFT_BUTTON_W, HOME_LEFT_BUTTON_H, 14, BBEP_WHITE);
    epaper->drawRoundRect(HOME_LEFT_BUTTON_X, HOME_LEFT_BUTTON_Y, HOME_LEFT_BUTTON_W, HOME_LEFT_BUTTON_H, 14, BBEP_BLACK);
    ui_draw_wifi_indicator(epaper, HOME_LEFT_BUTTON_X + HOME_LEFT_BUTTON_W / 2,
                           HOME_LEFT_BUTTON_Y + HOME_LEFT_BUTTON_H / 2, wifi_rssi, wifi_connected);

    // Battery indicator immediately right of the Wi-Fi button.
    const int16_t batt_slot_left = HOME_LEFT_BUTTON_X + HOME_LEFT_BUTTON_W + HEADER_BATTERY_GAP_LEFT;
    const int16_t batt_right = batt_slot_left + HEADER_BATTERY_SLOT_W;
    if (battery_valid) {
        const int16_t batt_y = HOME_LEFT_BUTTON_Y + HOME_LEFT_BUTTON_H / 2;
        ui_draw_battery_indicator(epaper, batt_right, batt_y, battery_soc_pct);
    }

    // Title centered in the remaining space between the battery slot and the
    // Power button.
    epaper->setFont(Montserrat_Regular_26);
    const BB_RECT title_rect = get_text_box(epaper, title);
    const int16_t title_y = HOME_LEFT_BUTTON_Y + (HOME_LEFT_BUTTON_H + title_rect.h) / 2 - 4;
    const int16_t left_bound = batt_right + HEADER_BATTERY_GAP_RIGHT;
    const int16_t right_bound = HOME_RIGHT_BUTTON_X - HEADER_BATTERY_GAP_RIGHT;
    const int16_t title_x = (left_bound + right_bound) / 2 - title_rect.w / 2;
    draw_text_at(epaper, title_x, title_y, title);

    // Power button (top-right).
    ui_draw_media_icon_button(epaper, HOME_RIGHT_BUTTON_X, HOME_RIGHT_BUTTON_Y, HOME_RIGHT_BUTTON_W, HOME_RIGHT_BUTTON_H, power);
}

void ui_draw_media_controller(FASTEPD* epaper, const Configuration* config, uint8_t device_idx, bool battery_valid, uint8_t battery_soc_pct,
                              int16_t wifi_rssi, bool wifi_connected) {
    const MediaDevice* device = nullptr;
    if (config && config->media_device_count > 0) {
        const uint8_t idx = device_idx < config->media_device_count ? device_idx : 0;
        device = &config->media_devices[idx];
    }
    const char* title = device && device->title ? device->title : "Media";
    epaper->setTextColor(BBEP_BLACK);
    ui_draw_media_header(epaper, title, battery_valid, battery_soc_pct, wifi_rssi, wifi_connected);

    // Top section: 3x3 grid. Right column = volume (Vol+/Vol-/Mute);
    // left+middle = 6 source buttons in reading order (fills all six cells).
    {
        const int16_t inner_w = DISPLAY_WIDTH - 2 * MEDIA_MARGIN_X;
        const int16_t btn_w = (inner_w - 2 * MEDIA_BUTTON_GAP) / 3;
        const int16_t col_x[3] = {
            MEDIA_MARGIN_X,
            MEDIA_MARGIN_X + (btn_w + MEDIA_BUTTON_GAP),
            MEDIA_MARGIN_X + 2 * (btn_w + MEDIA_BUTTON_GAP),
        };
        const int16_t row_y[3] = {MEDIA_VOLUME_ROW_Y, MEDIA_VOLUME_ROW2_Y, MEDIA_VOLUME_ROW3_Y};

        // Right column = volume controls.
        ui_draw_media_icon_button(epaper, col_x[2], row_y[0], btn_w, MEDIA_BUTTON_H, volume_plus);
        ui_draw_media_icon_button(epaper, col_x[2], row_y[1], btn_w, MEDIA_BUTTON_H, volume_minus);
        ui_draw_media_icon_button(epaper, col_x[2], row_y[2], btn_w, MEDIA_BUTTON_H, volume_mute);

        // Sources fill the left+middle columns top-to-bottom, left-to-right.
        const int16_t source_cells[MEDIA_SOURCE_COUNT][2] = {
            {col_x[0], row_y[0]},
            {col_x[1], row_y[0]},
            {col_x[0], row_y[1]},
            {col_x[1], row_y[1]},
            {col_x[0], row_y[2]},
            {col_x[1], row_y[2]},
        };
        for (size_t i = 0; i < MEDIA_SOURCE_COUNT; i++) {
            const MediaSource* src = device ? &device->sources[i] : nullptr;
            if (src) {
                if (src->icon) {
                    ui_draw_media_icon_button(epaper, source_cells[i][0], source_cells[i][1], btn_w, MEDIA_BUTTON_H, src->icon);
                } else {
                    ui_draw_media_button(epaper, source_cells[i][0], source_cells[i][1], btn_w, MEDIA_BUTTON_H, src->label);
                }
            }
        }
    }

    // Nav row: [Back] [Home] [Info]
    {
        const int16_t inner_w = DISPLAY_WIDTH - 2 * MEDIA_MARGIN_X;
        const int16_t btn_w = (inner_w - 2 * MEDIA_BUTTON_GAP) / 3;
        const int16_t x_back = MEDIA_MARGIN_X;
        const int16_t x_home = MEDIA_MARGIN_X + btn_w + MEDIA_BUTTON_GAP;
        const int16_t x_info = MEDIA_MARGIN_X + 2 * (btn_w + MEDIA_BUTTON_GAP);
        ui_draw_media_icon_button(epaper, x_back, MEDIA_NAV_ROW_Y, btn_w, MEDIA_BUTTON_H, arrow_left);
        ui_draw_media_icon_button(epaper, x_home, MEDIA_NAV_ROW_Y, btn_w, MEDIA_BUTTON_H, home);
        ui_draw_media_icon_button(epaper, x_info, MEDIA_NAV_ROW_Y, btn_w, MEDIA_BUTTON_H, asterisk_circle_outline);
    }

    // Touchpad: width matches the buttons above and below. Swipe = D-pad.
    {
        const int16_t pad_w = DISPLAY_WIDTH - 2 * MEDIA_MARGIN_X;
        epaper->fillRoundRect(MEDIA_MARGIN_X, MEDIA_DPAD_Y, pad_w, MEDIA_DPAD_SIZE, MEDIA_BUTTON_RADIUS * 2, BBEP_WHITE);
        epaper->drawRoundRect(MEDIA_MARGIN_X, MEDIA_DPAD_Y, pad_w, MEDIA_DPAD_SIZE, MEDIA_BUTTON_RADIUS * 2, BBEP_BLACK);
    }

    // Transport row: [Rev] [Replay] [Play] [Fwd]
    {
        const int16_t inner_w = DISPLAY_WIDTH - 2 * MEDIA_MARGIN_X;
        const int16_t btn_w = (inner_w - 3 * MEDIA_BUTTON_GAP) / 4;
        const int16_t x0 = MEDIA_MARGIN_X;
        const int16_t x1 = x0 + (btn_w + MEDIA_BUTTON_GAP);
        const int16_t x2 = x0 + 2 * (btn_w + MEDIA_BUTTON_GAP);
        const int16_t x3 = x0 + 3 * (btn_w + MEDIA_BUTTON_GAP);
        ui_draw_media_icon_button(epaper, x0, MEDIA_TRANSPORT_ROW_Y, btn_w, MEDIA_BUTTON_H, rewind_button);
        ui_draw_media_icon_button(epaper, x1, MEDIA_TRANSPORT_ROW_Y, btn_w, MEDIA_BUTTON_H, replay);
        ui_draw_media_icon_button(epaper, x2, MEDIA_TRANSPORT_ROW_Y, btn_w, MEDIA_BUTTON_H, play);
        ui_draw_media_icon_button(epaper, x3, MEDIA_TRANSPORT_ROW_Y, btn_w, MEDIA_BUTTON_H, fast_forward);
    }
}

static void ui_draw_settings_header(FASTEPD* epaper, const char* title);

void ui_draw_media_device_select(FASTEPD* epaper, const Configuration* config, uint8_t active_idx) {
    epaper->setTextColor(BBEP_BLACK);
    ui_draw_settings_header(epaper, "Select device");

    if (!config || config->media_device_count == 0) {
        epaper->setFont(Roboto_Condensed_Regular_20);
        draw_text_at(epaper, MEDIA_MARGIN_X, SETTINGS_HEADER_HEIGHT + 60, "No devices configured");
        return;
    }

    constexpr int16_t row_h = 96;
    constexpr int16_t row_gap = 18;
    const int16_t row_w = DISPLAY_WIDTH - 2 * MEDIA_MARGIN_X;
    const int16_t y0 = SETTINGS_HEADER_HEIGHT + 32;

    for (size_t i = 0; i < config->media_device_count; i++) {
        const int16_t y = y0 + static_cast<int16_t>(i) * (row_h + row_gap);
        const bool active = static_cast<uint8_t>(i) == active_idx;
        const uint8_t bg = active ? BBEP_BLACK : BBEP_WHITE;
        const uint8_t fg = active ? BBEP_WHITE : BBEP_BLACK;

        epaper->fillRoundRect(MEDIA_MARGIN_X, y, row_w, row_h, MEDIA_BUTTON_RADIUS, bg);
        epaper->drawRoundRect(MEDIA_MARGIN_X, y, row_w, row_h, MEDIA_BUTTON_RADIUS, BBEP_BLACK);

        const char* title = config->media_devices[i].title ? config->media_devices[i].title : "(unnamed)";
        epaper->setFont(Montserrat_Regular_26);
        epaper->setTextColor(fg);
        const BB_RECT rect = get_text_box(epaper, title);
        const int16_t tx = MEDIA_MARGIN_X + 28;
        const int16_t ty = y + (row_h + rect.h) / 2 - 4;
        draw_text_at(epaper, tx, ty, title);
    }
    epaper->setTextColor(BBEP_BLACK);
}

static const char* ui_charge_state_label(uint8_t chrg_stat) {
    switch (chrg_stat) {
    case 0: return "Not charging";
    case 1: return "Pre-charging";
    case 2: return "Fast charging";
    case 3: return "Charge done";
    default: return "Unknown";
    }
}

void ui_draw_battery_status(FASTEPD* epaper, const ChargerSnapshot* charger, const FuelGaugeDetail* detail,
                            bool fuel_valid, uint8_t soc_pct, uint16_t voltage_mv) {
    epaper->setTextColor(BBEP_BLACK);
    ui_draw_settings_header(epaper, "Battery");

    const int16_t inner_w = DISPLAY_WIDTH - 2 * MEDIA_MARGIN_X;
    const int16_t card_y = SETTINGS_HEADER_HEIGHT + 24;
    const int16_t card_h = 572;
    epaper->fillRoundRect(MEDIA_MARGIN_X, card_y, inner_w, card_h, 14, BBEP_WHITE);
    epaper->drawRoundRect(MEDIA_MARGIN_X, card_y, inner_w, card_h, 14, BBEP_BLACK);

    epaper->setFont(Roboto_Condensed_Regular_20);
    constexpr int16_t row_h = 56;
    const int16_t label_x = MEDIA_MARGIN_X + 24;
    const int16_t value_x = MEDIA_MARGIN_X + 240;
    int16_t y = card_y + 50;

    auto draw_row = [&](const char* label, const char* value) {
        draw_text_at(epaper, label_x, y, label);
        draw_text_at(epaper, value_x, y, value);
        y += row_h;
    };

    char buf[64];
    const bool charger_ok = charger && charger->valid;

    // State
    if (fuel_valid) {
        snprintf(buf, sizeof(buf), "%u%%", static_cast<unsigned>(soc_pct));
        draw_row("State", buf);
    } else {
        draw_row("State", "n/a");
    }

    // Capacity
    if (detail && detail->valid && detail->full_charge_capacity_mah > 0) {
        snprintf(buf, sizeof(buf), "%u mAh", detail->full_charge_capacity_mah);
        draw_row("Capacity", buf);
    }

    // Time left (only when discharging and the gauge has a valid estimate).
    if (detail && detail->valid && detail->avg_current_ma < 0 && detail->time_to_empty_min != 0xFFFF) {
        const uint16_t hours = detail->time_to_empty_min / 60;
        const uint16_t mins = detail->time_to_empty_min % 60;
        if (hours > 0) {
            snprintf(buf, sizeof(buf), "%uh %02um", hours, mins);
        } else {
            snprintf(buf, sizeof(buf), "%u min", mins);
        }
        draw_row("Time left", buf);
    }

    if (charger_ok) {
        // Charging
        draw_row("Charging", ui_charge_state_label(charger->chrg_stat));
        // Input
        if (charger->vbus_present && charger->vbus_mv > 0) {
            snprintf(buf, sizeof(buf), "%u.%03u V", charger->vbus_mv / 1000, charger->vbus_mv % 1000);
            draw_row("Input", buf);
        } else {
            draw_row("Input", "Disconnected");
        }
        // Battery
        snprintf(buf, sizeof(buf), "%u.%03u V", charger->batv_mv / 1000, charger->batv_mv % 1000);
        draw_row("Battery", buf);
        // System
        snprintf(buf, sizeof(buf), "%u.%03u V", charger->sysv_mv / 1000, charger->sysv_mv % 1000);
        draw_row("System", buf);
    }

    // Gauge
    if (fuel_valid) {
        snprintf(buf, sizeof(buf), "%u.%03u V", voltage_mv / 1000, voltage_mv % 1000);
        draw_row("Gauge", buf);
    }

    // Current (charger ICHG, only while charging)
    if (charger_ok && charger->ichg_ma > 0) {
        snprintf(buf, sizeof(buf), "%u mA", charger->ichg_ma);
        draw_row("Current", buf);
    }

    // Load (discharge-only)
    if (detail && detail->valid && detail->avg_current_ma < 0) {
        snprintf(buf, sizeof(buf), "%d mA", -static_cast<int>(detail->avg_current_ma));
        draw_row("Load", buf);
    }

    // Temperature
    if (detail && detail->valid && detail->temperature_dc != INT16_MIN) {
        const int16_t dc = detail->temperature_dc;
        const int whole = dc / 10;
        const int tenth = (dc < 0 ? -dc : dc) % 10;
        snprintf(buf, sizeof(buf), "%d.%d C", whole, tenth);
        draw_row("Temp", buf);
    }

    if (!charger_ok) {
        epaper->setFont(Roboto_Condensed_Regular_16);
        draw_text_at(epaper, label_x, y, "Charger IC not detected.");
    }
}

void ui_draw_standby_screen(FASTEPD* epaper) {
    epaper->setMode(BB_MODE_1BPP);
    epaper->setTextColor(BBEP_BLACK);
    epaper->fillScreen(BBEP_WHITE);

    epaper->setFont(Montserrat_Regular_26);
    const char* msg = "< Wake";
    const BB_RECT rect = get_text_box(epaper, msg);
    constexpr int16_t margin_left = 32;
    const int16_t text_x = margin_left;
    // 38% of the screen height up from the bottom, near the BOOT button on
    // the right edge — the front circle key cannot wake from deep sleep.
    const int16_t text_y = static_cast<int16_t>(DISPLAY_HEIGHT - DISPLAY_HEIGHT * 38 / 100);
    draw_text_at(epaper, text_x, text_y, msg, true);
}

static const char* ui_wifi_state_label(ConnState state, bool connecting) {
    if (connecting) {
        return "Connecting...";
    }
    switch (state) {
    case ConnState::Up:
        return "Connected";
    case ConnState::Initializing:
        return "Connecting...";
    case ConnState::InvalidCredentials:
        return "Auth failed";
    case ConnState::ConnectionError:
    default:
        return "Disconnected";
    }
}

static uint8_t ui_rssi_quality(int16_t rssi) {
    if (rssi <= -95) {
        return 0;
    }
    if (rssi >= -45) {
        return 100;
    }
    return static_cast<uint8_t>((rssi + 95) * 2);
}

static void ui_draw_settings_header(FASTEPD* epaper, const char* title) {
    epaper->fillRect(0, 0, DISPLAY_WIDTH, SETTINGS_HEADER_HEIGHT, BBEP_WHITE);
    ui_draw_back_button(epaper);
    epaper->setFont(Montserrat_Regular_26);
    const BB_RECT rect = get_text_box(epaper, title);
    const int16_t y = ROOM_CONTROLS_BACK_Y + (ROOM_CONTROLS_BACK_H + rect.h) / 2 - 4;
    draw_text_at(epaper, ROOM_CONTROLS_BACK_X + ROOM_CONTROLS_BACK_W + 32, y, title);
    epaper->drawLine(0, SETTINGS_HEADER_HEIGHT, DISPLAY_WIDTH, SETTINGS_HEADER_HEIGHT, BBEP_BLACK);
}

static void ui_draw_wifi_network_row(FASTEPD* epaper, int16_t x, int16_t y, int16_t w, const WifiNetwork& network, bool connected) {
    epaper->fillRoundRect(x, y, w, WIFI_NETWORK_ROW_H, 12, BBEP_WHITE);
    epaper->drawRoundRect(x, y, w, WIFI_NETWORK_ROW_H, 12, BBEP_BLACK);

    epaper->setFont(Roboto_Condensed_Regular_16);
    char ssid[MAX_WIFI_SSID_LEN];
    strncpy(ssid, network.ssid, sizeof(ssid) - 1);
    ssid[sizeof(ssid) - 1] = '\0';
    truncate_with_ellipsis(epaper, ssid, sizeof(ssid), static_cast<int16_t>(w - 190));

    const char* status;
    if (connected) {
        status = "CONN";
    } else if (network.known) {
        status = "SAVED";
    } else if (network.secure) {
        status = "LOCK";
    } else {
        status = "OPEN";
    }
    char right_text[40];
    snprintf(right_text, sizeof(right_text), "%s  %ddBm", status, static_cast<int>(network.rssi));
    BB_RECT right_rect = get_text_box(epaper, right_text);

    // Vertically center both labels in the row. Montserrat's cursor sits at
    // the baseline, so y = top + (h + rect.h)/2 - 4 (matches ui_draw_media_button).
    const int16_t baseline_y = y + (WIFI_NETWORK_ROW_H + right_rect.h) / 2 - 4;
    draw_text_at(epaper, x + 16, baseline_y, ssid);
    draw_text_at(epaper, x + w - right_rect.w - 14, baseline_y, right_text);
}

void ui_draw_wifi_settings(FASTEPD* epaper, const WifiSettingsSnapshot* snapshot) {
    epaper->setTextColor(BBEP_BLACK);
    ui_draw_settings_header(epaper, "Wi-Fi");

    epaper->fillRoundRect(WIFI_INFO_X, WIFI_INFO_Y, WIFI_INFO_W, WIFI_INFO_H, 14, BBEP_WHITE);
    epaper->drawRoundRect(WIFI_INFO_X, WIFI_INFO_Y, WIFI_INFO_W, WIFI_INFO_H, 14, BBEP_BLACK);

    // 26 px → 29 px between baselines (≈ +10%); first row baseline unchanged.
    constexpr int16_t info_first_y = 32;
    constexpr int16_t info_step_y = 29;
    epaper->setFont(Roboto_Condensed_Regular_20);
    draw_text_at(epaper, WIFI_INFO_X + 14, WIFI_INFO_Y + info_first_y, ui_wifi_state_label(snapshot->wifi_state, snapshot->connecting));

    epaper->setFont(Roboto_Condensed_Regular_16);
    char profile_line[96];
    if (snapshot->custom_profile_active && snapshot->profile_ssid[0] != '\0') {
        snprintf(profile_line, sizeof(profile_line), "Profile: Custom (%s)", snapshot->profile_ssid);
    } else {
        snprintf(profile_line, sizeof(profile_line), "Profile: Home default");
    }
    truncate_with_ellipsis(epaper, profile_line, sizeof(profile_line), static_cast<int16_t>(WIFI_INFO_W - 24));
    draw_text_at(epaper, WIFI_INFO_X + 14, WIFI_INFO_Y + info_first_y + info_step_y, profile_line);

    char ssid_line[96];
    snprintf(ssid_line, sizeof(ssid_line), "Network: %s", snapshot->connected && snapshot->connected_ssid[0] ? snapshot->connected_ssid : "(none)");
    draw_text_at(epaper, WIFI_INFO_X + 14, WIFI_INFO_Y + info_first_y + 2 * info_step_y, ssid_line);

    char ip_line[80];
    snprintf(ip_line, sizeof(ip_line), "IP: %s", snapshot->connected && snapshot->ip_address[0] ? snapshot->ip_address : "-");
    draw_text_at(epaper, WIFI_INFO_X + 14, WIFI_INFO_Y + info_first_y + 3 * info_step_y, ip_line);

    char signal_line[80];
    snprintf(signal_line, sizeof(signal_line), "Signal: %d dBm (%u%%)", static_cast<int>(snapshot->rssi),
             static_cast<unsigned>(ui_rssi_quality(snapshot->rssi)));
    draw_text_at(epaper, WIFI_INFO_X + 14, WIFI_INFO_Y + info_first_y + 4 * info_step_y, signal_line);

    const int16_t status_y = WIFI_INFO_Y + info_first_y + 5 * info_step_y;
    if (snapshot->scan_in_progress) {
        draw_text_at(epaper, WIFI_INFO_X + 14, status_y, "Scanning nearby networks...");
    } else if (snapshot->connect_error[0] != '\0') {
        draw_text_at(epaper, WIFI_INFO_X + 14, status_y, snapshot->connect_error);
    }

    epaper->fillRoundRect(WIFI_SCAN_BUTTON_X, WIFI_SCAN_BUTTON_Y, WIFI_SCAN_BUTTON_W, WIFI_SCAN_BUTTON_H, 10, BBEP_WHITE);
    epaper->drawRoundRect(WIFI_SCAN_BUTTON_X, WIFI_SCAN_BUTTON_Y, WIFI_SCAN_BUTTON_W, WIFI_SCAN_BUTTON_H, 10, BBEP_BLACK);
    epaper->setFont(Roboto_Condensed_Regular_16);
    BB_RECT scan_rect = get_text_box(epaper, "Scan");
    draw_text_at(epaper, WIFI_SCAN_BUTTON_X + (WIFI_SCAN_BUTTON_W - scan_rect.w) / 2,
                 WIFI_SCAN_BUTTON_Y + (WIFI_SCAN_BUTTON_H + scan_rect.h) / 2 - 2, "Scan");

    // Only show the "Use Default" button when there's a custom profile to
    // revert from; on the default profile the button has nothing to do.
    if (snapshot->custom_profile_active) {
        const char* default_label = "Use Default";
        epaper->fillRoundRect(WIFI_DEFAULT_BUTTON_X, WIFI_DEFAULT_BUTTON_Y, WIFI_DEFAULT_BUTTON_W, WIFI_DEFAULT_BUTTON_H, 10,
                              BBEP_WHITE);
        epaper->drawRoundRect(WIFI_DEFAULT_BUTTON_X, WIFI_DEFAULT_BUTTON_Y, WIFI_DEFAULT_BUTTON_W, WIFI_DEFAULT_BUTTON_H, 10, BBEP_BLACK);
        BB_RECT default_rect = get_text_box(epaper, default_label);
        draw_text_at(epaper, WIFI_DEFAULT_BUTTON_X + (WIFI_DEFAULT_BUTTON_W - default_rect.w) / 2,
                     WIFI_DEFAULT_BUTTON_Y + (WIFI_DEFAULT_BUTTON_H + default_rect.h) / 2 - 2, default_label);
    }

    const uint8_t page_count = snapshot->network_count == 0
                                   ? 1
                                   : static_cast<uint8_t>((snapshot->network_count + WIFI_NETWORKS_PER_PAGE - 1) / WIFI_NETWORKS_PER_PAGE);
    const uint8_t page = std::min(snapshot->page, static_cast<uint8_t>(page_count - 1));
    const uint8_t first_idx = static_cast<uint8_t>(page * WIFI_NETWORKS_PER_PAGE);
    const uint8_t last_idx = std::min<uint8_t>(snapshot->network_count, static_cast<uint8_t>(first_idx + WIFI_NETWORKS_PER_PAGE));

    if (snapshot->network_count == 0) {
        epaper->setFont(Roboto_Condensed_Regular_16);
        draw_text_at(epaper, WIFI_NETWORK_LIST_X + 4, WIFI_NETWORK_LIST_Y + 30, "No networks found. Tap Scan.");
    } else {
        for (uint8_t idx = first_idx; idx < last_idx; idx++) {
            int16_t row_y = WIFI_NETWORK_LIST_Y + static_cast<int16_t>((idx - first_idx) * (WIFI_NETWORK_ROW_H + WIFI_NETWORK_ROW_GAP));
            bool connected = snapshot->connected && strcmp(snapshot->connected_ssid, snapshot->networks[idx].ssid) == 0;
            ui_draw_wifi_network_row(epaper, WIFI_NETWORK_LIST_X, row_y, WIFI_NETWORK_LIST_W, snapshot->networks[idx], connected);
        }
    }

    if (page_count > 1) {
        char page_text[24];
        snprintf(page_text, sizeof(page_text), "Page %u/%u", static_cast<unsigned>(page + 1), static_cast<unsigned>(page_count));
        BB_RECT page_rect = get_text_box(epaper, page_text);
        const int16_t badge_w = page_rect.w + 22;
        const int16_t badge_x = DISPLAY_WIDTH - WIFI_INFO_X - badge_w;
        epaper->fillRoundRect(badge_x, WIFI_NETWORK_PAGE_BADGE_Y - 24, badge_w, 34, 10, BBEP_WHITE);
        epaper->drawRoundRect(badge_x, WIFI_NETWORK_PAGE_BADGE_Y - 24, badge_w, 34, 10, BBEP_BLACK);
        draw_text_at(epaper, badge_x + 11, WIFI_NETWORK_PAGE_BADGE_Y, page_text);
    }
}

static void ui_draw_key(FASTEPD* epaper, int16_t x, int16_t y, int16_t w, int16_t h, const char* label, bool active = false) {
    epaper->fillRoundRect(x, y, w, h, 8, active ? BBEP_BLACK : BBEP_WHITE);
    epaper->drawRoundRect(x, y, w, h, 8, BBEP_BLACK);
    BB_RECT text_rect = get_text_box(epaper, label);
    epaper->setTextColor(active ? BBEP_WHITE : BBEP_BLACK);
    draw_text_at(epaper, x + (w - text_rect.w) / 2, y + (h + text_rect.h) / 2 - 1, label);
    epaper->setTextColor(BBEP_BLACK);
}

void ui_draw_wifi_password(FASTEPD* epaper, const WifiPasswordSnapshot* snapshot) {
    epaper->setTextColor(BBEP_BLACK);
    ui_draw_settings_header(epaper, "Wi-Fi Password");
    epaper->setFont(Roboto_Condensed_Regular_16);

    epaper->fillRoundRect(WIFI_PASSWORD_BOX_X, WIFI_PASSWORD_BOX_Y, WIFI_PASSWORD_BOX_W, WIFI_PASSWORD_BOX_H, 14, BBEP_WHITE);
    epaper->drawRoundRect(WIFI_PASSWORD_BOX_X, WIFI_PASSWORD_BOX_Y, WIFI_PASSWORD_BOX_W, WIFI_PASSWORD_BOX_H, 14, BBEP_BLACK);

    char ssid_line[96];
    snprintf(ssid_line, sizeof(ssid_line), "Network: %s", snapshot->target_ssid);
    draw_text_at(epaper, WIFI_PASSWORD_BOX_X + 14, WIFI_PASSWORD_BOX_Y + 28, ssid_line);

    char pass_line[MAX_WIFI_PASSWORD_LEN + 20];
    snprintf(pass_line, sizeof(pass_line), "Password: %s", snapshot->password);
    truncate_with_ellipsis(epaper, pass_line, sizeof(pass_line), static_cast<int16_t>(WIFI_PASSWORD_BOX_W - 24));
    draw_text_at(epaper, WIFI_PASSWORD_BOX_X + 14, WIFI_PASSWORD_BOX_Y + 58, pass_line);
    draw_text_at(epaper, WIFI_PASSWORD_BOX_X + 14, WIFI_PASSWORD_BOX_Y + 88, snapshot->connecting ? "Connecting..." : "Tap Connect when ready");
    if (snapshot->connect_error[0] != '\0') {
        draw_text_at(epaper, WIFI_PASSWORD_BOX_X + 14, WIFI_PASSWORD_BOX_Y + 116, snapshot->connect_error);
    }

    const int16_t key_w = static_cast<int16_t>((WIFI_KEYBOARD_W - 9 * WIFI_KEY_GAP) / 10);
    const int16_t row1_y = WIFI_KEYBOARD_Y;
    const int16_t row2_y = row1_y + WIFI_KEY_H + WIFI_KEY_GAP;
    const int16_t row3_y = row2_y + WIFI_KEY_H + WIFI_KEY_GAP;
    const int16_t row4_y = row3_y + WIFI_KEY_H + WIFI_KEY_GAP;
    const int16_t row5_y = row4_y + WIFI_KEY_H + WIFI_KEY_GAP;

    auto draw_row = [&](int16_t row_x, int16_t row_y, const char* keys, uint8_t count) {
        for (uint8_t i = 0; i < count; i++) {
            int16_t x = static_cast<int16_t>(row_x + i * (key_w + WIFI_KEY_GAP));
            char label[2] = {keys[i], '\0'};
            if (!snapshot->symbols && snapshot->shift && label[0] >= 'a' && label[0] <= 'z') {
                label[0] = static_cast<char>(label[0] - ('a' - 'A'));
            }
            ui_draw_key(epaper, x, row_y, key_w, WIFI_KEY_H, label);
        }
    };

    const char* row1 = snapshot->symbols ? "1234567890" : "qwertyuiop";
    const char* row2 = snapshot->symbols ? "!@#$%^&*()" : "asdfghjkl";
    const char* row3 = snapshot->symbols ? "-_=+.,:/?" : "zxcvbnm";

    draw_row(WIFI_KEYBOARD_X, row1_y, row1, 10);
    draw_row(static_cast<int16_t>(WIFI_KEYBOARD_X + (key_w + WIFI_KEY_GAP) / 2), row2_y, row2, 9);
    draw_row(static_cast<int16_t>(WIFI_KEYBOARD_X + 2 * (key_w + WIFI_KEY_GAP)), row3_y, row3, 7);

    const int16_t shift_w = 88;
    const int16_t symbols_w = 92;
    const int16_t space_w = 180;
    const int16_t del_w = 88;
    const int16_t clear_w = 88;
    int16_t x = WIFI_KEYBOARD_X;
    ui_draw_key(epaper, x, row4_y, shift_w, WIFI_KEY_H, "Shift", snapshot->shift);
    x += shift_w + WIFI_KEY_GAP;
    ui_draw_key(epaper, x, row4_y, symbols_w, WIFI_KEY_H, snapshot->symbols ? "ABC" : "123", snapshot->symbols);
    x += symbols_w + WIFI_KEY_GAP;
    ui_draw_key(epaper, x, row4_y, space_w, WIFI_KEY_H, "Space");
    x += space_w + WIFI_KEY_GAP;
    ui_draw_key(epaper, x, row4_y, del_w, WIFI_KEY_H, "Del");
    x += del_w + WIFI_KEY_GAP;
    ui_draw_key(epaper, x, row4_y, clear_w, WIFI_KEY_H, "Clear");

    ui_draw_key(epaper, WIFI_KEYBOARD_X, row5_y, WIFI_KEYBOARD_W, WIFI_KEY_H, "Connect", snapshot->connecting);
}


void ui_task(void* arg) {
    UITaskArgs* ctx = static_cast<UITaskArgs*>(arg);
    UIState current_state = {};
    UIState displayed_state = {};
    // What is actually painted on the e-paper. Diverges from
    // displayed_state during standby — displayed_state follows the
    // logical mode (so the touch task sees Standby and routes
    // wake-touches via store_go_home), while panel_state stays at the
    // pre-standby screen, since the e-paper is not repainted on
    // standby entry. On wake, comparing current_state to panel_state
    // means we only repaint if the destination mode differs from
    // what's already on the panel, or if header bits drifted during
    // the standby window.
    UIState panel_state = {};
    bool display_is_dirty = false;
    static WifiSettingsSnapshot wifi_settings_snapshot;
    static WifiPasswordSnapshot wifi_password_snapshot;

    memset(&wifi_settings_snapshot, 0, sizeof(wifi_settings_snapshot));
    memset(&wifi_password_snapshot, 0, sizeof(wifi_password_snapshot));

    xTaskNotifyGive(xTaskGetCurrentTaskHandle()); // First refresh needs a notification

    while (1) {
        TickType_t notify_timeout = portMAX_DELAY;
        if (displayed_state.mode == UiMode::BatteryStatus) {
            // While the battery-status page is open, wake every 20 s to
            // re-poll the BQ25896. Charging state changes slowly and a
            // longer cadence keeps the e-paper full-redraw cost down.
            notify_timeout = pdMS_TO_TICKS(20000);
        } else if (display_is_dirty) {
            notify_timeout = pdMS_TO_TICKS(DISPLAY_FULL_REDRAW_TIMEOUT_MS);
        }

        const bool notified = ulTaskNotifyTake(pdTRUE, notify_timeout) != 0;
        const bool battery_status_refresh = !notified && displayed_state.mode == UiMode::BatteryStatus;
        if (notified || battery_status_refresh) {
            store_update_ui_state(ctx->store, &current_state);

            const bool mode_changed = current_state.mode != panel_state.mode;
            const bool settings_changed = current_state.settings_revision != panel_state.settings_revision;
            const bool battery_changed = current_state.battery_revision != panel_state.battery_revision;
            const bool device_changed = current_state.media_device_idx != panel_state.media_device_idx;
            const bool wifi_changed =
                wifi_bars_from_rssi(current_state.wifi_rssi, current_state.wifi_connected) !=
                wifi_bars_from_rssi(panel_state.wifi_rssi, panel_state.wifi_connected);
            // Suppress the home-screen header partial update on the
            // single iteration that exits standby, so the wake-touch
            // gets the fastest possible response. Header drift (wifi
            // bars, battery SoC) gets reconciled on the next sample
            // notification instead of on the wake itself.
            const bool waking_from_standby =
                displayed_state.mode == UiMode::Standby && current_state.mode != UiMode::Standby;

            if (current_state.mode == UiMode::Standby) {
                // Keep whatever was on screen before standby. The standby
                // state itself is still tracked for deep-sleep timing; the
                // 'Press to wake ->' screen is only painted right before
                // entering deep sleep (see main.cpp enter_deep_sleep).
            } else if (current_state.mode == UiMode::WifiSettings && (mode_changed || settings_changed)) {
                store_get_wifi_settings_snapshot(ctx->store, &wifi_settings_snapshot);
                ctx->epaper->setMode(BB_MODE_1BPP);
                ctx->epaper->fillScreen(BBEP_WHITE);
                ui_draw_wifi_settings(ctx->epaper, &wifi_settings_snapshot);
                { PmRefreshGuard g; ctx->epaper->fullUpdate(CLEAR_FAST, true); }
                display_is_dirty = false;
                panel_state = current_state;
            } else if (current_state.mode == UiMode::WifiPassword && (mode_changed || settings_changed)) {
                if (!store_get_wifi_password_snapshot(ctx->store, &wifi_password_snapshot)) {
                    current_state.mode = UiMode::GenericError;
                    ctx->epaper->setMode(BB_MODE_1BPP);
                    ctx->epaper->fillScreen(BBEP_WHITE);
                    ui_show_message(current_state.mode, ctx->epaper);
                    { PmRefreshGuard g; ctx->epaper->fullUpdate(CLEAR_FAST, true); }
                } else {
                    ctx->epaper->setMode(BB_MODE_1BPP);
                    ctx->epaper->fillScreen(BBEP_WHITE);
                    ui_draw_wifi_password(ctx->epaper, &wifi_password_snapshot);
                    { PmRefreshGuard g; ctx->epaper->fullUpdate(CLEAR_FAST, true); }
                }
                display_is_dirty = false;
                panel_state = current_state;
            } else if (current_state.mode == UiMode::MediaController && (mode_changed || device_changed)) {
                uint8_t batt_soc = 0;
                const bool batt_valid = store_get_battery_state(ctx->store, &batt_soc, nullptr);

                ctx->epaper->setMode(BB_MODE_1BPP);
                ctx->epaper->fillScreen(BBEP_WHITE);
                ui_draw_media_controller(ctx->epaper, ctx->config, current_state.media_device_idx, batt_valid, batt_soc,
                                         current_state.wifi_rssi, current_state.wifi_connected);
                { PmRefreshGuard g; ctx->epaper->fullUpdate(CLEAR_FAST, true); }
                display_is_dirty = false;
                panel_state = current_state;
            } else if (current_state.mode == UiMode::MediaController && (wifi_changed || battery_changed) && !waking_from_standby) {
                // Header-only change: redraw just the wifi button and/or
                // battery slot with a partial update so the rest of the
                // screen doesn't flash.
                ctx->epaper->setMode(BB_MODE_1BPP);

                if (wifi_changed) {
                    ctx->epaper->fillRoundRect(HOME_LEFT_BUTTON_X, HOME_LEFT_BUTTON_Y,
                                               HOME_LEFT_BUTTON_W, HOME_LEFT_BUTTON_H, 14, BBEP_WHITE);
                    ctx->epaper->drawRoundRect(HOME_LEFT_BUTTON_X, HOME_LEFT_BUTTON_Y,
                                               HOME_LEFT_BUTTON_W, HOME_LEFT_BUTTON_H, 14, BBEP_BLACK);
                    ui_draw_wifi_indicator(ctx->epaper,
                                           HOME_LEFT_BUTTON_X + HOME_LEFT_BUTTON_W / 2,
                                           HOME_LEFT_BUTTON_Y + HOME_LEFT_BUTTON_H / 2,
                                           current_state.wifi_rssi, current_state.wifi_connected);
                }

                // Battery slot is the strip just right of the wifi button.
                // Add 8 px of leeway on the right so a "100" SoC text that
                // overhangs the body stays inside the cleared region.
                constexpr int16_t batt_clear_x = HOME_LEFT_BUTTON_X + HOME_LEFT_BUTTON_W;
                constexpr int16_t batt_clear_w = HEADER_BATTERY_SLOT_W + 8;
                if (battery_changed) {
                    ctx->epaper->fillRect(batt_clear_x, HOME_LEFT_BUTTON_Y,
                                          batt_clear_w, HOME_LEFT_BUTTON_H, BBEP_WHITE);
                    uint8_t batt_soc = 0;
                    const bool batt_valid = store_get_battery_state(ctx->store, &batt_soc, nullptr);
                    if (batt_valid) {
                        const int16_t batt_y = HOME_LEFT_BUTTON_Y + HOME_LEFT_BUTTON_H / 2;
                        const int16_t batt_right = HOME_LEFT_BUTTON_X + HOME_LEFT_BUTTON_W
                                                   + HEADER_BATTERY_GAP_LEFT + HEADER_BATTERY_SLOT_W;
                        ui_draw_battery_indicator(ctx->epaper, batt_right, batt_y, batt_soc);
                    }
                }

                // partialUpdate row range is in panel-native coordinates.
                // With setRotation(90) the user x axis maps to inverted
                // native rows: native_row = DISPLAY_WIDTH - 1 - user_x.
                // Take the union of the two regions if both changed.
                int start_row = DISPLAY_WIDTH;
                int end_row = 0;
                if (wifi_changed) {
                    const int s = DISPLAY_WIDTH - HOME_LEFT_BUTTON_X - HOME_LEFT_BUTTON_W;
                    const int e = DISPLAY_WIDTH - 1 - HOME_LEFT_BUTTON_X;
                    if (s < start_row) start_row = s;
                    if (e > end_row) end_row = e;
                }
                if (battery_changed) {
                    const int s = DISPLAY_WIDTH - batt_clear_x - batt_clear_w;
                    const int e = DISPLAY_WIDTH - 1 - batt_clear_x;
                    if (s < start_row) start_row = s;
                    if (e > end_row) end_row = e;
                }
                { PmRefreshGuard g; ctx->epaper->partialUpdate(true, start_row, end_row); }
                display_is_dirty = false;
                panel_state = current_state;
            } else if (current_state.mode == UiMode::MediaDeviceSelect && (mode_changed || settings_changed)) {
                ctx->epaper->setMode(BB_MODE_1BPP);
                ctx->epaper->fillScreen(BBEP_WHITE);
                ui_draw_media_device_select(ctx->epaper, ctx->config, current_state.media_device_idx);
                { PmRefreshGuard g; ctx->epaper->fullUpdate(CLEAR_FAST, true); }
                display_is_dirty = false;
                panel_state = current_state;
            } else if (current_state.mode == UiMode::BatteryStatus &&
                       (mode_changed || settings_changed || battery_changed || battery_status_refresh)) {
                ChargerSnapshot charger = {};
                const bool charger_ok = battery_read_charger(&charger);
                FuelGaugeDetail detail = {};
                const bool detail_ok = battery_read_fuel_gauge_detail(&detail);
                uint8_t batt_soc = 0;
                uint16_t batt_mv = 0;
                const bool batt_valid = store_get_battery_state(ctx->store, &batt_soc, &batt_mv);

                ctx->epaper->setMode(BB_MODE_1BPP);
                ctx->epaper->fillScreen(BBEP_WHITE);
                ui_draw_battery_status(ctx->epaper,
                                       charger_ok ? &charger : nullptr,
                                       detail_ok ? &detail : nullptr,
                                       batt_valid, batt_soc, batt_mv);
                { PmRefreshGuard g; ctx->epaper->fullUpdate(CLEAR_FAST, true); }
                display_is_dirty = false;
                panel_state = current_state;
            } else if (mode_changed) {
                ctx->epaper->setMode(BB_MODE_1BPP);
                ctx->epaper->fillScreen(BBEP_WHITE);
                ui_show_message(current_state.mode, ctx->epaper);
                { PmRefreshGuard g; ctx->epaper->fullUpdate(CLEAR_FAST, true); }
                display_is_dirty = false;
                panel_state = current_state;
            }

            displayed_state = current_state;
            ui_state_set(ctx->shared_state, &displayed_state);
        }
    }
}
