#pragma once

// Hold an ESP_PM_NO_LIGHT_SLEEP lock during e-paper refresh sequences.
// Light sleep would otherwise let the SoC nap mid-refresh, collapsing the
// EPDIY high-voltage rails and progressively darkening the panel. CPU
// freq scaling (DFS) still runs while the lock is held — the lock only
// blocks the auto light-sleep transition. All four functions are no-ops
// when neither ENABLE_PM_DFS nor ENABLE_PM_LIGHT_SLEEP is defined.

void pm_lock_init();
void pm_lock_acquire_refresh();
void pm_lock_release_refresh();

struct PmRefreshGuard {
    PmRefreshGuard() { pm_lock_acquire_refresh(); }
    ~PmRefreshGuard() { pm_lock_release_refresh(); }
    PmRefreshGuard(const PmRefreshGuard&) = delete;
    PmRefreshGuard& operator=(const PmRefreshGuard&) = delete;
};
