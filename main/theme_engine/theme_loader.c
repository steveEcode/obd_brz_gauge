#include "theme_interface.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_err.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "bsp_obd_dsp/nvs_storage.h"
#include "export_path/ui_theme.h"
#include "src/misc/lv_fs.h"

#define TAG "theme_engine"
#define THEME_MANIFEST_MAX_SIZE  (16 * 1024)  // 16KB for JSON manifest

// Exact-match schema version this firmware's parser understands. No
// range/semver comparison — the format has only ever been "1.0", and a
// mismatch (older or newer) means this loader may not understand new
// element types the manifest relies on, so it's safer to refuse and fall
// back to the default theme than to silently misrender.
#define THEME_SCHEMA_VERSION_SUPPORTED "1.0"

// Widget kinds a layout.json element can bind live OBD data to. Kept as an
// enum + shared params struct (instead of a per-widget-type function
// pointer) because label/arc/bar need different LVGL setter calls and
// different extra parameters (format string, divisor, range) — a single
// void(*)(lv_obj_t*, int32_t) signature can't carry that.
typedef enum {
    BINDING_KIND_LABEL,   // lv_label_set_text_fmt with printf-style format + divisor
    BINDING_KIND_ARC,     // lv_arc_set_value, raw value clamped to [range_min, range_max]
    BINDING_KIND_BAR,     // lv_bar_set_value, raw value clamped to [range_min, range_max]
} theme_binding_kind_t;

typedef struct {
    lv_obj_t *widget;
    char data_source[24];      // "obd.boost" / "obd.rpm" / ...
    theme_binding_kind_t kind;
    int32_t range_min;
    int32_t range_max;
    int32_t divisor;           // label only: value/divisor is split into whole.fraction
    char format[24];           // label only: printf-style, e.g. "%d.%02d" or "%d PSI"
    int32_t anchor_x;          // label only: design x anchor, re-applied after every
                                // text update since center/right alignment depends on
                                // the newly rendered width, not the "--" placeholder's
    uint8_t align;              // label only: 0=left, 1=center, 2=right
} theme_binding_t;

// A single named, memory-mapped image asset from the manifest's "assets"
// object. dial_background/ring_overlay are also mapped into this table (in
// addition to the dedicated dial_img/ring_img fields, which stay for
// theme_get_asset()'s "dial"/"ring" back-compat lookup) so "image" layout
// elements can address them the same way as any other imported image.
#define THEME_MAX_NAMED_ASSETS 16
typedef struct {
    char name[32];
    const void *data;
    esp_partition_mmap_handle_t handle;
    lv_img_dsc_t img;
} theme_named_asset_t;

// A compiled LVGL binary font ("lv_font_bin" asset), mmap'd from flash like
// any other named asset. Kept separate from theme_named_asset_t because it's
// read through lv_fs (see theme_font_fs_*) rather than handed to LVGL as a
// raw lv_img_dsc_t pointer.
#define THEME_MAX_NAMED_FONTS 16
typedef struct {
    char name[40];
    const void *data;
    esp_partition_mmap_handle_t handle;
    size_t size;
} theme_named_font_t;

// A font actually loaded via lv_font_load(), cached by asset name so pages
// with multiple labels sharing one custom font don't reload it per-label.
typedef struct {
    char name[40];
    lv_font_t *font;
} theme_loaded_font_t;

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
    esp_partition_mmap_handle_t dial_handle;
    const void *ring_data;
    esp_partition_mmap_handle_t ring_handle;

    // LVGL image descriptors
    lv_img_dsc_t dial_img;
    lv_img_dsc_t ring_img;

    // Generic named assets (superset of dial_background/ring_overlay above,
    // keyed by whatever name pack_theme.py assigned in the manifest's
    // "assets" object) -- lets layout.json "image" elements reference any
    // imported image by name, not just the two built-in dial/ring slots.
    theme_named_asset_t named_assets[THEME_MAX_NAMED_ASSETS];
    uint8_t named_asset_count;

    // Compiled LVGL binary fonts (format == "lv_font_bin" in the manifest's
    // "assets" object), mmap'd the same way as named_assets above but kept
    // in a separate table since lv_font_load() reads them through the "F:"
    // lv_fs_drv_t (theme_font_fs_*) instead of being handed a raw pointer.
    theme_named_font_t named_fonts[THEME_MAX_NAMED_FONTS];
    uint8_t named_font_count;

    // Fonts actually lv_font_load()'ed so far (lazy, keyed by asset name) --
    // avoids reloading the same .bin once per label when several labels on
    // a page share one custom font. Freed in theme_unload().
    theme_loaded_font_t loaded_fonts[THEME_MAX_NAMED_FONTS];
    uint8_t loaded_font_count;

    // Color palette (8 themed colors)
    uint32_t colors[UI_COLOR__COUNT];

    // Page registry
    theme_page_entry_t pages[32];
    uint8_t page_count;

    // Ordered list of theme-declared page ids (subset of `pages` with
    // type == THEME_PAGE_TYPE_THEME, kept separately so UI navigation can
    // walk "page N of the active theme" without caring about future system
    // pages mixed into the same registry). Strings point into s_ctx.manifest,
    // valid until the next theme_load()/theme_unload().
    const char *theme_page_ids[32];
    uint8_t theme_page_count;

    // Data bindings (for theme_update_data), populated while building a
    // custom page from layout.json
    theme_binding_t bindings[32];
    uint8_t binding_count;
} theme_context_t;

