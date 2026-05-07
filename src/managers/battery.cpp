#include "managers/battery.h"
#include "boards.h"
#include "constants.h"
#include <Arduino.h>
#include <Wire.h>
#include <esp_log.h>

static const char* TAG = "battery";

// BQ27220 standard command registers (little-endian word reads).
constexpr uint8_t BQ27220_REG_TEMPERATURE              = 0x06;
constexpr uint8_t BQ27220_REG_VOLTAGE                  = 0x08;
constexpr uint8_t BQ27220_REG_FULL_CHARGE_CAPACITY     = 0x12;
constexpr uint8_t BQ27220_REG_AVG_CURRENT              = 0x14;
constexpr uint8_t BQ27220_REG_AVG_TIME_TO_EMPTY        = 0x16;
constexpr uint8_t BQ27220_REG_STATE_OF_CHARGE          = 0x2C;

// BQ25896 status registers.
constexpr uint8_t BQ25896_REG_SYS_STATUS = 0x0B;
constexpr uint8_t BQ25896_REG_BATV       = 0x0E;
constexpr uint8_t BQ25896_REG_SYSV       = 0x0F;
constexpr uint8_t BQ25896_REG_ICHG       = 0x12;
constexpr uint8_t BQ25896_REG_VBUS       = 0x11;

static uint8_t voltage_to_soc_pct(uint16_t mv) {
    if (mv <= BATTERY_EMPTY_MV) {
        return 0;
    }
    if (mv >= BATTERY_FULL_MV) {
        return 100;
    }
    const uint32_t span = static_cast<uint32_t>(BATTERY_FULL_MV - BATTERY_EMPTY_MV);
    const uint32_t above = static_cast<uint32_t>(mv - BATTERY_EMPTY_MV);
    return static_cast<uint8_t>((above * 100) / span);
}

static bool bq27220_read_word(uint8_t reg, uint16_t* out) {
    Wire.beginTransmission(BATTERY_BQ27220_I2C_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }
    if (Wire.requestFrom(static_cast<int>(BATTERY_BQ27220_I2C_ADDR), 2) != 2) {
        return false;
    }
    const uint8_t lo = Wire.read();
    const uint8_t hi = Wire.read();
    *out = (static_cast<uint16_t>(hi) << 8) | lo;
    return true;
}

static bool bq25896_read_byte(uint8_t reg, uint8_t* out) {
    Wire.beginTransmission(BATTERY_BQ25896_I2C_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        return false;
    }
    if (Wire.requestFrom(static_cast<int>(BATTERY_BQ25896_I2C_ADDR), 1) != 1) {
        return false;
    }
    *out = Wire.read();
    return true;
}

bool battery_read_charger(ChargerSnapshot* out) {
    if (!out) return false;
    *out = ChargerSnapshot{};
    if (BATTERY_BQ25896_I2C_ADDR == 0) {
        return false;
    }
    Wire.begin(TOUCH_SDA, TOUCH_SCL);

    uint8_t status = 0, batv = 0, sysv = 0, vbus = 0, ichg = 0;
    if (!bq25896_read_byte(BQ25896_REG_SYS_STATUS, &status) ||
        !bq25896_read_byte(BQ25896_REG_BATV, &batv) ||
        !bq25896_read_byte(BQ25896_REG_SYSV, &sysv) ||
        !bq25896_read_byte(BQ25896_REG_VBUS, &vbus) ||
        !bq25896_read_byte(BQ25896_REG_ICHG, &ichg)) {
        ESP_LOGW(TAG, "BQ25896 read failed");
        return false;
    }

    out->valid = true;
    out->vbus_stat = (status >> 5) & 0x07;
    out->chrg_stat = (status >> 3) & 0x03;
    out->power_good = (status & 0x04) != 0;
    out->vbus_present = (vbus & 0x80) != 0;
    out->batv_mv = static_cast<uint16_t>(2304 + (batv & 0x7F) * 20);
    out->sysv_mv = static_cast<uint16_t>(2304 + (sysv & 0x7F) * 20);
    out->vbus_mv = out->vbus_present ? static_cast<uint16_t>(2600 + (vbus & 0x7F) * 100) : 0;
    out->ichg_ma = static_cast<uint16_t>((ichg & 0x7F) * 50);
    return true;
}

