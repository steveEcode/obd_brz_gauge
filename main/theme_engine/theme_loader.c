#include "theme_interface.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_err.h"
#include "cJSON.h"
#include <string.h>
#include "bsp_obd_dsp/nvs_storage.h"
#include "export_path/ui_theme.h"

#define TAG "theme_engine"
#define THEME_MANIFEST_MAX_SIZE  (16 * 1024)  // 16KB for JSON manifest

// Internal theme context
typedef struct {
    bool loaded;
    uint8_t slot;
    const esp_partition_t *partition;
    cJSON *manifest;

    // Theme metadata
    theme_info_t info;

    // Memory-mapped assets
    const void *dial_data;
    spi_flash_mmap_handle_t dial_handle;
    const void *ring_data;
    spi_flash_mmap_handle_t ring_handle;

    // LVGL image descriptors
    lv_img_dsc_t dial_img;
    lv_img_dsc_t ring_img;

    // Color palette (8 themed colors)
    uint32_t colors[UI_COLOR__COUNT];

    // Page registry
    theme_page_entry_t pages[32];
    uint8_t page_count;

    // Data bindings (for theme_update_data)
    struct {
        lv_obj_t *widget;
        char data_source[32];  // "obd.boost" / "obd.rpm"
        void (*update_fn)(lv_obj_t*, int32_t);
    } bindings[64];
    uint8_t binding_count;
} theme_context_t;

static theme_context_t s_ctx = {0};

// Forward declarations
static esp_err_t theme_load_default(void);
static esp_err_t theme_parse_manifest(void);
static esp_err_t theme_load_assets(void);
static void theme_register_pages(void);
static lv_obj_t* theme_create_custom_page(const char *page_id);

// ============================================================
//  Public API Implementation
// ============================================================

esp_err_t theme_engine_init(void) {
    ESP_LOGI(TAG, "Initializing theme engine");

    // Read theme slot from NVS
    uint8_t theme_slot = nvs_cfg_get()->theme_cfg.theme;  // 0 or 1

    esp_err_t ret = theme_load(theme_slot);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load theme from slot %d, using default", theme_slot);
        return theme_load_default();
    }

    return ESP_OK;
}

esp_err_t theme_load(uint8_t slot) {
    ESP_LOGI(TAG, "Loading theme from slot %d", slot);

    // Unload previous theme if any
    if (s_ctx.loaded) {
        theme_unload();
    }

    // Find theme partition
    const char *partition_name = (slot == 0) ? "theme_0" : "theme_1";
    s_ctx.partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_SPIFFS,
        partition_name
    );

    if (!s_ctx.partition) {
        ESP_LOGE(TAG, "Theme partition '%s' not found", partition_name);
        return ESP_ERR_NOT_FOUND;
    }

    s_ctx.slot = slot;

    // Read and parse manifest
    esp_err_t ret = theme_parse_manifest();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to parse theme manifest");
        return ret;
    }

    // Load assets (dial/ring images)
    ret = theme_load_assets();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load some assets, continuing anyway");
    }

    // Register pages (system + theme)
    theme_register_pages();

    s_ctx.loaded = true;

    ESP_LOGI(TAG, "Theme '%s' v%s loaded successfully", s_ctx.info.name, s_ctx.info.version);

    return ESP_OK;
}

