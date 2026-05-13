#pragma once

#include "boards.h"
#include <cstddef>
#include <cstdint>

// Home assistant configuration
constexpr uint32_t HASS_MAX_JSON_BUFFER = 1024 * 256; // 256k, area/entity registries can be large
constexpr uint32_t HASS_RECONNECT_DELAY_MS = 10000;

// When sending commands too fast (on a slider), this can flood
// the zigbee network and make the commands fail. Increase this delay
// if you see errors when using sliders.
constexpr uint32_t HASS_TASK_SEND_DELAY_MS = 50;
constexpr size_t   MEDIA_COMMAND_QUEUE_SIZE = 8;
// Drop queued commands that have been sitting around longer than this —
// stale state changes are worse than no change, especially after a
// reconnect or a flood from a slider that the user already moved past.
constexpr uint32_t MEDIA_COMMAND_MAX_AGE_MS = 2000;

// When sending commands, we'll receive the updates from the server
// with a delay. This causes jittering in the slider and unnecessary
// commands sent to the server. We ignore updates from the server
// during this delay after a command was sent on an entity.
// FIXME: We can lose updates, we should have an authoritative value
// and a target value in the store at some point.
constexpr uint32_t HASS_IGNORE_UPDATE_DELAY_MS = 1000;

// Other constants
constexpr size_t MAX_WIFI_NETWORKS = 24;
constexpr uint8_t MAX_WIFI_SAVED_NETWORKS = 8;
constexpr size_t MAX_WIFI_SSID_LEN = 33;
constexpr size_t MAX_WIFI_PASSWORD_LEN = 64;
constexpr size_t MAX_WIFI_IP_LEN = 20;
constexpr size_t MAX_WIFI_ERROR_LEN = 64;
constexpr uint32_t WIFI_BOOT_SETTINGS_FALLBACK_MS = 7000;
constexpr uint32_t TOUCH_RELEASE_TIMEOUT_MS = 80;
constexpr uint32_t TOUCH_RELEASE_POLL_MS = 5;
// Hold time after which a still-pressed touchpad contact fires DpadOk
// without waiting for finger lift. Anything below this still goes through
// the release-edge tap path so a brisk tap-and-lift still registers.
constexpr uint32_t TOUCHPAD_TAP_HOLD_MS = 120;
constexpr uint32_t DISPLAY_FULL_REDRAW_TIMEOUT_MS = 15000;
constexpr uint8_t DISPLAY_PARTIAL_UPDATE_PASSES = 2;
constexpr uint8_t DISPLAY_FULL_UPDATE_PASSES = 4;
// Idle phases, all measured as time since last_interaction_ms:
//   0 .. BACKLIGHT_PULSE_MS              → Active (backlight on)
//   .. STANDBY_IDLE_TIMEOUT_MS           → Dim    (backlight off, UI active)
//   .. DEEP_SLEEP_IDLE_TIMEOUT_MS        → Standby
//   beyond                               → DeepSleep
constexpr uint32_t BACKLIGHT_PULSE_MS = 10 * 1000;          // frontlight on for 10 s after each user interaction
constexpr uint32_t STANDBY_IDLE_TIMEOUT_MS = 60 * 1000;     // 1 minute total idle → Standby
constexpr uint32_t DEEP_SLEEP_IDLE_TIMEOUT_MS = 600 * 1000; // 10 minute total idle → DeepSleep
constexpr uint32_t BATTERY_REFRESH_INTERVAL_MS = 20 * 1000; // 20 s between battery samples
constexpr uint16_t BATTERY_FULL_MV = 4200;
constexpr uint16_t BATTERY_EMPTY_MV = 3300;

// Paginated list swipe (used by the Wi-Fi network list).
constexpr uint16_t ROOM_LIST_SWIPE_THRESHOLD_X = 80;

// Media controller geometry
constexpr int16_t MEDIA_MARGIN_X = 20;
constexpr int16_t MEDIA_BUTTON_GAP = 12;
constexpr int16_t MEDIA_BUTTON_H = 80;
constexpr int16_t MEDIA_BUTTON_RADIUS = 14;

// Order top -> bottom:
//   nav row -> touchpad (tap=OK, swipe=dpad) -> transport -> 3x3 (sources + volume).
// Header sits above the nav row; the 3x3 grid is anchored at the bottom.
constexpr int16_t MEDIA_NAV_ROW_Y = 96;
constexpr int16_t MEDIA_DPAD_Y = MEDIA_NAV_ROW_Y + MEDIA_BUTTON_H + 24;
constexpr int16_t MEDIA_DPAD_SIZE = 352;
constexpr int16_t MEDIA_TRANSPORT_ROW_Y = MEDIA_DPAD_Y + MEDIA_DPAD_SIZE + 24;
constexpr int16_t MEDIA_VOLUME_ROW_Y = MEDIA_TRANSPORT_ROW_Y + MEDIA_BUTTON_H + 24;
constexpr int16_t MEDIA_VOLUME_ROW2_Y = MEDIA_VOLUME_ROW_Y + MEDIA_BUTTON_H + MEDIA_BUTTON_GAP;
constexpr int16_t MEDIA_VOLUME_ROW3_Y = MEDIA_VOLUME_ROW2_Y + MEDIA_BUTTON_H + MEDIA_BUTTON_GAP;

