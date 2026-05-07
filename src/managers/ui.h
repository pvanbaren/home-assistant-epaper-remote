#pragma once
#include "config.h"
#include "store.h"
#include "ui_state.h"
#include <FastEPD.h>

struct UITaskArgs {
    EntityStore* store;
    FASTEPD* epaper;
    SharedUIState* shared_state;
    const Configuration* config;
};

void ui_task(void* arg);
void ui_draw_standby_screen(FASTEPD* epaper);
void ui_draw_media_controller(FASTEPD* epaper, const Configuration* config, uint8_t device_idx, bool battery_valid, uint8_t battery_soc_pct,
                              int16_t wifi_rssi, bool wifi_connected);
void ui_draw_media_device_select(FASTEPD* epaper, const Configuration* config, uint8_t active_idx);
struct ChargerSnapshot;
struct FuelGaugeDetail;
void ui_draw_battery_status(FASTEPD* epaper, const ChargerSnapshot* charger, const FuelGaugeDetail* detail,
                            bool fuel_valid, uint8_t soc_pct, uint16_t voltage_mv);