esp_err_t theme_get_info(theme_info_t *info) {
    if (!s_ctx.loaded || !info) {
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(info, &s_ctx.info, sizeof(theme_info_t));
    return ESP_OK;
}

lv_obj_t* theme_create_page(const char *page_id) {
    if (!s_ctx.loaded) {
        ESP_LOGW(TAG, "Theme not loaded, cannot create page '%s'", page_id);
        return NULL;
    }

    // CRITICAL: Boot pages are NEVER themed, always use system implementation
    // These ensure consistent Sky Gauge branding and boot experience
    if (strcmp(page_id, "logo") == 0 ||
        strcmp(page_id, "intro") == 0 ||
        strcmp(page_id, "boot_video") == 0) {
        ESP_LOGI(TAG, "Page '%s' is a protected boot page, using system implementation", page_id);
        // Return NULL here - caller should use original ui_ScreenPageLogo_screen_init()
        return NULL;
    }

    // Search page registry
    for (int i = 0; i < s_ctx.page_count; i++) {
        if (strcmp(s_ctx.pages[i].page_id, page_id) == 0) {
            ESP_LOGI(TAG, "Creating page '%s' (type: %s)",
                     page_id,
                     s_ctx.pages[i].type == THEME_PAGE_TYPE_SYSTEM ? "system" : "theme");
            if (s_ctx.pages[i].type == THEME_PAGE_TYPE_THEME) {
                return theme_create_custom_page(page_id);
            }
            return s_ctx.pages[i].create_fn ? s_ctx.pages[i].create_fn() : NULL;
        }
    }

    ESP_LOGW(TAG, "Page '%s' not found in registry", page_id);
    return NULL;
}

void theme_update_data(const obd_snapshot_t *obd) {
    if (!s_ctx.loaded || !obd) {
        return;
    }

    // Update all registered data bindings
    for (int i = 0; i < s_ctx.binding_count; i++) {
        if (!s_ctx.bindings[i].widget || !s_ctx.bindings[i].update_fn) {
            continue;
        }

        int32_t value = 0;
        const char *src = s_ctx.bindings[i].data_source;

        // Map data source to value
        if (strcmp(src, "obd.rpm") == 0) {
            value = obd->rpm;
        } else if (strcmp(src, "obd.speed") == 0) {
            value = obd->speed;
        } else if (strcmp(src, "obd.boost") == 0) {
            value = obd->boost;
        } else if (strcmp(src, "obd.coolant_temp") == 0) {
            value = obd->coolant_temp;
        } else if (strcmp(src, "obd.oil_pressure") == 0) {
            value = obd->oil_pressure;
        } else if (strcmp(src, "obd.gear") == 0) {
            value = obd->gear;
        } else if (strcmp(src, "obd.battery_voltage") == 0) {
            value = obd->battery_voltage;
        } else if (strcmp(src, "obd.oil_temp") == 0) {
            value = obd->oil_temp;
        } else if (strcmp(src, "obd.afr") == 0) {
            value = obd->afr;
        } else if (strcmp(src, "obd.throttle") == 0) {
            value = obd->throttle;
        }

        // Call update function
        s_ctx.bindings[i].update_fn(s_ctx.bindings[i].widget, value);
    }
}

lv_color_t theme_get_color(uint8_t role) {
    if (role >= UI_COLOR__COUNT) {
        return lv_color_black();
    }

    uint32_t rgb = s_ctx.colors[role];
    return lv_color_hex(rgb);
}

const lv_img_dsc_t* theme_get_asset(const char *asset_name) {
    if (!s_ctx.loaded) {
        return NULL;
    }

    if (strcmp(asset_name, "dial") == 0 && s_ctx.dial_data) {
        return &s_ctx.dial_img;
    } else if (strcmp(asset_name, "ring") == 0 && s_ctx.ring_data) {
        return &s_ctx.ring_img;
    }

    return NULL;
}

bool theme_has_page(const char *page_id) {
    for (int i = 0; i < s_ctx.page_count; i++) {
        if (strcmp(s_ctx.pages[i].page_id, page_id) == 0) {
            return s_ctx.pages[i].type == THEME_PAGE_TYPE_THEME;
        }
    }
    return false;
}

void theme_unload(void) {
    if (!s_ctx.loaded) {
        return;
    }

    ESP_LOGI(TAG, "Unloading theme '%s'", s_ctx.info.name);

    // Unmap assets
    if (s_ctx.dial_handle) {
        spi_flash_munmap(s_ctx.dial_handle);
        s_ctx.dial_handle = 0;
        s_ctx.dial_data = NULL;
    }

    if (s_ctx.ring_handle) {
        spi_flash_munmap(s_ctx.ring_handle);
        s_ctx.ring_handle = 0;
        s_ctx.ring_data = NULL;
    }

    // Free manifest
    if (s_ctx.manifest) {
        cJSON_Delete(s_ctx.manifest);
        s_ctx.manifest = NULL;
    }

    // Clear context
    memset(&s_ctx, 0, sizeof(s_ctx));
}

// ============================================================
//  Internal Helper Functions
// ============================================================

static esp_err_t theme_load_default(void) {
    ESP_LOGI(TAG, "Loading default theme");

    // Set default metadata
    strcpy(s_ctx.info.id, "default");
    strcpy(s_ctx.info.name, "DEFAULT");
    strcpy(s_ctx.info.version, "1.0.0");
    strcpy(s_ctx.info.author, "builtin");

    // Use default colors from existing theme system
    const ui_theme_t *default_theme = ui_theme_get(0);  // slot 0 = default
    for (int i = 0; i < UI_COLOR__COUNT; i++) {
        s_ctx.colors[i] = default_theme->colors[i];
    }

    // Register only system pages (no theme pages)
    theme_register_pages();

    s_ctx.loaded = true;

    ESP_LOGI(TAG, "Default theme loaded");
    return ESP_OK;
}

static esp_err_t theme_parse_manifest(void) {
    // Allocate buffer for manifest JSON
    char *json_buf = heap_caps_malloc(THEME_MANIFEST_MAX_SIZE, MALLOC_CAP_SPIRAM);
    if (!json_buf) {
        ESP_LOGE(TAG, "Failed to allocate manifest buffer");
        return ESP_ERR_NO_MEM;
    }

    // Read manifest from partition (first 16KB)
    esp_err_t ret = esp_partition_read(s_ctx.partition, 0, json_buf, THEME_MANIFEST_MAX_SIZE);
    if (ret != ESP_OK) {
        free(json_buf);
        return ret;
    }

    // Parse JSON
    s_ctx.manifest = cJSON_Parse(json_buf);
    free(json_buf);

    if (!s_ctx.manifest) {
        ESP_LOGE(TAG, "Failed to parse theme manifest JSON");
        return ESP_ERR_INVALID_ARG;
    }

    // Extract theme metadata
    cJSON *theme_obj = cJSON_GetObjectItem(s_ctx.manifest, "theme");
    if (theme_obj) {
        cJSON *id = cJSON_GetObjectItem(theme_obj, "id");
        cJSON *name = cJSON_GetObjectItem(theme_obj, "name");
        cJSON *version = cJSON_GetObjectItem(theme_obj, "version");
        cJSON *author = cJSON_GetObjectItem(theme_obj, "author");

        if (id && cJSON_IsString(id)) {
            strncpy(s_ctx.info.id, id->valuestring, sizeof(s_ctx.info.id) - 1);
        }
        if (name && cJSON_IsString(name)) {
            strncpy(s_ctx.info.name, name->valuestring, sizeof(s_ctx.info.name) - 1);
        }
        if (version && cJSON_IsString(version)) {
            strncpy(s_ctx.info.version, version->valuestring, sizeof(s_ctx.info.version) - 1);
        }
        if (author && cJSON_IsString(author)) {
            strncpy(s_ctx.info.author, author->valuestring, sizeof(s_ctx.info.author) - 1);
        }
    }

    // Extract color palette
    cJSON *colors = cJSON_GetObjectItem(s_ctx.manifest, "colors");
    if (colors) {
        const char *color_names[] = {
            "bg", "ring", "arc_track", "arc_indicator",
            "text_primary", "text_secondary", "needle", "panel"
        };

        for (int i = 0; i < UI_COLOR__COUNT; i++) {
            cJSON *color = cJSON_GetObjectItem(colors, color_names[i]);
            if (color && cJSON_IsString(color)) {
                // Parse hex string "0xRRGGBB"
                s_ctx.colors[i] = strtoul(color->valuestring, NULL, 16);
            }
        }
    }

    ESP_LOGI(TAG, "Manifest parsed: id=%s, name=%s, version=%s",
             s_ctx.info.id, s_ctx.info.name, s_ctx.info.version);

    return ESP_OK;
}

static esp_err_t theme_load_assets(void) {
    cJSON *assets = cJSON_GetObjectItem(s_ctx.manifest, "assets");
    if (!assets) {
        ESP_LOGW(TAG, "No assets section in manifest");
        return ESP_OK;  // Not an error, theme may have no assets
    }

    // Load dial background
    cJSON *dial = cJSON_GetObjectItem(assets, "dial_background");
    if (dial) {
        cJSON *offset_obj = cJSON_GetObjectItem(dial, "offset");
        cJSON *size_obj = cJSON_GetObjectItem(dial, "size");

        if (offset_obj && size_obj) {
            size_t offset = offset_obj->valueint;
            size_t size = size_obj->valueint;

            esp_err_t ret = esp_partition_mmap(
                s_ctx.partition, offset, size,
                ESP_PARTITION_MMAP_DATA,
                &s_ctx.dial_data, &s_ctx.dial_handle
            );

            if (ret == ESP_OK) {
                s_ctx.dial_img.header.w = 360;
                s_ctx.dial_img.header.h = 360;
                s_ctx.dial_img.header.cf = LV_IMG_CF_TRUE_COLOR;
                s_ctx.dial_img.data = (uint8_t*)s_ctx.dial_data;
                s_ctx.dial_img.data_size = size;
                ESP_LOGI(TAG, "Dial background loaded: %zu bytes @ 0x%zx", size, offset);
            } else {
                ESP_LOGW(TAG, "Failed to mmap dial background: %s", esp_err_to_name(ret));
            }
        }
    }

    // Load ring overlay (similar logic)
    cJSON *ring = cJSON_GetObjectItem(assets, "ring_overlay");
    if (ring) {
        cJSON *offset_obj = cJSON_GetObjectItem(ring, "offset");
        cJSON *size_obj = cJSON_GetObjectItem(ring, "size");

        if (offset_obj && size_obj) {
            size_t offset = offset_obj->valueint;
            size_t size = size_obj->valueint;

            esp_err_t ret = esp_partition_mmap(
                s_ctx.partition, offset, size,
                ESP_PARTITION_MMAP_DATA,
                &s_ctx.ring_data, &s_ctx.ring_handle
            );

            if (ret == ESP_OK) {
                s_ctx.ring_img.header.w = 360;
                s_ctx.ring_img.header.h = 360;
                s_ctx.ring_img.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
                s_ctx.ring_img.data = (uint8_t*)s_ctx.ring_data;
                s_ctx.ring_img.data_size = size;
                ESP_LOGI(TAG, "Ring overlay loaded: %zu bytes @ 0x%zx", size, offset);
            } else {
                ESP_LOGW(TAG, "Failed to mmap ring overlay: %s", esp_err_to_name(ret));
            }
        }
    }

    return ESP_OK;
}

static void theme_register_pages(void) {
    s_ctx.page_count = 0;

    // TODO: Register system pages (settings/OTA/bluetooth/etc.)
    // This will be implemented when integrating with existing UI

    // Register theme pages declared in the manifest (Phase 2: placeholder page only)
    if (s_ctx.manifest) {
        cJSON *pages = cJSON_GetObjectItem(s_ctx.manifest, "pages");
        cJSON *theme_pages = pages ? cJSON_GetObjectItem(pages, "theme_pages") : NULL;
        cJSON *page = NULL;
        cJSON_ArrayForEach(page, theme_pages) {
            if (s_ctx.page_count >= (int)(sizeof(s_ctx.pages) / sizeof(s_ctx.pages[0]))) {
                ESP_LOGW(TAG, "Too many theme pages, ignoring the rest");
                break;
            }
            cJSON *id = cJSON_GetObjectItem(page, "id");
            if (!id || !cJSON_IsString(id)) {
                continue;
            }
            // Protected boot pages can never be declared as theme pages
            if (strcmp(id->valuestring, "logo") == 0 ||
                strcmp(id->valuestring, "intro") == 0 ||
                strcmp(id->valuestring, "boot_video") == 0) {
                ESP_LOGW(TAG, "Theme tried to override protected page '%s', ignoring", id->valuestring);
                continue;
            }
            theme_page_entry_t *entry = &s_ctx.pages[s_ctx.page_count++];
            entry->page_id = id->valuestring;
            entry->type = THEME_PAGE_TYPE_THEME;
            entry->create_fn = NULL;  // custom pages are built by theme_create_page() directly
        }
    }

    ESP_LOGI(TAG, "Registered %d pages", s_ctx.page_count);
}

static lv_obj_t* theme_create_custom_page(const char *page_id) {
    // TODO: Implement custom page creation from layout data
    // For now, return a placeholder

    lv_obj_t *page = lv_obj_create(NULL);
    lv_obj_set_size(page, 360, 360);

    // Apply theme background color
    lv_obj_set_style_bg_color(page, theme_get_color(UI_COLOR_BG), 0);

    // Draw dial background if available
    const lv_img_dsc_t *dial = theme_get_asset("dial");
    if (dial) {
        lv_obj_t *bg_img = lv_img_create(page);
        lv_img_set_src(bg_img, dial);
        lv_obj_align(bg_img, LV_ALIGN_CENTER, 0, 0);
    }

    ESP_LOGI(TAG, "Created custom page '%s' (placeholder)", page_id);

    return page;
}