// Two equal slots in the header: left hosts the Wi-Fi indicator (tap →
// Wi-Fi settings); right hosts the Power button. Naming is geometric so
// the role assignments can change without renaming again.
constexpr uint16_t HOME_LEFT_BUTTON_X = MEDIA_MARGIN_X;
constexpr uint16_t HOME_LEFT_BUTTON_Y = 18;
constexpr uint16_t HOME_LEFT_BUTTON_W = 64;
constexpr uint16_t HOME_LEFT_BUTTON_H = 64;

constexpr uint16_t HOME_RIGHT_BUTTON_X = DISPLAY_WIDTH - 92;
constexpr uint16_t HOME_RIGHT_BUTTON_Y = 18;
constexpr uint16_t HOME_RIGHT_BUTTON_W = 64;
constexpr uint16_t HOME_RIGHT_BUTTON_H = 64;

// Back-button hit region used by the Wi-Fi settings / password screens
// (legacy "ROOM_CONTROLS" prefix kept for now).
constexpr uint16_t ROOM_CONTROLS_BACK_X = 20;
constexpr uint16_t ROOM_CONTROLS_BACK_Y = 25;
constexpr uint16_t ROOM_CONTROLS_BACK_W = 120;
constexpr uint16_t ROOM_CONTROLS_BACK_H = 60;

// Settings / Wi-Fi UI geometry
constexpr uint16_t SETTINGS_HEADER_HEIGHT = 100;

constexpr uint16_t WIFI_INFO_X = 24;
constexpr uint16_t WIFI_INFO_Y = SETTINGS_HEADER_HEIGHT + 18;
constexpr uint16_t WIFI_INFO_W = DISPLAY_WIDTH - 2 * WIFI_INFO_X;
constexpr uint16_t WIFI_INFO_H = 190;
constexpr uint16_t WIFI_ACTIONS_Y = WIFI_INFO_Y + WIFI_INFO_H + 12;
constexpr uint16_t WIFI_SCAN_BUTTON_X = WIFI_INFO_X;
constexpr uint16_t WIFI_SCAN_BUTTON_Y = WIFI_ACTIONS_Y;
constexpr uint16_t WIFI_SCAN_BUTTON_W = 170;
constexpr uint16_t WIFI_SCAN_BUTTON_H = 46;
constexpr uint16_t WIFI_DEFAULT_BUTTON_X = WIFI_SCAN_BUTTON_X + WIFI_SCAN_BUTTON_W + 12;
constexpr uint16_t WIFI_DEFAULT_BUTTON_Y = WIFI_ACTIONS_Y;
constexpr uint16_t WIFI_DEFAULT_BUTTON_W = WIFI_INFO_X + WIFI_INFO_W - WIFI_DEFAULT_BUTTON_X;
constexpr uint16_t WIFI_DEFAULT_BUTTON_H = WIFI_SCAN_BUTTON_H;
constexpr uint16_t WIFI_NETWORK_LIST_X = WIFI_INFO_X;
constexpr uint16_t WIFI_NETWORK_LIST_Y = WIFI_ACTIONS_Y + WIFI_SCAN_BUTTON_H + 14;
constexpr uint16_t WIFI_NETWORK_LIST_W = WIFI_INFO_W;
constexpr uint16_t WIFI_NETWORK_ROW_H = 64;
constexpr uint16_t WIFI_NETWORK_ROW_GAP = 10;
constexpr uint8_t WIFI_NETWORKS_PER_PAGE = 7;
// Y is the badge's text baseline. Box top = Y - 24, height 34, so the
// box bottom sits at Y + 10. With Y = DISPLAY_HEIGHT - 34 the box bottom
// matches WIFI_INFO_X (= 24) for a uniform side/bottom margin.
constexpr uint16_t WIFI_NETWORK_PAGE_BADGE_Y = DISPLAY_HEIGHT - 34;

constexpr uint16_t WIFI_PASSWORD_BOX_X = WIFI_INFO_X;
constexpr uint16_t WIFI_PASSWORD_BOX_Y = SETTINGS_HEADER_HEIGHT + 20;
constexpr uint16_t WIFI_PASSWORD_BOX_W = WIFI_INFO_W;
constexpr uint16_t WIFI_PASSWORD_BOX_H = 140;
constexpr uint16_t WIFI_KEYBOARD_X = 18;
constexpr uint16_t WIFI_KEYBOARD_Y = WIFI_PASSWORD_BOX_Y + WIFI_PASSWORD_BOX_H + 16;
constexpr uint16_t WIFI_KEYBOARD_W = DISPLAY_WIDTH - 2 * WIFI_KEYBOARD_X;
constexpr uint16_t WIFI_KEY_H = 56;
constexpr uint16_t WIFI_KEY_GAP = 8;

// WiFi Disconnected screen buttons
constexpr uint16_t WIFI_DISC_BUTTON_Y = 750;
constexpr uint16_t WIFI_DISC_BUTTON_H = 80;
constexpr uint16_t WIFI_DISC_RETRY_X = 30;
constexpr uint16_t WIFI_DISC_RETRY_W = 230;
constexpr uint16_t WIFI_DISC_SETTINGS_X = 280;
constexpr uint16_t WIFI_DISC_SETTINGS_W = 230;

