#include "boards.h"
#include "config_remote.h"
#include "constants.h"
#include "managers/battery.h"
#include "managers/home_assistant.h"
#include "managers/touch.h"
#include "managers/ui.h"
#include "managers/wifi.h"
#include <WiFi.h>
#include "pm_lock.h"
#include "store.h"
#include "ui_state.h"
#include <Arduino.h>
#include <FastEPD.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_log.h>
#include <esp_pm.h>
#include <esp_sleep.h>

static Configuration config;
static const char* TAG = "main";

static FASTEPD epaper;
static BBCapTouch bbct;
static EntityStore store;
static SharedUIState shared_ui_state;

static UITaskArgs ui_task_args;
static TouchTaskArgs touch_task_args;
static HomeAssistantTaskArgs hass_task_args;
static BatteryTaskArgs battery_task_args;

static volatile TaskHandle_t s_main_task_handle = nullptr;
static volatile bool s_home_button_pending = false;
static volatile bool s_io_expander_pending = false;
// Last-known IO48 button state (active-low on the expander). Tracked
// across INT events so we only fire the named "pressed" / "released"
// log on the actual edge.
static bool s_io48_pressed = false;
// Whether the backlight follows the IdlePhase::Active level automatically.
// Toggled by the IO48 button. When false, backlight stays LOW regardless
// of activity. Defaults off on boot so the panel stays dark until the
// user opts in via the IO48 side button.
static bool s_backlight_auto_on = false;

static void IRAM_ATTR home_button_isr() {
    s_home_button_pending = true;
    if (s_main_task_handle) {
        BaseType_t hpw = pdFALSE;
        vTaskNotifyGiveFromISR(s_main_task_handle, &hpw);
        portYIELD_FROM_ISR(hpw);
    }
}

static void IRAM_ATTR io_expander_int_isr() {
    s_io_expander_pending = true;
    if (s_main_task_handle) {
        BaseType_t hpw = pdFALSE;
        vTaskNotifyGiveFromISR(s_main_task_handle, &hpw);
        portYIELD_FROM_ISR(hpw);
    }
}

