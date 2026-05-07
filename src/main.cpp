#include "boards.h"
#include "config_remote.h"
#include "constants.h"
#include "managers/home_assistant.h"
#include "managers/touch.h"
#include "managers/ui.h"
#include "managers/wifi.h"
#include "pm_lock.h"
#include "screen.h"
#include "store.h"
#include "ui_state.h"
#include "widgets/Slider.h"
#include <Arduino.h>
#include <FastEPD.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_pm.h>
#include <esp_sleep.h>

static Configuration config;
static const char* TAG = "main";

static FASTEPD epaper;
static Screen screen;
static BBCapTouch bbct;
static EntityStore store;
static SharedUIState shared_ui_state;

static UITaskArgs ui_task_args;
static TouchTaskArgs touch_task_args;
static HomeAssistantTaskArgs hass_task_args;

static volatile TaskHandle_t s_main_task_handle = nullptr;
static volatile bool s_home_button_pending = false;

static void IRAM_ATTR home_button_isr() {
    s_home_button_pending = true;
    if (s_main_task_handle) {
        BaseType_t hpw = pdFALSE;
        vTaskNotifyGiveFromISR(s_main_task_handle, &hpw);
        portYIELD_FROM_ISR(hpw);
    }
}

void setup() {
    // Bluetooth is unused; release the controller so the BT phy can stay off.
    // Idempotent / safe even if BT was never initialized.
    btStop();

#if defined(ENABLE_PM_DFS) || defined(ENABLE_PM_LIGHT_SLEEP)
    // Dynamic frequency scaling so the SoC can drop to min_freq_mhz between
    // FreeRTOS ticks. Min 40 MHz is the XTAL clock — the deepest DFS state on
    // ESP32-S3.
    esp_pm_config_esp32s3_t pm_cfg = {
        .max_freq_mhz = 240,
        .min_freq_mhz = 40,
#if defined(ENABLE_PM_LIGHT_SLEEP)
        .light_sleep_enable = true,
#else
        .light_sleep_enable = false,
#endif
    };
    esp_err_t pm_err = esp_pm_configure(&pm_cfg);
    if (pm_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_configure returned %s", esp_err_to_name(pm_err));
    }
#endif

    // Allocate the NO_LIGHT_SLEEP lock e-paper refreshes will hold.
    // No-op when no PM flags are defined.
    pm_lock_init();

    s_main_task_handle = xTaskGetCurrentTaskHandle();

    // Initialize objects
    store_init(&store);
    ui_state_init(&shared_ui_state);
    configure_remote(&config, &store, &screen);
    initialize_slider_sprites();

    // Initialize display
    epaper.initPanel(DISPLAY_PANEL);
    epaper.setPanelSize(DISPLAY_HEIGHT, DISPLAY_WIDTH);
    epaper.setRotation(90);
    epaper.setPasses(DISPLAY_PARTIAL_UPDATE_PASSES, DISPLAY_FULL_UPDATE_PASSES);
    epaper.einkPower(true); // FIXME: Disabling power makes the GT911 unavailable

    // Launch UI task
    ui_task_args.epaper = &epaper;
    ui_task_args.screen = &screen;
    ui_task_args.store = &store;
    ui_task_args.shared_state = &shared_ui_state;
    xTaskCreate(ui_task, "ui", 4096, &ui_task_args, 1, &store.ui_task);

    // Connect to wifi and launch watcher
    launch_wifi(&config, &store);

    // Connect to home assistant
    hass_task_args.config = &config;
    hass_task_args.store = &store;
    xTaskCreate(home_assistant_task, "home_assistant", 8192, &hass_task_args, 1, &store.home_assistant_task);

    // Launch touch task
    touch_task_args.bbct = &bbct;
    touch_task_args.screen = &screen;
    touch_task_args.state = &shared_ui_state;
    touch_task_args.store = &store;
    xTaskCreate(touch_task, "touch", 4096, &touch_task_args, 1, nullptr);

    if (HOME_BUTTON_PIN >= 0) {
        if (HOME_BUTTON_ACTIVE_LOW) {
            pinMode(HOME_BUTTON_PIN, INPUT_PULLUP);
        } else {
            pinMode(HOME_BUTTON_PIN, INPUT);
        }
        attachInterrupt(digitalPinToInterrupt(HOME_BUTTON_PIN), home_button_isr,
                        HOME_BUTTON_ACTIVE_LOW ? FALLING : RISING);
#if defined(ENABLE_PM_LIGHT_SLEEP)
        gpio_wakeup_enable(static_cast<gpio_num_t>(HOME_BUTTON_PIN),
                           HOME_BUTTON_ACTIVE_LOW ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
#endif
    }

#if defined(ENABLE_PM_LIGHT_SLEEP)
    // Allow GPIO interrupts (touch INT, home button) to wake the SoC from
    // automatic light sleep. Per-pin level config is set where each pin is
    // registered.
    gpio_wakeup_enable(static_cast<gpio_num_t>(TOUCH_INT), GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
#endif
}

void loop() {
    // Block until either the 1 s housekeeping tick elapses or an ISR (home button)
    // wakes us. This lets the chip enter light sleep instead of busy-polling.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

    wifi_poll();

    if (s_home_button_pending) {
        s_home_button_pending = false;
        ESP_LOGI(TAG, "Home button pressed");
        store_go_home(&store);
    }
}
