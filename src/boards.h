#pragma once
#include <FastEPD.h>
#include <cstddef>
#include <cstdint>

#ifdef TARGET_LILYGO_T5_S3_PRO
constexpr uint16_t DISPLAY_WIDTH = 540;
constexpr uint16_t DISPLAY_HEIGHT = 960;
constexpr size_t TOUCH_SDA = 39;
constexpr size_t TOUCH_SCL = 40;
constexpr size_t TOUCH_INT = 3;
constexpr size_t TOUCH_RST = 9;
constexpr int HOME_BUTTON_PIN = 0; // Front boot button
constexpr bool HOME_BUTTON_ACTIVE_LOW = true;
// Frontlight / backlight rail. Driven HIGH = on, LOW = off. Set to the
// GPIO that controls the front LED on your board, or -1 to disable.
constexpr int BACKLIGHT_PIN = 11;
// PCA9535 GPIO expander INT line (the chip itself is owned by FastEPD's
// BB_PANEL_EPDIY_V7 driver — we only listen on its INT to know when an
// input bit we care about has flipped). Open-drain active-low. Set to
// the SoC GPIO it's routed to, or -1 to disable.
constexpr int IO_EXPANDER_INT_PIN = 38;
// Side "IO48" button mapped to expander pin 10, active-low. Set to -1 to
// disable. The per-bit log in loop() still fires regardless; this just
// adds the named "IO48 button pressed" event and the backlight pulse.
constexpr int EXPANDER_IO48_BIT = 10;
constexpr int DISPLAY_PANEL = BB_PANEL_EPDIY_V7;
// Battery sense: set BATTERY_ADC_PIN to the ADC GPIO that reads battery
// voltage through the on-board divider (-1 disables the indicator).
// Verify against your board's schematic before enabling.
constexpr int BATTERY_ADC_PIN = -1;
constexpr int BATTERY_ENABLE_PIN = -1; // GPIO that gates the divider, or -1
constexpr uint16_t BATTERY_DIVIDER_NUM = 2; // V_batt = adc_mv * NUM / DEN
constexpr uint16_t BATTERY_DIVIDER_DEN = 1;
// BQ27220 fuel gauge over I2C (shares the touch I2C bus). Set to the 7-bit
// I2C address (typically 0x55) to enable; 0 disables. When enabled the
// BQ27220 path is used in preference to the ADC path above.
constexpr uint8_t BATTERY_BQ27220_I2C_ADDR = 0x55;
// BQ25896 charger over I2C. Set to the 7-bit address (typically 0x6B) to
// enable; 0 disables. Read-only and only accessed while the user is on the
// Battery Status page — no background polling.
constexpr uint8_t BATTERY_BQ25896_I2C_ADDR = 0x6B;
#endif

#ifdef TARGET_M5PAPER_S3
constexpr uint16_t DISPLAY_WIDTH = 540;
constexpr uint16_t DISPLAY_HEIGHT = 960;
constexpr size_t TOUCH_SDA = 41;
constexpr size_t TOUCH_SCL = 42;
constexpr size_t TOUCH_INT = 48;
constexpr size_t TOUCH_RST = 0;
constexpr int HOME_BUTTON_PIN = -1; // No dedicated front home button mapping
constexpr bool HOME_BUTTON_ACTIVE_LOW = true;
constexpr int BACKLIGHT_PIN = -1;
constexpr int IO_EXPANDER_INT_PIN = -1;
constexpr int EXPANDER_IO48_BIT = -1;
constexpr int DISPLAY_PANEL = BB_PANEL_M5PAPERS3;
// M5 Paper S3 reads battery via the AXP2101 PMU over I2C, not a direct ADC.
// Leave BATTERY_ADC_PIN = -1 here; integrating AXP2101 is a separate driver.
constexpr int BATTERY_ADC_PIN = -1;
constexpr int BATTERY_ENABLE_PIN = -1;
constexpr uint16_t BATTERY_DIVIDER_NUM = 2;
constexpr uint16_t BATTERY_DIVIDER_DEN = 1;
constexpr uint8_t BATTERY_BQ27220_I2C_ADDR = 0;
constexpr uint8_t BATTERY_BQ25896_I2C_ADDR = 0;
#endif