#if defined(ENABLE_DEEP_SLEEP_STANDBY)
static void enter_deep_sleep() {
    if (HOME_BUTTON_PIN < 0) {
        // No wake source on this board; do not deep sleep — it would brick
        // the device until external reset.
        return;
    }
    if (!rtc_gpio_is_valid_gpio(static_cast<gpio_num_t>(HOME_BUTTON_PIN))) {
        ESP_LOGW(TAG, "HOME_BUTTON_PIN %d is not RTC-capable; skipping deep sleep", HOME_BUTTON_PIN);
        return;
    }

    // Hold the NO_LIGHT_SLEEP lock for the full transition. Without it
    // (with ENABLE_PM_LIGHT_SLEEP on) the SoC can drop into auto light
    // sleep between fullUpdate(CLEAR_FAST) and esp_deep_sleep_start —
    // e.g. during rtc_gpio register pokes — corrupting the transition
    // and leading to a panic+reset instead of a clean sleep. No-op when
    // PM flags are disabled.
    PmRefreshGuard pm_guard;

    ESP_LOGI(TAG, "Entering deep sleep (wake on BOOT button, GPIO %d)", HOME_BUTTON_PIN);

    // Tear down network state cleanly before the radios get powered off.
    // Without this the WebSocket TCP socket dies abruptly and HA holds a
    // half-open session until its keepalive timer expires (~30-60 s).
    home_assistant_shutdown();
    WiFi.disconnect(true, false);

    // Paint the "Press to wake ->" screen right before powering down so the
    // panel ends in a clear, hint-bearing state for the duration of the sleep.
    ui_draw_standby_screen(&epaper);
    epaper.fullUpdate(CLEAR_FAST, true);
    epaper.einkPower(false);

    // Detach matrix-level ISRs on the pins we're about to touch with the
    // RTC GPIO peripheral. rtc_gpio_init momentarily drives the pin low
    // while reconfiguring the IOMUX, and a still-armed FALLING ISR will
    // fire repeatedly off that transient — gpio_isr_loop spins until
    // IWDT trips. Has to happen before rtc_gpio_init.
    detachInterrupt(digitalPinToInterrupt(HOME_BUTTON_PIN));
    if (IO_EXPANDER_INT_PIN >= 0) {
        detachInterrupt(digitalPinToInterrupt(IO_EXPANDER_INT_PIN));
    }

    const auto pin = static_cast<gpio_num_t>(HOME_BUTTON_PIN);
#if defined(ENABLE_PM_LIGHT_SLEEP)
    // Wipe every residual wake source the PM/tickless-idle path armed before
    // we hand off to ext0. ESP-IDF enables ESP_SLEEP_WAKEUP_TIMER on each
    // light-sleep entry, and loop()'s deferred-PM block calls
    // gpio_wakeup_enable on HOME_BUTTON_PIN and TOUCH_INT. Those enable bits
    // carry into deep sleep: a stale timer wakes the SoC microseconds after
    // esp_deep_sleep_start(), and a floating TOUCH_INT (once einkPower(false)
    // unpowers the GT911 below) fires GPIO wake before ext0 has a chance.
    // Gated on ENABLE_PM_LIGHT_SLEEP for symmetry with the enable side —
    // nothing arms either source without that flag.
    gpio_wakeup_disable(pin);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
#endif
    rtc_gpio_init(pin);
    rtc_gpio_set_direction(pin, RTC_GPIO_MODE_INPUT_ONLY);
    if (HOME_BUTTON_ACTIVE_LOW) {
        rtc_gpio_pullup_en(pin);
        rtc_gpio_pulldown_dis(pin);
        esp_sleep_enable_ext0_wakeup(pin, 0);
    } else {
        rtc_gpio_pullup_dis(pin);
        rtc_gpio_pulldown_en(pin);
        esp_sleep_enable_ext0_wakeup(pin, 1);
    }
    // Latch the pull-up/pull-down state through the deep-sleep transition.
    // Without this, the RTC peripheral lockdown can drop the pull-up the
    // moment sleep starts, the pin floats, ext0 sees low, and the SoC
    // wakes immediately — looks like the device "resets" right after the
    // standby screen.
    rtc_gpio_hold_en(pin);

    esp_deep_sleep_start();
}
#endif