bool battery_read_fuel_gauge_detail(FuelGaugeDetail* out) {
    if (!out) return false;
    *out = FuelGaugeDetail{};
    if (BATTERY_BQ27220_I2C_ADDR == 0) {
        return false;
    }
    Wire.begin(TOUCH_SDA, TOUCH_SCL);

    uint16_t avg_raw = 0;
    if (!bq27220_read_word(BQ27220_REG_AVG_CURRENT, &avg_raw)) {
        return false;
    }
    uint16_t tte_raw = 0;
    if (!bq27220_read_word(BQ27220_REG_AVG_TIME_TO_EMPTY, &tte_raw)) {
        return false;
    }
    uint16_t fcc_raw = 0;
    if (!bq27220_read_word(BQ27220_REG_FULL_CHARGE_CAPACITY, &fcc_raw)) {
        return false;
    }
    uint16_t temp_raw = 0;
    if (!bq27220_read_word(BQ27220_REG_TEMPERATURE, &temp_raw)) {
        return false;
    }

    out->valid = true;
    out->avg_current_ma = static_cast<int16_t>(avg_raw);
    out->time_to_empty_min = tte_raw;
    out->full_charge_capacity_mah = fcc_raw;
    // BQ27220 reports temperature in 0.1 K. Convert to deci-Celsius
    // (271 = 27.1 °C). 0 K is implausible and means the read returned junk.
    if (temp_raw == 0) {
        out->temperature_dc = INT16_MIN;
    } else {
        out->temperature_dc = static_cast<int16_t>(static_cast<int32_t>(temp_raw) - 2732);
    }
    return true;
}

static bool bq27220_sample(uint16_t* voltage_mv, uint8_t* soc_pct) {
    uint16_t v = 0;
    if (!bq27220_read_word(BQ27220_REG_VOLTAGE, &v)) {
        return false;
    }
    uint16_t soc = 0;
    if (!bq27220_read_word(BQ27220_REG_STATE_OF_CHARGE, &soc)) {
        return false;
    }
    if (soc > 100) {
        soc = 100;
    }
    *voltage_mv = v;
    *soc_pct = static_cast<uint8_t>(soc);
    return true;
}

void battery_task(void* arg) {
    BatteryTaskArgs* ctx = static_cast<BatteryTaskArgs*>(arg);
    EntityStore* store = ctx->store;

    if (BATTERY_BQ27220_I2C_ADDR > 0) {
        // Share the touch I2C bus. Wire.begin is idempotent on Arduino-ESP32,
        // so this is safe even after BBCapTouch has already initialized it.
        // Concurrent transactions with the touch task can lose individual reads;
        // both sides retry on their next cycle.
        Wire.begin(TOUCH_SDA, TOUCH_SCL);

        ESP_LOGI(TAG, "Battery source: BQ27220 @ 0x%02X", BATTERY_BQ27220_I2C_ADDR);

        while (true) {
            uint16_t voltage_mv = 0;
            uint8_t soc_pct = 0;
            if (bq27220_sample(&voltage_mv, &soc_pct)) {
                store_set_battery_state(store, true, soc_pct, voltage_mv);
            } else {
                ESP_LOGW(TAG, "BQ27220 read failed");
            }
            vTaskDelay(pdMS_TO_TICKS(BATTERY_REFRESH_INTERVAL_MS));
        }
        return;
    }

    if (BATTERY_ADC_PIN < 0) {
        ESP_LOGI(TAG, "Battery sensor not configured for this board; task exiting");
        store_set_battery_state(store, false, 0, 0);
        vTaskDelete(nullptr);
        return;
    }

    if (BATTERY_ENABLE_PIN >= 0) {
        pinMode(BATTERY_ENABLE_PIN, OUTPUT);
        digitalWrite(BATTERY_ENABLE_PIN, LOW);
    }

    ESP_LOGI(TAG, "Battery source: ADC pin %d", BATTERY_ADC_PIN);

    while (true) {
        if (BATTERY_ENABLE_PIN >= 0) {
            digitalWrite(BATTERY_ENABLE_PIN, HIGH);
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        constexpr int samples = 8;
        uint32_t sum_mv = 0;
        for (int i = 0; i < samples; i++) {
            sum_mv += analogReadMilliVolts(BATTERY_ADC_PIN);
        }
        const uint32_t adc_mv = sum_mv / samples;

        if (BATTERY_ENABLE_PIN >= 0) {
            digitalWrite(BATTERY_ENABLE_PIN, LOW);
        }

        const uint32_t batt_mv = (adc_mv * BATTERY_DIVIDER_NUM) / BATTERY_DIVIDER_DEN;
        const uint8_t soc_pct = voltage_to_soc_pct(static_cast<uint16_t>(batt_mv));

        store_set_battery_state(store, true, soc_pct, static_cast<uint16_t>(batt_mv));

        vTaskDelay(pdMS_TO_TICKS(BATTERY_REFRESH_INTERVAL_MS));
    }
}