static theme_context_t s_ctx = {0};

// Forward declarations
static esp_err_t theme_load_default(void);
static esp_err_t theme_parse_manifest(void);
static esp_err_t theme_load_assets(void);
static void theme_register_pages(void);
static lv_obj_t* theme_create_custom_page(const char *page_id);
static const char* theme_json_str(cJSON *obj, const char *key, const char *fallback);
static void theme_font_fs_register(void);
static const theme_named_font_t* theme_find_named_font(const char *name);
static const lv_font_t* theme_load_custom_font(const char *font_asset);

// ============================================================
//  Memory-backed lv_fs_drv_t for lv_font_load()
// ============================================================
//
// lv_font_load() reads its .bin through LVGL's virtual filesystem, not a
// raw pointer -- unlike images, which theme_find_named_asset() hands to
// LVGL directly as an lv_img_dsc_t. The bytes are already mmap'd flash
// (same as image assets), so this driver is a thin adapter: open_cb looks
// the path up in named_fonts[] and returns a small in-memory cursor,
// read/seek/tell operate purely on that cursor, close_cb frees it. No real
// file I/O and no sdkconfig change needed (lv_fs.c is unconditionally
// compiled by LVGL's own CMakeLists).
#define THEME_FONT_FS_LETTER 'F'

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
} theme_font_fs_file_t;

static void* theme_font_fs_open(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode) {
    (void)drv;
    if (mode != LV_FS_MODE_RD) {
        return NULL;  // fonts are read-only assets
    }
    const theme_named_font_t *entry = theme_find_named_font(path);
    if (!entry) {
        ESP_LOGW(TAG, "Font fs: unknown font asset '%s'", path);
        return NULL;
    }
    theme_font_fs_file_t *file = lv_mem_alloc(sizeof(theme_font_fs_file_t));
    if (!file) {
        return NULL;
    }
    file->data = (const uint8_t *)entry->data;
    file->size = entry->size;
    file->pos = 0;
    return file;
}

static lv_fs_res_t theme_font_fs_close(lv_fs_drv_t *drv, void *file_p) {
    (void)drv;
    lv_mem_free(file_p);
    return LV_FS_RES_OK;
}

static lv_fs_res_t theme_font_fs_read(lv_fs_drv_t *drv, void *file_p, void *buf, uint32_t btr, uint32_t *br) {
    (void)drv;
    theme_font_fs_file_t *file = (theme_font_fs_file_t *)file_p;
    size_t remaining = file->size - file->pos;
    size_t to_read = btr < remaining ? btr : remaining;
    memcpy(buf, file->data + file->pos, to_read);
    file->pos += to_read;
    *br = (uint32_t)to_read;
    return LV_FS_RES_OK;
}

static lv_fs_res_t theme_font_fs_seek(lv_fs_drv_t *drv, void *file_p, uint32_t pos, lv_fs_whence_t whence) {
    (void)drv;
    theme_font_fs_file_t *file = (theme_font_fs_file_t *)file_p;
    size_t base;
    switch (whence) {
        case LV_FS_SEEK_CUR: base = file->pos; break;
        case LV_FS_SEEK_END: base = file->size; break;
        default: base = 0; break;
    }
    size_t new_pos = base + pos;
    file->pos = new_pos > file->size ? file->size : new_pos;
    return LV_FS_RES_OK;
}

static lv_fs_res_t theme_font_fs_tell(lv_fs_drv_t *drv, void *file_p, uint32_t *pos_p) {
    (void)drv;
    *pos_p = (uint32_t)((theme_font_fs_file_t *)file_p)->pos;
    return LV_FS_RES_OK;
}

static void theme_font_fs_register(void) {
    static lv_fs_drv_t drv;
    lv_fs_drv_init(&drv);
    drv.letter = THEME_FONT_FS_LETTER;
    drv.open_cb = theme_font_fs_open;
    drv.close_cb = theme_font_fs_close;
    drv.read_cb = theme_font_fs_read;
    drv.seek_cb = theme_font_fs_seek;
    drv.tell_cb = theme_font_fs_tell;
    lv_fs_drv_register(&drv);
}

// ============================================================
//  Public API Implementation
// ============================================================

esp_err_t theme_engine_init(void) {
    ESP_LOGI(TAG, "Initializing theme engine");
    theme_font_fs_register();

    // Only one theme partition exists (theme_0); there is nothing to select.
    esp_err_t ret = theme_load(0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load theme from theme_0, using default");
        return theme_load_default();
    }

    return ESP_OK;
}

