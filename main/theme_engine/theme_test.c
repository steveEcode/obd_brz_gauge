#include "theme_interface.h"
#include "esp_log.h"

#define TAG "theme_test"

/**
 * Test function to verify theme engine integration
 * Call this from ui_init() to see theme system status
 */
void theme_engine_test(void) {
    ESP_LOGI(TAG, "=== Theme Engine Test ===");

    // Get theme info
    theme_info_t info;
    esp_err_t ret = theme_get_info(&info);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "  Theme ID: %s", info.id);
        ESP_LOGI(TAG, "  Name: %s", info.name);
        ESP_LOGI(TAG, "  Version: %s", info.version);
        ESP_LOGI(TAG, "  Author: %s", info.author);
    } else {
        ESP_LOGE(TAG, "  Failed to get theme info");
        return;
    }

    // Test color access
    ESP_LOGI(TAG, "  Colors:");
    for (int i = 0; i < UI_COLOR__COUNT; i++) {
        lv_color_t color = theme_get_color(i);
        ESP_LOGI(TAG, "    Role %d: 0x%06X", i, lv_color_to32(color) & 0xFFFFFF);
    }

    // Test asset access
    const lv_img_dsc_t *dial = theme_get_asset("dial");
    const lv_img_dsc_t *ring = theme_get_asset("ring");
    ESP_LOGI(TAG, "  Assets:");
    ESP_LOGI(TAG, "    dial: %s", dial ? "available" : "not available");
    ESP_LOGI(TAG, "    ring: %s", ring ? "available" : "not available");

    // Test protected pages
    ESP_LOGI(TAG, "  Protected pages:");
    ESP_LOGI(TAG, "    logo: can theme = %s", theme_has_page("logo") ? "YES (ERROR!)" : "NO (correct)");
    ESP_LOGI(TAG, "    intro: can theme = %s", theme_has_page("intro") ? "YES (ERROR!)" : "NO (correct)");
    ESP_LOGI(TAG, "    boot_video: can theme = %s", theme_has_page("boot_video") ? "YES (ERROR!)" : "NO (correct)");

    ESP_LOGI(TAG, "=== Theme Engine Test Complete ===");
}
