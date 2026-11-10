#pragma once
// ============================================================
//  theme_interface.h — Theme Engine Public Interface
//
//  The theme system separates UI presentation from core logic:
//    - Core firmware provides data (signal_store, contracts/signals.json)
//    - the selected SD theme provides UI (editor theme.bin: pages / layouts / assets)
//    - SD /OBD/THEMES/<key>/sidecar.json provides rules + overlay fields
//      extracted from the editor's <id>.gaugeproj.json
//
//  System pages (settings/OTA/bluetooth/...) remain in core firmware.
//  System pages and the independent SD boot animation stay in core firmware.
// ============================================================

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define THEME_ID_LEN    64   /* max id length in chars (editor limit), without the NUL */
#define THEME_NAME_LEN  64

// Theme metadata (theme.bin manifest)
typedef struct {
    char id[THEME_ID_LEN + 1];
    char name[THEME_NAME_LEN];
    char version[16];
    char author[48];
} theme_info_t;

/**
 * Initialize theme engine and load the selected SD theme.
 * Called once at boot, before UI initialization.
 * @return ESP_OK on success, ESP_ERR_* when no usable theme (built-in default UI is used)
 */
esp_err_t theme_engine_init(void);

/** Reload the selected SD package (after an install). Caller must have deleted every theme page object first. */
esp_err_t theme_load(uint8_t slot);

/** @return ESP_OK if a theme is loaded, ESP_ERR_INVALID_STATE if none */
esp_err_t theme_get_info(theme_info_t *info);

/**
 * Create a theme page by id. Returns a new screen object or NULL.
 * The returned screen is owned by the caller; delete it (lv_obj_del) when
 * navigating away — the engine hooks LV_EVENT_DELETE and releases per-page
 * rule/binding state and its converted images there.
 */
lv_obj_t *theme_create_page(const char *page_id);

/** Staged page creation for boot and theme-page navigation. Begin/finish require the LVGL task or mutex.
 * Begin captures an owned layout/rules snapshot. Run reads and converts images
 * using an isolated cache, without LVGL or the theme lock. After run returns,
 * finish adopts the result and creates widgets, or discards a stale/failed job.
 * Finish also cancels a job that was never run. Never finish while run executes.
 * Load/unload refuses until finish releases the job's hold on the SD package.
 */
typedef struct theme_page_prepared theme_page_prepared_t;
theme_page_prepared_t *theme_page_prepare_begin(const char *page_id);
void theme_page_prepare_run(theme_page_prepared_t *job);
lv_obj_t *theme_page_prepare_finish(theme_page_prepared_t *job);

/** Dispose without creating widgets (e.g. navigation was cancelled). Same
 * LVGL ownership requirement as finish; never call while run executes. */
void theme_page_prepare_discard(theme_page_prepared_t *job);

/** Number of theme-declared pages (manifest.pages.theme_pages[]), manifest order. */
uint8_t theme_page_list_count(void);

/** Page id at index (0-based) or NULL. Valid until the next theme_load()/theme_unload(). */
const char *theme_page_list_at(uint8_t index);

/**
 * Index of the page the UI should open first: the page with id "main_gauge"
 * when present, else 0. Only meaningful when theme_page_list_count() > 0.
 */
uint8_t theme_page_default_index(void);

/**
 * Per-frame update of the live theme page (call from the LVGL task, normally ~16 ms):
 * reads signal_store once, updates bindings (only changed values touch LVGL),
 * then evaluates sidecar rules on the same snapshot. Unchanged resolved rules
 * and alarm suppression reuse the applied look; changes apply the diff.
 * No-op when no theme page is on screen.
 */
void theme_tick(void);

/** Resolve the standard OBD signals used by a live theme page's bindings/rules.
 * Returns false only when the theme lock is busy; on false, `mask` is untouched. */
bool theme_page_obd_demand_mask(lv_obj_t *screen, uint16_t *mask);

/**
 * Gauge sweep (boot / connect animation): ratio in [0,1] drives every binding
 * to min + ratio * (max - min) of its own display range; < 0 turns it off.
 * While active, all standard `obd.*` signals are simulated from the same sweep
 * ratio for rule evaluation, including change triggers. Vehicle-specific `veh.*`
 * signals remain real because their profile ranges are not uniform.
 */
void theme_set_sweep(float ratio, uint16_t rpm);

/**
 * System alarms (RPM flash, threshold red) take priority over theme rules:
 * while suppressed, theme_tick() keeps bindings live but skips rule styling
 * and the background color is left to the caller.
 */
void theme_set_rules_suppressed(bool suppressed);

/**
 * Release converted images / caches (before OTA, theme switch, clearing the SD selection).
 * Theme page objects must already be deleted.
 */
void theme_release_resources(void);

/** Unload current theme and free all resources. */
void theme_unload(void);

/* ------------------------------------------------------------
 *  Sidecar (rules + overlay) — see docs/THEMES.md
 * ------------------------------------------------------------ */

/**
 * Install a sidecar. `json` is either the reduced sidecar produced by the
 * device web page, or a full <id>.gaugeproj.json (assets/fonts are skipped by
 * a streaming filter). Stored on SD at
 * /OBD/THEMES/<storage_theme_key(theme_id)>/sidecar.json. `report` (optional) receives a short human-readable pairing
 * report (matched / missing pages and elements, truncated rules).
 * Takes effect on the next theme page build.
 */
esp_err_t theme_sidecar_install(const char *json, size_t len, char *report, size_t report_len);

/** Install a sidecar from a file (SD import); same semantics as above. */
esp_err_t theme_sidecar_install_file(const char *path, char *report, size_t report_len);

/** Remove the sidecar of theme_id (NULL = current theme). */
esp_err_t theme_sidecar_remove(const char *theme_id);

/** True when the current theme has a paired sidecar. */
bool theme_sidecar_present(void);

#ifdef __cplusplus
}
#endif