esp_err_t theme_load(uint8_t slot) {
    // Only slot 0 (partition "theme_0") exists. A bad/missing theme falls
    // back to the built-in default (see theme_load_default()), so a second
    // slot for A/B rollback was judged unnecessary.
    if (slot != 0) {
        ESP_LOGE(TAG, "Theme slot %d does not exist (only theme_0 is available)", slot);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "Loading theme from theme_0");

    // Unload previous theme if any
    if (s_ctx.loaded) {
        theme_unload();
    }

    s_ctx.partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
        "theme_0"
    );

    if (!s_ctx.partition) {
        ESP_LOGE(TAG, "Theme partition 'theme_0' not found");
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

// layout.json's "format" string is untrusted theme content, but gets handed
// straight to lv_label_set_text_fmt (a vsnprintf wrapper) with only integer
// arguments. A theme containing "%s" would make vsnprintf dereference an int
// as a pointer -> crash or out-of-bounds read. Only allow conversions that
// consume an int (d/i/u/x/X/o/c) plus literal '%%'; reject everything else
// (a bad theme falls back to a safe literal instead of being trusted as-is).
static bool theme_format_is_int_only(const char *fmt) {
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            continue;
        }
        p++;
        if (*p == '%') {
            continue;  // literal '%%'
        }
        // skip flags/width/precision: digits, '.', '-', '+', '0', ' '
        while (*p && (isdigit((unsigned char)*p) || *p == '.' || *p == '-' || *p == '+' || *p == ' ')) {
            p++;
        }
        if (*p == '\0') {
            return false;  // truncated conversion
        }
        switch (*p) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c':
            break;  // safe: consumes one int
        default:
            return false;  // %s, %f, %p, %n, etc. — reject
        }
    }
    return true;
}

// Resolves a "obd.<field>" data_source string against a live snapshot.
// Returns false if the source name is unrecognized (binding left untouched).
static bool theme_resolve_data_source(const obd_snapshot_t *obd, const char *src, int32_t *out_value) {
    if (strcmp(src, "obd.rpm") == 0) {
        *out_value = obd->rpm;
    } else if (strcmp(src, "obd.speed") == 0) {
        *out_value = obd->speed;
    } else if (strcmp(src, "obd.boost") == 0) {
        *out_value = obd->boost;
    } else if (strcmp(src, "obd.coolant_temp") == 0) {
        *out_value = obd->coolant_temp;
    } else if (strcmp(src, "obd.oil_pressure") == 0) {
        *out_value = obd->oil_pressure;
    } else if (strcmp(src, "obd.gear") == 0) {
        *out_value = obd->gear;
    } else if (strcmp(src, "obd.battery_voltage") == 0) {
        *out_value = obd->battery_voltage;
    } else if (strcmp(src, "obd.oil_temp") == 0) {
        *out_value = obd->oil_temp;
    } else if (strcmp(src, "obd.afr") == 0) {
        *out_value = obd->afr;
    } else if (strcmp(src, "obd.throttle") == 0) {
        *out_value = obd->throttle;
    } else {
        return false;
    }
    return true;
}

