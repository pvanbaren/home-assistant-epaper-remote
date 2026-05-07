#pragma once
#include "store.h"

struct BatteryTaskArgs {
    EntityStore* store;
};

void battery_task(void* arg);

// BQ25896 charger snapshot. Populated on demand by battery_read_charger();
// not maintained in the background. All voltages are millivolts. chrg_stat
// is the raw 2-bit field from REG0B (0..3 == Not / Pre / Fast / Done).
struct ChargerSnapshot {
    bool valid;          // false if the chip didn't ACK
    bool vbus_present;   // VBUS_GD bit
    bool power_good;     // PG_STAT bit
    uint8_t vbus_stat;   // 3-bit VBUS_STAT field
    uint8_t chrg_stat;   // 2-bit CHRG_STAT field
    uint16_t batv_mv;    // BATV ADC
    uint16_t sysv_mv;    // SYSV ADC
    uint16_t vbus_mv;    // VBUS ADC
    uint16_t ichg_ma;    // charge current ADC (REG12)
};

// Probe the BQ25896 at BATTERY_BQ25896_I2C_ADDR and read its status registers.
// Returns true on a successful read; populates out->valid in either case.
// No-op (returns false) on boards without a BQ25896 configured.
bool battery_read_charger(ChargerSnapshot* out);

// BQ27220 detail snapshot — fields that aren't part of the steady-state
// header battery indicator. avg_current_ma is signed (positive = charging,
// negative = discharging). time_to_empty_min is 0xFFFF when invalid (not
// discharging or insufficient learning). full_charge_capacity_mah is the
// gauge's learned capacity (decreases as the battery ages).
struct FuelGaugeDetail {
    bool valid;
    int16_t avg_current_ma;
    uint16_t time_to_empty_min;
    uint16_t full_charge_capacity_mah;
    int16_t temperature_dc;          // deci-Celsius (271 = 27.1 °C); INT16_MIN if invalid
};

// Read AverageCurrent() (0x10) and AverageTimeToEmpty() (0x18). No-op
// (returns false) on boards without a BQ27220 configured.
bool battery_read_fuel_gauge_detail(FuelGaugeDetail* out);

// Issue SET_HIBERNATE (0x0011) to the BQ27220 Control() register. Drops the
// gauge from active (~30–100 µA) to hibernate (~5–10 µA); state-of-charge
// tracking is preserved and the chip wakes on the next I2C transaction.
// Call right before deep sleep to minimize quiescent draw. No-op on boards
// without a BQ27220.
bool battery_hibernate_fuel_gauge();
