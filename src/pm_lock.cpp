#include "pm_lock.h"

#if defined(ENABLE_PM_LIGHT_SLEEP) || defined(ENABLE_PM_DFS)

#include <esp_log.h>
#include <esp_pm.h>

static const char* TAG = "pm_lock";
static esp_pm_lock_handle_t s_refresh_lock = nullptr;

void pm_lock_init() {
    if (s_refresh_lock != nullptr) {
        return;
    }
    esp_err_t err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "epaper", &s_refresh_lock);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_lock_create failed: %s", esp_err_to_name(err));
        s_refresh_lock = nullptr;
    }
}

void pm_lock_acquire_refresh() {
    if (s_refresh_lock != nullptr) {
        esp_pm_lock_acquire(s_refresh_lock);
    }
}

void pm_lock_release_refresh() {
    if (s_refresh_lock != nullptr) {
        esp_pm_lock_release(s_refresh_lock);
    }
}

#else // No PM build flags — keep symbols available but do nothing.

void pm_lock_init() {}
void pm_lock_acquire_refresh() {}
void pm_lock_release_refresh() {}

#endif