void theme_update_data(const obd_snapshot_t *obd) {
    if (!s_ctx.loaded || !obd) {
        return;
    }

    for (int i = 0; i < s_ctx.binding_count; i++) {
        theme_binding_t *bind = &s_ctx.bindings[i];
        if (!bind->widget) {
            continue;
        }

        int32_t value = 0;
        if (!theme_resolve_data_source(obd, bind->data_source, &value)) {
            continue;
        }

        switch (bind->kind) {
        case BINDING_KIND_ARC: {
            int32_t clamped = value;
            if (clamped < bind->range_min) clamped = bind->range_min;
            if (clamped > bind->range_max) clamped = bind->range_max;
            lv_arc_set_value(bind->widget, clamped);
            break;
        }
        case BINDING_KIND_BAR: {
            int32_t clamped = value;
            if (clamped < bind->range_min) clamped = bind->range_min;
            if (clamped > bind->range_max) clamped = bind->range_max;
            lv_bar_set_value(bind->widget, clamped, LV_ANIM_OFF);
            break;
        }
        case BINDING_KIND_LABEL: {
            if (bind->divisor > 1) {
                // Split e.g. 150/100 -> whole=1, frac=50, so "%d.%02d" -> "1.50"
                int32_t whole = value / bind->divisor;
                int32_t frac = value % bind->divisor;
                if (frac < 0) frac = -frac;
                lv_label_set_text_fmt(bind->widget, bind->format, whole, frac);
            } else {
                lv_label_set_text_fmt(bind->widget, bind->format, value);
            }
            if (bind->align != 0) {
                // Center/right alignment is anchored to the rendered text
                // width, which changes with every value (e.g. "5" vs "145")
                // -- theme_build_label_element() only computes this once at
                // page-build time against the "--" placeholder, so it has
                // to be redone here or the label visibly drifts left/right
                // as the OBD value's digit count changes.
                lv_obj_update_layout(bind->widget);
                int32_t w = lv_obj_get_width(bind->widget);
                int32_t new_x = (bind->align == 1) ? (bind->anchor_x - w / 2) : (bind->anchor_x - w);
                lv_obj_set_x(bind->widget, new_x);
            }
            break;
        }
        }
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

// Looks up a named asset by its exact packer-assigned name (as opposed to
// theme_get_asset()'s fixed "dial"/"ring" aliases). Used by layout.json
// "image" elements, which reference arbitrary imported images by name.
static const lv_img_dsc_t* theme_find_named_asset(const char *name) {
    for (int i = 0; i < s_ctx.named_asset_count; i++) {
        if (strcmp(s_ctx.named_assets[i].name, name) == 0) {
            return &s_ctx.named_assets[i].img;
        }
    }
    return NULL;
}

// Looks up a compiled font asset ("lv_font_bin") by its packer-assigned
// name. Used by theme_font_fs_open() (via lv_font_load()'s "F:<name>" path)
// and theme_load_custom_font() below.
static const theme_named_font_t* theme_find_named_font(const char *name) {
    for (int i = 0; i < s_ctx.named_font_count; i++) {
        if (strcmp(s_ctx.named_fonts[i].name, name) == 0) {
            return &s_ctx.named_fonts[i];
        }
    }
    return NULL;
}

// Loads (or returns the cached) lv_font_t for a custom font asset name, so
// a page with several labels sharing one custom font only pays
// lv_font_load()'s cost once. Returns NULL if the asset doesn't exist or
// fails to parse -- callers fall back to a built-in Montserrat size rather
// than failing the whole label.
static const lv_font_t* theme_load_custom_font(const char *font_asset) {
    for (int i = 0; i < s_ctx.loaded_font_count; i++) {
        if (strcmp(s_ctx.loaded_fonts[i].name, font_asset) == 0) {
            return s_ctx.loaded_fonts[i].font;
        }
    }

    if (!theme_find_named_font(font_asset)) {
        ESP_LOGW(TAG, "Custom font: unknown font asset '%s'", font_asset);
        return NULL;
    }

    char path[48];
    snprintf(path, sizeof(path), "%c:%s", THEME_FONT_FS_LETTER, font_asset);
    lv_font_t *font = lv_font_load(path);
    if (!font) {
        ESP_LOGW(TAG, "Custom font: failed to load '%s'", font_asset);
        return NULL;
    }

    if (s_ctx.loaded_font_count >= THEME_MAX_NAMED_FONTS) {
        ESP_LOGW(TAG, "Loaded-font cache full, not caching '%s' (still usable this call)", font_asset);
        return font;
    }
    theme_loaded_font_t *slot = &s_ctx.loaded_fonts[s_ctx.loaded_font_count];
    strncpy(slot->name, font_asset, sizeof(slot->name) - 1);
    slot->name[sizeof(slot->name) - 1] = '\0';
    slot->font = font;
    s_ctx.loaded_font_count++;
    return font;
}

bool theme_has_page(const char *page_id) {
    for (int i = 0; i < s_ctx.page_count; i++) {
        if (strcmp(s_ctx.pages[i].page_id, page_id) == 0) {
            return s_ctx.pages[i].type == THEME_PAGE_TYPE_THEME;
        }
    }
    return false;
}

uint8_t theme_page_list_count(void) {
    return s_ctx.theme_page_count;
}

const char* theme_page_list_at(uint8_t index) {
    if (index >= s_ctx.theme_page_count) {
        return NULL;
    }
    return s_ctx.theme_page_ids[index];
}

void theme_unload(void) {
    if (!s_ctx.loaded) {
        return;
    }

    ESP_LOGI(TAG, "Unloading theme '%s'", s_ctx.info.name);

    // Unmap every named asset. dial_handle/ring_handle (when set) are just
    // copies of a named_assets[] entry's handle -- unmap only through this
    // table so each mapping is torn down exactly once.
    for (int i = 0; i < s_ctx.named_asset_count; i++) {
        if (s_ctx.named_assets[i].handle) {
            esp_partition_munmap(s_ctx.named_assets[i].handle);
        }
    }
    s_ctx.dial_handle = 0;
    s_ctx.dial_data = NULL;
    s_ctx.ring_handle = 0;
    s_ctx.ring_data = NULL;

    // Free every lv_font_load()'ed custom font, then unmap its backing
    // memory -- same handle-per-slot lifecycle as named_assets above.
    for (int i = 0; i < s_ctx.loaded_font_count; i++) {
        if (s_ctx.loaded_fonts[i].font) {
            lv_font_free(s_ctx.loaded_fonts[i].font);
        }
    }
    for (int i = 0; i < s_ctx.named_font_count; i++) {
        if (s_ctx.named_fonts[i].handle) {
            esp_partition_munmap(s_ctx.named_fonts[i].handle);
        }
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

    // Reject a schema_version this loader doesn't understand rather than
    // risk silently misrendering element types added by a newer packer
    // (e.g. "image" elements on firmware that predates them). A missing
    // field is treated the same as a mismatch -- fail closed.
    cJSON *schema_version = cJSON_GetObjectItem(s_ctx.manifest, "schema_version");
    if (!schema_version || !cJSON_IsString(schema_version) ||
        strcmp(schema_version->valuestring, THEME_SCHEMA_VERSION_SUPPORTED) != 0) {
        ESP_LOGE(TAG, "Unsupported theme schema_version '%s' (firmware supports '%s')",
                 (schema_version && cJSON_IsString(schema_version)) ? schema_version->valuestring : "(missing)",
                 THEME_SCHEMA_VERSION_SUPPORTED);
        cJSON_Delete(s_ctx.manifest);
        s_ctx.manifest = NULL;
        return ESP_ERR_NOT_SUPPORTED;
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

// Maps a packer-assigned "format" string to the matching LVGL color format.
// Defaults to true-color (no alpha) for anything unrecognized, matching the
// original dial_background behavior.
static lv_img_cf_t theme_asset_color_format(const char *format) {
    if (format && strcmp(format, "rgba8888") == 0) {
        return LV_IMG_CF_TRUE_COLOR_ALPHA;
    }
    return LV_IMG_CF_TRUE_COLOR;
}

static esp_err_t theme_load_assets(void) {
    cJSON *assets = cJSON_GetObjectItem(s_ctx.manifest, "assets");
    if (!assets) {
        ESP_LOGW(TAG, "No assets section in manifest");
        return ESP_OK;  // Not an error, theme may have no assets
    }

    // Every entry in "assets" (not just the two original built-ins) gets
    // mmap'd into the generic named-asset table, so layout.json "image"
    // elements can reference any imported image by its packer-assigned
    // name. dial_background/ring_overlay additionally get mirrored into
    // the dedicated dial_img/ring_img fields for theme_get_asset()'s
    // existing "dial"/"ring" lookup.
    cJSON *asset = NULL;
    cJSON_ArrayForEach(asset, assets) {
        const char *name = asset->string;
        if (!name) {
            continue;
        }

        cJSON *offset_obj = cJSON_GetObjectItem(asset, "offset");
        cJSON *size_obj = cJSON_GetObjectItem(asset, "size");
        cJSON *width_obj = cJSON_GetObjectItem(asset, "width");
        cJSON *height_obj = cJSON_GetObjectItem(asset, "height");
        const char *format = theme_json_str(asset, "format", NULL);
        if (!offset_obj || !size_obj) {
            ESP_LOGW(TAG, "Asset '%s' missing offset/size, skipping", name);
            continue;
        }

        size_t offset = (size_t)offset_obj->valueint;
        size_t size = (size_t)size_obj->valueint;

        // Compiled fonts go into named_fonts[] (read via lv_fs by
        // theme_load_custom_font()), not the image asset table below.
        if (format && strcmp(format, "lv_font_bin") == 0) {
            if (s_ctx.named_font_count >= THEME_MAX_NAMED_FONTS) {
                ESP_LOGW(TAG, "Named font table full, skipping '%s'", name);
                continue;
            }
            theme_named_font_t *font_slot = &s_ctx.named_fonts[s_ctx.named_font_count];
            esp_err_t font_ret = esp_partition_mmap(
                s_ctx.partition, offset, size,
                ESP_PARTITION_MMAP_DATA,
                &font_slot->data, &font_slot->handle
            );
            if (font_ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to mmap font asset '%s': %s", name, esp_err_to_name(font_ret));
                continue;
            }
            strncpy(font_slot->name, name, sizeof(font_slot->name) - 1);
            font_slot->name[sizeof(font_slot->name) - 1] = '\0';
            font_slot->size = size;
            s_ctx.named_font_count++;
            ESP_LOGI(TAG, "Font asset '%s' loaded: %zu bytes @ 0x%zx", name, size, offset);
            continue;
        }

        if (s_ctx.named_asset_count >= THEME_MAX_NAMED_ASSETS) {
            ESP_LOGW(TAG, "Named asset table full, skipping '%s'", name);
            continue;
        }

        theme_named_asset_t *slot = &s_ctx.named_assets[s_ctx.named_asset_count];
        esp_err_t ret = esp_partition_mmap(
            s_ctx.partition, offset, size,
            ESP_PARTITION_MMAP_DATA,
            &slot->data, &slot->handle
        );
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to mmap asset '%s': %s", name, esp_err_to_name(ret));
            continue;
        }

        strncpy(slot->name, name, sizeof(slot->name) - 1);
        slot->name[sizeof(slot->name) - 1] = '\0';
        slot->img.header.w = width_obj ? width_obj->valueint : 360;
        slot->img.header.h = height_obj ? height_obj->valueint : 360;
        slot->img.header.cf = theme_asset_color_format(format);
        slot->img.data = (const uint8_t *)slot->data;
        slot->img.data_size = size;
        s_ctx.named_asset_count++;
        ESP_LOGI(TAG, "Asset '%s' loaded: %zu bytes @ 0x%zx", name, size, offset);

        // Back-compat mirror for theme_get_asset("dial"/"ring")
        if (strcmp(name, "dial_background") == 0) {
            s_ctx.dial_data = slot->data;
            s_ctx.dial_handle = slot->handle;
            s_ctx.dial_img = slot->img;
        } else if (strcmp(name, "ring_overlay") == 0) {
            s_ctx.ring_data = slot->data;
            s_ctx.ring_handle = slot->handle;
            s_ctx.ring_img = slot->img;
        }
    }

    return ESP_OK;
}

static void theme_register_pages(void) {
    s_ctx.page_count = 0;
    s_ctx.theme_page_count = 0;

    // TODO: Register system pages (settings/OTA/bluetooth/etc.)
    // This will be implemented when integrating with existing UI

    // Register theme pages declared in the manifest, in manifest order.
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

            if (s_ctx.theme_page_count < (int)(sizeof(s_ctx.theme_page_ids) / sizeof(s_ctx.theme_page_ids[0]))) {
                s_ctx.theme_page_ids[s_ctx.theme_page_count++] = id->valuestring;
            }
        }
    }

    ESP_LOGI(TAG, "Registered %d pages (%d theme pages)", s_ctx.page_count, s_ctx.theme_page_count);
}

// Parses "0xRRGGBB" / "RRGGBB" into an lv_color_t; falls back to `fallback`
// if the field is missing or not a string (keeps a malformed element from
// crashing the whole page build).
static lv_color_t theme_json_color(cJSON *obj, const char *key, lv_color_t fallback) {
    cJSON *v = cJSON_GetObjectItem(obj, key);
    if (!v || !cJSON_IsString(v)) {
        return fallback;
    }
    return lv_color_hex(strtoul(v->valuestring, NULL, 16));
}

static int32_t theme_json_int(cJSON *obj, const char *key, int32_t fallback) {
    cJSON *v = cJSON_GetObjectItem(obj, key);
    if (!v || !cJSON_IsNumber(v)) {
        return fallback;
    }
    // Use valuedouble and round to handle floating point coordinates from the designer
    return (int32_t)(v->valuedouble + 0.5);
}

static const char* theme_json_str(cJSON *obj, const char *key, const char *fallback) {
    cJSON *v = cJSON_GetObjectItem(obj, key);
    if (!v || !cJSON_IsString(v)) {
        return fallback;
    }
    return v->valuestring;
}

// Registers a binding so theme_update_data() drives this widget on every
// OBD refresh. Silently drops the binding (widget stays static) if the
// element didn't declare a data_source, or the binding table is full —
// a full table means a theme's layout.json has more live elements than we
// support per page; that's a theme-authoring problem, not a crash.
static void theme_add_binding(cJSON *elem, lv_obj_t *widget, theme_binding_kind_t kind,
                               int32_t range_min, int32_t range_max) {
    const char *src = theme_json_str(elem, "data_source", NULL);
    if (!src) {
        return;
    }
    if (s_ctx.binding_count >= (int)(sizeof(s_ctx.bindings) / sizeof(s_ctx.bindings[0]))) {
        ESP_LOGW(TAG, "Binding table full, '%s' will not update live", src);
        return;
    }

    theme_binding_t *bind = &s_ctx.bindings[s_ctx.binding_count++];
    bind->widget = widget;
    bind->kind = kind;
    bind->range_min = range_min;
    bind->range_max = range_max;
    strncpy(bind->data_source, src, sizeof(bind->data_source) - 1);
    bind->data_source[sizeof(bind->data_source) - 1] = '\0';

    bind->divisor = theme_json_int(elem, "divisor", 1);
    const char *fmt = theme_json_str(elem, "format", "%d");
    if (theme_format_is_int_only(fmt)) {
        strncpy(bind->format, fmt, sizeof(bind->format) - 1);
    } else {
        ESP_LOGW(TAG, "Rejecting unsafe format string '%s' for '%s', using \"%%d\"", fmt, src);
        strncpy(bind->format, "%d", sizeof(bind->format) - 1);
    }
    bind->format[sizeof(bind->format) - 1] = '\0';

    bind->anchor_x = theme_json_int(elem, "x", 0);
    const char *align_str = theme_json_str(elem, "align", "left");
    if (strcmp(align_str, "center") == 0) bind->align = 1;
    else if (strcmp(align_str, "right") == 0) bind->align = 2;
    else bind->align = 0;
}

static void theme_build_arc_element(lv_obj_t *parent, cJSON *elem) {
    lv_obj_t *arc = lv_arc_create(parent);

    // Enable manual positioning - ignore parent's layout management
    lv_obj_add_flag(arc, LV_OBJ_FLAG_IGNORE_LAYOUT);

    int32_t x = theme_json_int(elem, "x", 0);
    int32_t y = theme_json_int(elem, "y", 0);
    int32_t width = theme_json_int(elem, "width", 100);
    int32_t height = theme_json_int(elem, "height", 100);

    lv_obj_set_size(arc, width, height);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);

    int32_t range_min = theme_json_int(elem, "range_min", 0);
    int32_t range_max = theme_json_int(elem, "range_max", 100);
    lv_arc_set_range(arc, range_min, range_max);
    lv_arc_set_bg_angles(arc, theme_json_int(elem, "start_angle", 135), theme_json_int(elem, "end_angle", 45));
    lv_arc_set_rotation(arc, theme_json_int(elem, "rotation", 0));
    lv_arc_set_value(arc, range_min);

    lv_color_t bg_color = theme_json_color(elem, "bg_color", theme_get_color(UI_COLOR_ARC_TRACK));
    lv_color_t fg_color = theme_json_color(elem, "color", theme_get_color(UI_COLOR_ARC_INDICATOR));
    int32_t line_width = theme_json_int(elem, "line_width", 10);

    lv_obj_set_style_arc_color(arc, bg_color, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, line_width, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, fg_color, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, line_width, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
    // LVGL's default theme rounds both arc ends (arc_rounded=true) the same
    // way lv_bar defaults to a pill radius -- the designer renders square-cut
    // ends, so this must be forced off here too (same fix as theme_build_bar_element's radius override below).
    lv_obj_set_style_arc_rounded(arc, false, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, false, LV_PART_INDICATOR);

    // Remove any padding on the Arc itself
    lv_obj_set_style_pad_all(arc, 0, 0);

    // Set position AFTER all Arc config functions (they can reset position like lv_img_set_zoom did)
    lv_obj_set_pos(arc, x, y);

    theme_add_binding(elem, arc, BINDING_KIND_ARC, range_min, range_max);
}

static void theme_build_bar_element(lv_obj_t *parent, cJSON *elem) {
    lv_obj_t *bar = lv_bar_create(parent);

    // Enable manual positioning - ignore parent's layout management
    lv_obj_add_flag(bar, LV_OBJ_FLAG_IGNORE_LAYOUT);

    lv_obj_set_pos(bar, theme_json_int(elem, "x", 0), theme_json_int(elem, "y", 0));
    lv_obj_set_size(bar, theme_json_int(elem, "width", 100), theme_json_int(elem, "height", 20));

    int32_t range_min = theme_json_int(elem, "range_min", 0);
    int32_t range_max = theme_json_int(elem, "range_max", 100);
    lv_bar_set_range(bar, range_min, range_max);
    lv_bar_set_value(bar, range_min, LV_ANIM_OFF);

    lv_color_t bg_color = theme_json_color(elem, "bg_color", theme_get_color(UI_COLOR_ARC_TRACK));
    lv_color_t fg_color = theme_json_color(elem, "color", theme_get_color(UI_COLOR_ARC_INDICATOR));
    lv_obj_set_style_bg_color(bar, bg_color, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, fg_color, LV_PART_INDICATOR);

    // LVGL's default theme gives lv_bar a pill-shaped radius on both parts;
    // the designer's bars default to square corners, so this must be set
    // explicitly (0 unless the layout requests one) or the two diverge.
    int32_t radius = theme_json_int(elem, "radius", 0);
    lv_obj_set_style_radius(bar, radius, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, radius, LV_PART_INDICATOR);

    // "segments" (a stepped look, like the example theme's 10-block oil
    // pressure meter) is a purely visual grouping of the same bar — LVGL's
    // bar doesn't natively support discrete blocks, so this is intentionally
    // approximated as a continuous bar for now. Faithful block rendering
    // needs a custom draw event, left for a follow-up.

    theme_add_binding(elem, bar, BINDING_KIND_BAR, range_min, range_max);
}

static void theme_build_label_element(lv_obj_t *parent, cJSON *elem) {
    lv_obj_t *label = lv_label_create(parent);

    // Enable manual positioning - ignore parent's layout management
    lv_obj_add_flag(label, LV_OBJ_FLAG_IGNORE_LAYOUT);

    lv_color_t color = theme_json_color(elem, "color", theme_get_color(UI_COLOR_TEXT_PRIMARY));
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);

    int32_t font_size = theme_json_int(elem, "font_size", 16);
    const char *font_asset = theme_json_str(elem, "font_asset", NULL);
    const lv_font_t *font = font_asset ? theme_load_custom_font(font_asset) : NULL;
    if (!font) {
        // Falls back here both when no font_asset was set, and when a
        // font_asset was set but failed to load (missing/corrupt bin) --
        // graceful degradation, same posture as theme_load_default().
        if (font_size >= 48) font = &lv_font_montserrat_48;
        else if (font_size >= 32) font = &lv_font_montserrat_32;
        else if (font_size >= 26) font = &lv_font_montserrat_26;
        else if (font_size >= 16) font = &lv_font_montserrat_16;
        else if (font_size >= 14) font = &lv_font_montserrat_14;
        else font = &lv_font_montserrat_12;
    }
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);

    const char *data_source = theme_json_str(elem, "data_source", NULL);
    if (data_source) {
        // Live-bound label: initial text is just a placeholder until the
        // first theme_update_data() tick fills in the real value.
        theme_add_binding(elem, label, BINDING_KIND_LABEL, 0, 0);
        lv_label_set_text(label, "--");
    } else {
        // Static label: fixed text from the manifest, never updated.
        lv_label_set_text(label, theme_json_str(elem, "text", ""));
    }

    // Position: x/y is the anchor point; "center"/"right" shift the label so
    // that anchor is the label's horizontal center/right edge instead of its
    // left edge. Needs the label's rendered width, so force a layout pass
    // now rather than relying on lazy layout (which wouldn't be resolved
    // yet at this point in page construction).
    int32_t x = theme_json_int(elem, "x", 0);
    int32_t y = theme_json_int(elem, "y", 0);
    const char *align = theme_json_str(elem, "align", "left");

    // Remove any padding on the Label itself
    lv_obj_set_style_pad_all(label, 0, 0);

    lv_obj_update_layout(label);
    int32_t w = lv_obj_get_width(label);
    if (strcmp(align, "center") == 0) {
        lv_obj_set_pos(label, x - w / 2, y);
    } else if (strcmp(align, "right") == 0) {
        lv_obj_set_pos(label, x - w, y);
    } else {
        lv_obj_set_pos(label, x, y);
    }
}

