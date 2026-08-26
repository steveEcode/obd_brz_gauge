#pragma once
// ============================================================
//  theme_interface.h — Theme Engine Public Interface
//
//  The theme system separates UI presentation from core logic:
//    - Core firmware provides data (OBD/BLE/NVS/vehicle config)
//    - Theme partition provides UI (pages/layouts/colors/assets)
//
//  System pages (settings/OTA/bluetooth) remain in core firmware.
//  Theme pages (gauges/data visualization) load from theme partition.
// ============================================================

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
//  Data Snapshot (ABI-stable, version 1.0)
// ============================================================

// Real-time OBD data snapshot (atomic read from core firmware)
typedef struct __attribute__((packed)) {
    uint16_t rpm;              // 0-10000 RPM
    uint8_t  speed;            // 0-255 km/h
    int16_t  boost;            // -100 to +250 (in 0.01 bar, e.g. 150 = 1.50 bar)
    uint8_t  coolant_temp;     // 0-255°C
    uint8_t  oil_pressure;     // 0-255 PSI
    int8_t   gear;             // -1=R, 0=N, 1-8=forward, 127=invalid
    uint8_t  battery_voltage;  // in 0.1V (e.g. 136 = 13.6V)
    uint8_t  oil_temp;         // 0-255°C
    uint16_t afr;              // Air-fuel ratio * 100 (e.g. 1470 = 14.70)
    uint8_t  throttle;         // 0-100%
    uint8_t  _reserved[4];     // padding to 16 bytes
} obd_snapshot_t;  // 16 bytes total

// ============================================================
//  Theme Engine API
// ============================================================

typedef enum {
    THEME_PAGE_TYPE_SYSTEM,   // System page (settings/OTA/bluetooth) from core firmware
    THEME_PAGE_TYPE_THEME,    // Theme page (gauges/visualization) from theme partition
} theme_page_type_t;

typedef struct {
    const char *page_id;           // "settings" / "main_gauge" / "ota"
    theme_page_type_t type;
    lv_obj_t *(*create_fn)(void);  // Page creation function
} theme_page_entry_t;

// Theme metadata (read from theme_manifest.json)
typedef struct {
    char id[32];          // "boost_oil_v2"
    char name[32];        // "TURBO PRO"
    char version[16];     // "2.1.0"
    char author[32];      // "community/steveE"
} theme_info_t;

// ============================================================
//  Public API Functions
// ============================================================

/**
 * Initialize theme engine and load theme from NVS-selected slot
 * Called once at boot, before UI initialization
 *
 * @return ESP_OK on success, ESP_ERR_* on failure (falls back to default theme)
 */
esp_err_t theme_engine_init(void);

/**
 * Load theme from specific slot (0 or 1)
 * Used by OTA to switch themes, or for fallback on corruption
 *
 * @param slot Theme partition slot (only 0 = theme_0 exists)
 * @return ESP_OK on success, ESP_ERR_* on failure
 */
esp_err_t theme_load(uint8_t slot);

/**
 * Get currently loaded theme information
 *
 * @param info Output buffer for theme metadata
 * @return ESP_OK if theme loaded, ESP_ERR_INVALID_STATE if none
 */
esp_err_t theme_get_info(theme_info_t *info);

/**
 * Create a page by ID (routes to system or theme page based on manifest)
 * Called by UI layer during screen initialization
 *
 * NOTE: Boot pages are NEVER themed:
 *   - "logo" (ui_ScreenPageLogo) - Sky Gauge logo screen
 *   - "intro" (ui_ScreenPageIntro) - RACE AS ONE animation
 *   - Boot video playback (boot_block_player)
 * These always use core firmware implementations to ensure consistent boot experience.
 *
 * @param page_id Page identifier (e.g. "settings", "main_gauge")
 * @return LVGL object for the page, or NULL if not found
 */
lv_obj_t* theme_create_page(const char *page_id);

/**
 * Update all theme pages with new OBD data
 * Called periodically (e.g. every 50ms) by data layer
 *
 * @param obd Real-time OBD data snapshot
 */
void theme_update_data(const obd_snapshot_t *obd);

/**
 * Get themed color for a role (from current theme's color palette)
 *
 * @param role Color role (UI_COLOR_BG, UI_COLOR_RING, etc.)
 * @return LVGL color (lv_color_t)
 */
lv_color_t theme_get_color(uint8_t role);

/**
 * Get theme asset (dial/ring image descriptor)
 *
 * @param asset_name "dial" or "ring"
 * @return LVGL image descriptor, or NULL if not available
 */
const lv_img_dsc_t* theme_get_asset(const char *asset_name);

/**
 * Check if a page is provided by theme or system
 *
 * @param page_id Page identifier
 * @return true if theme provides this page, false if system page
 */
bool theme_has_page(const char *page_id);

/**
 * Unload current theme and free resources
 * Called before theme switching or system shutdown
 */
void theme_unload(void);

/**
 * Test function to verify theme engine integration
 * Prints theme info, colors, assets, and protected pages to log
 * Call from ui_init() for debugging
 */
void theme_engine_test(void);

#ifdef __cplusplus
}
#endif
