#pragma once

#include <cstddef>
#include <cstdint>

struct MediaRemoteCommands {
    const char* up;
    const char* down;
    const char* left;
    const char* right;
    const char* select;
    const char* back;
    const char* home;
    const char* menu;
    const char* play_pause;
    const char* rewind;
    const char* fast_forward;
    const char* previous;
};

constexpr size_t MEDIA_SOURCE_COUNT = 6;
constexpr size_t MAX_MEDIA_DEVICES = 6;

struct MediaSource {
    const char* label;          // fallback text if icon == nullptr
    const char* source_name;    // passed to media_player.select_source
    const uint8_t* icon;        // optional 64x64 BMP from icons.h
};

// Generic Home Assistant service-call descriptor used by hardware buttons
// that map to a configurable action. Fire-and-forget — the result of the
// service call is not surfaced back to the UI. Leave domain = nullptr to
// mark the action as unset.
struct HassAction {
    const char* domain;     // e.g. "script", "light", "media_player"
    const char* service;    // e.g. "turn_on", "toggle", "media_play_pause"
    const char* entity_id;  // target entity, or nullptr to omit
};

struct MediaDevice {
    const char* title;                    // header label, e.g. "Living Room"
    const char* remote_entity_id;         // remote.* target for D-pad / transport / Back / Home / Menu
    const char* volume_entity_id;         // media_player.* for volume up/down/mute
    const char* source_entity_id;         // media_player.* for select_source (often the streaming box)
    HassAction power_action;              // Action fired by the on-screen power button. Leave domain = nullptr to disable.
    MediaSource sources[MEDIA_SOURCE_COUNT];
};

struct Configuration {
    const char* wifi_ssid;
    const char* wifi_password;
    // Optional static IP for the default profile. Leave wifi_static_ip
    // null to use DHCP. wifi_static_ip / wifi_static_gateway /
    // wifi_static_subnet must all be set together (any null → DHCP).
    // wifi_dns1 / wifi_dns2 are optional; nullptr falls back to gateway.
    // Custom profiles (entered via the on-device Wi-Fi UI) always use
    // DHCP regardless of these fields.
    const char* wifi_static_ip;
    const char* wifi_static_gateway;
    const char* wifi_static_subnet;
    const char* wifi_dns1;
    const char* wifi_dns2;

    const char* home_assistant_url;
    const char* home_assistant_token;
    const char* root_ca;

    // Remote command names are shared across devices (Roku ECP, etc.).
    MediaRemoteCommands media_remote_commands;

    MediaDevice media_devices[MAX_MEDIA_DEVICES];
    size_t media_device_count;
};

constexpr const char* ISRG_ROOT_X1 = ( // LetsEncrypt CA
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "-----END CERTIFICATE-----\n");