// "image" element: places a named, packer-imported asset at x/y sized to
// width/height, with an optional rotation. Missing/unknown asset names are
// logged and skipped rather than failing the whole page, consistent with
// how a full binding table or an unknown element type is handled above.
static void theme_build_image_element(lv_obj_t *parent, cJSON *elem) {
    const char *asset_name = theme_json_str(elem, "asset", NULL);
    if (!asset_name) {
        ESP_LOGW(TAG, "Image element missing 'asset' field, skipping");
        return;
    }

    const lv_img_dsc_t *img = theme_find_named_asset(asset_name);
    if (!img) {
        ESP_LOGW(TAG, "Image element references unknown asset '%s', skipping", asset_name);
        return;
    }

    lv_obj_t *img_obj = lv_img_create(parent);
    lv_img_set_src(img_obj, img);

    // Enable manual positioning - ignore parent's layout management
    lv_obj_add_flag(img_obj, LV_OBJ_FLAG_IGNORE_LAYOUT);

    int32_t x = theme_json_int(elem, "x", 0);
    int32_t y = theme_json_int(elem, "y", 0);
    int32_t width = theme_json_int(elem, "width", img->header.w);
    int32_t height = theme_json_int(elem, "height", img->header.h);
    int32_t rotation = theme_json_int(elem, "rotation", 0);

    // Set size and position directly - avoid lv_img_set_zoom which resets position
    lv_obj_set_size(img_obj, width, height);
    lv_obj_set_pos(img_obj, x, y);

    // Apply rotation if needed
    if (rotation != 0) {
        lv_img_set_angle(img_obj, rotation * 10);
    }

    (void)height;  // zoom is uniform (single scale factor), height is derived from width's ratio
}