void setup() {
    // Bluetooth is unused; release the controller so the BT phy can stay off.
    // Idempotent / safe even if BT was never initialized.
    btStop();

    // Install the GPIO ISR service explicitly while we're still single-
    // threaded. attachInterrupt() lazily installs it on first call, but
    // the lazy path has a TOCTOU between the "already installed" check
    // and the calloc that backs gpio_isr_func: when setup()'s home/IO48
    // ISR install races touch_task's TOUCH_INT install on a different
    // core, both can pass the check, both calloc, and the second
    // assignment can leave gpio_isr_func partially clobbered — the
    // crash signature is gpio_isr_handler_add deref'ing NULL inside
    // touch_task. Doing the install once here, before any xTaskCreate,
    // closes the race; later attachInterrupt calls just see "already
    // installed" and skip the install path.
    gpio_install_isr_service(0);

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
    configure_remote(&config);
    store_load_persisted_media_device_idx(&store, config.media_device_count);

    // Initialize display
    epaper.initPanel(DISPLAY_PANEL);
    epaper.setPanelSize(DISPLAY_HEIGHT, DISPLAY_WIDTH);
    epaper.setRotation(90);
    epaper.setPasses(DISPLAY_PARTIAL_UPDATE_PASSES, DISPLAY_FULL_UPDATE_PASSES);
    epaper.einkPower(true); // FIXME: Disabling power makes the GT911 unavailable

    // Launch UI task
    ui_task_args.epaper = &epaper;
    ui_task_args.store = &store;
    ui_task_args.shared_state = &shared_ui_state;
    ui_task_args.config = &config;
    xTaskCreate(ui_task, "ui", 4096, &ui_task_args, 1, &store.ui_task);

    // Connect to wifi and launch watcher
    launch_wifi(&config, &store);

    // Connect to home assistant
    hass_task_args.config = &config;
    hass_task_args.store = &store;
    xTaskCreate(home_assistant_task, "home_assistant", 8192, &hass_task_args, 1, &store.home_assistant_task);

    // Launch touch task
    touch_task_args.bbct = &bbct;
    touch_task_args.state = &shared_ui_state;
    touch_task_args.store = &store;
    touch_task_args.config = &config;
    xTaskCreate(touch_task, "touch", 4096, &touch_task_args, 1, nullptr);

    // Launch battery monitor (no-op on boards with BATTERY_ADC_PIN < 0).
    battery_task_args.store = &store;
    xTaskCreate(battery_task, "battery", 4096, &battery_task_args, 1, nullptr);

    if (HOME_BUTTON_PIN >= 0) {
        pinMode(HOME_BUTTON_PIN, HOME_BUTTON_ACTIVE_LOW ? INPUT_PULLUP : INPUT_PULLDOWN);
        attachInterrupt(digitalPinToInterrupt(HOME_BUTTON_PIN), home_button_isr,
                        HOME_BUTTON_ACTIVE_LOW ? FALLING : RISING);
#if defined(ENABLE_PM_LIGHT_SLEEP)
        gpio_wakeup_enable(static_cast<gpio_num_t>(HOME_BUTTON_PIN),
                           HOME_BUTTON_ACTIVE_LOW ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
#endif
    }

    if (BACKLIGHT_PIN >= 0) {
        // Frontlight starts off; HOME button press (loop()) turns it on
        // for a fixed window.
        pinMode(BACKLIGHT_PIN, OUTPUT);
        digitalWrite(BACKLIGHT_PIN, LOW);
    }

    // Drain any pending PCA9535 INT condition before attachInterrupt arms
    // the FALLING edge. The PCA9535 only clears INT for the input port
    // whose register is read, so we read both ports — the IO48 pin (10)
    // is on port 1 and any other input change would be on port 0. We go
    // through epaper.ioRead so FastEPD's cached register state stays
    // consistent (FastEPD also owns several output bits on this chip).
    if (EXPANDER_IO48_BIT >= 0) {
        // PCA9535 power-on default already has every pin as an input and
        // FastEPD's EPDiyV7IOInit only flips the panel-control bits to
        // outputs, so this is conceptually a no-op — but make the intent
        // explicit. ioPinMode does an atomic single-bit update of FastEPD's
        // cached config register, so it won't clobber the panel outputs.
        epaper.ioPinMode(EXPANDER_IO48_BIT, INPUT);
        const uint8_t io48_level = epaper.ioRead(EXPANDER_IO48_BIT);
        epaper.ioRead(0); // drain the other port
        s_io48_pressed = (io48_level == 0);
        ESP_LOGI(TAG, "IOExp baseline: IO48 = %d", io48_level);
    }
    if (IO_EXPANDER_INT_PIN >= 0) {
        pinMode(IO_EXPANDER_INT_PIN, INPUT_PULLUP); // INT is open-drain active-low
        attachInterrupt(digitalPinToInterrupt(IO_EXPANDER_INT_PIN), io_expander_int_isr, FALLING);
    }

#if defined(ENABLE_PM_LIGHT_SLEEP)
    // Allow GPIO interrupts (touch INT, home button) to wake the SoC from
    // automatic light sleep. Per-pin level config is set where each pin is
    // registered.
    gpio_wakeup_enable(static_cast<gpio_num_t>(TOUCH_INT), GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
#endif

#if defined(DISABLE_USB_AFTER_BOOT)
    // Production builds: stop the USB-CDC stack to drop the phy power draw and
    // avoid spurious wake events from a connected host. All ESP_LOG output
    // after this point is silently discarded.
    Serial.end();
#endif
}

void loop() {
    // 1 s housekeeping tick. ISRs (home button) and store notifications
    // still wake us early.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

    wifi_poll();

    if (s_home_button_pending) {
        s_home_button_pending = false;
        ESP_LOGI(TAG, "Home button pressed");
        store_note_interaction(&store, millis());
        store_go_home(&store);
    }

    if (s_io_expander_pending) {
        s_io_expander_pending = false;
        if (EXPANDER_IO48_BIT >= 0) {
            // Read IO48 (port 1) and drain port 0 — PCA9535 only clears
            // INT for the port whose register is read, so we have to
            // touch both or INT stays asserted and the next FALLING edge
            // never fires.
            const uint8_t io48_level = epaper.ioRead(EXPANDER_IO48_BIT);
            epaper.ioRead(0);
            const bool now_pressed = (io48_level == 0); // active-low
            if (now_pressed != s_io48_pressed) {
                s_io48_pressed = now_pressed;
                if (now_pressed) {
                    // Age tells us whether the backlight is currently in
                    // its Active pulse: if last interaction was within
                    // BACKLIGHT_PULSE_MS, the pulse is live (light is on
                    // when auto-mode is enabled). The press itself is
                    // noted as activity below, so the same idle tick will
                    // drive the panel HIGH whenever auto-mode is on.
                    const uint32_t now = millis();
                    const uint32_t age = now - store_get_last_interaction_ms(&store);
                    if (age > BACKLIGHT_PULSE_MS) {
                        // Past the pulse window — the press is a "wake"
                        // gesture, not a mode toggle. Make sure auto-mode
                        // is on so the panel actually lights up; the
                        // interaction note below does the rest.
                        if (!s_backlight_auto_on) {
                            s_backlight_auto_on = true;
                            ESP_LOGI(TAG, "IO48 pressed; backlight auto-mode ENABLED (wake)");
                        } else {
                            ESP_LOGI(TAG, "IO48 pressed; waking backlight (auto-mode still ENABLED)");
                        }
                    } else {
                        // Inside the pulse window — toggle auto-mode so
                        // the user can turn the light off (or on, if a
                        // prior press disabled it during the same window).
                        s_backlight_auto_on = !s_backlight_auto_on;
                        ESP_LOGI(TAG, "IO48 pressed; backlight auto-mode %s",
                                 s_backlight_auto_on ? "ENABLED" : "DISABLED");
                    }
                    // Treat the press itself as activity so the standby
                    // timer slides forward and (when auto-mode is on)
                    // the next idle poll lights up the panel.
                    store_note_interaction(&store, now);
                } else {
                    ESP_LOGI(TAG, "IO48 button released");
                }
            }
        } else {
            // No IO48 mapping but INT fired — drain both ports so the
            // line releases for the next edge.
            epaper.ioRead(0);
            epaper.ioRead(8);
        }
    }

    // Single idle-phase poll drives everything time-related: backlight
    // level, standby UI mode, deep-sleep entry. Phase is computed from
    // store->last_interaction_ms with network gating applied inside
    // the store.
    const IdleSnapshot idle = store_poll_idle(&store, millis());

    if (idle.entered_standby) {
        // Auto-close any modal panes the user left open so coming back
        // from standby always lands on the home screen, not a half-
        // filled Wi-Fi password, a stale battery-status page, or the
        // device picker.
        store_close_settings(&store);
        store_close_battery_status(&store);
        store_close_media_device_select(&store);
    }

    if (BACKLIGHT_PIN >= 0) {
        const bool want_on = s_backlight_auto_on && idle.phase == IdlePhase::Active;
        digitalWrite(BACKLIGHT_PIN, want_on ? HIGH : LOW);
    }

#if defined(ENABLE_DEEP_SLEEP_STANDBY)
    if (idle.phase == IdlePhase::DeepSleep) {
        ESP_LOGI(TAG, "Idle timeout reached, entering deep sleep");
        enter_deep_sleep();
    }
#endif
}