static lv_obj_t* theme_create_custom_page(const char *page_id) {
    // Clear old bindings before creating new page to prevent use-after-free
    // when theme_update_data() timer fires after the old page is deleted
    s_ctx.binding_count = 0;
    memset(s_ctx.bindings, 0, sizeof(s_ctx.bindings));

    lv_obj_t *page = lv_obj_create(NULL);
    lv_obj_set_size(page, 360, 360);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    // Apply theme background color
    lv_obj_set_style_bg_color(page, theme_get_color(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);

    // Remove default padding - LVGL objects have default padding that offsets
    // child positions. Without this, Arc and Label elements appear at wrong
    // positions even though lv_obj_set_pos() is called with correct coordinates.
    lv_obj_set_style_pad_all(page, 0, 0);

    // Draw dial background if available
    const lv_img_dsc_t *dial = theme_get_asset("dial");
    if (dial) {
        lv_obj_t *bg_img = lv_img_create(page);
        lv_img_set_src(bg_img, dial);
        // Don't use lv_obj_align - it activates alignment mode for the whole container
        // Instead, manually center it: (360-360)/2 = 0, so just set to (0,0)
        lv_obj_set_pos(bg_img, 0, 0);
    }

    // Find this page's layout data location in the manifest
    cJSON *pages = s_ctx.manifest ? cJSON_GetObjectItem(s_ctx.manifest, "pages") : NULL;
    cJSON *theme_pages = pages ? cJSON_GetObjectItem(pages, "theme_pages") : NULL;
    cJSON *page_entry = NULL;
    cJSON_ArrayForEach(page_entry, theme_pages) {
        cJSON *id = cJSON_GetObjectItem(page_entry, "id");
        if (id && cJSON_IsString(id) && strcmp(id->valuestring, page_id) == 0) {
            break;
        }
    }

    if (!page_entry) {
        ESP_LOGW(TAG, "No manifest entry for theme page '%s', showing background only", page_id);
        return page;
    }

    cJSON *offset_obj = cJSON_GetObjectItem(page_entry, "layout_data_offset");
    cJSON *size_obj = cJSON_GetObjectItem(page_entry, "layout_data_size");
    if (!offset_obj || !size_obj || !cJSON_IsNumber(offset_obj) || !cJSON_IsNumber(size_obj)) {
        ESP_LOGW(TAG, "Theme page '%s' has no layout data, showing background only", page_id);
        return page;
    }

    size_t offset = (size_t)offset_obj->valueint;
    size_t size = (size_t)size_obj->valueint;
    if (size == 0 || size > 64 * 1024) {
        ESP_LOGW(TAG, "Theme page '%s' layout_data_size %zu out of bounds, skipping", page_id, size);
        return page;
    }

    char *layout_buf = heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM);
    if (!layout_buf) {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes for layout.json", size);
        return page;
    }

    esp_err_t ret = esp_partition_read(s_ctx.partition, offset, layout_buf, size);
    layout_buf[size] = '\0';
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read layout data for '%s': %s", page_id, esp_err_to_name(ret));
        free(layout_buf);
        return page;
    }

    cJSON *layout = cJSON_Parse(layout_buf);
    free(layout_buf);
    if (!layout) {
        ESP_LOGE(TAG, "Failed to parse layout.json for '%s'", page_id);
        return page;
    }

    cJSON *bg_color = cJSON_GetObjectItem(layout, "background_color");
    if (bg_color && cJSON_IsString(bg_color)) {
        lv_obj_set_style_bg_color(page, lv_color_hex(strtoul(bg_color->valuestring, NULL, 16)), 0);
    }

    cJSON *elements = cJSON_GetObjectItem(layout, "elements");
    cJSON *elem = NULL;
    int elem_count = 0;
    cJSON_ArrayForEach(elem, elements) {
        const char *type = theme_json_str(elem, "type", NULL);
        if (!type) {
            continue;
        }
        if (strcmp(type, "arc") == 0) {
            theme_build_arc_element(page, elem);
        } else if (strcmp(type, "bar") == 0) {
            theme_build_bar_element(page, elem);
        } else if (strcmp(type, "label") == 0) {
            theme_build_label_element(page, elem);
        } else if (strcmp(type, "image") == 0) {
            theme_build_image_element(page, elem);
        } else {
            ESP_LOGW(TAG, "Unknown layout element type '%s', skipping", type);
            continue;
        }
        elem_count++;
    }

    cJSON_Delete(layout);

    ESP_LOGI(TAG, "Created custom page '%s' with %d elements, %d live bindings",
             page_id, elem_count, s_ctx.binding_count);

    return page;
}
