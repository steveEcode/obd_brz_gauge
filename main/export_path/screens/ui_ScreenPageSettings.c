// Settings Page
// Configure: default boot page, vehicle, UI theme and screen brightness.
// Theme selection is saved and applied on reboot (esp_restart).
// Swipe down (LV_DIR_BOTTOM) opens the multi-gauge page (see ui_event_settings_background).

#include "../ui.h"
#include <string.h>
#include "bsp_obd_dsp/nvs_storage.h"
#include "bsp_obd_dsp/lcd_driver/ST77916.h"
#include "app_obd_dsp/vehicle_profiles.h"
#include "esp_system.h"

// Page names for the boot-page roller. Order must match the default-page
// switch in ui.c. (Brake temp moved into the CHART page.)
// 0=TEMP 1=INFO 2=CHART 3=NEEDLE 4=GEAR 5=RPM 6=SPEED
static const char *page_names = "TEMP\nINFO\nCHART\nNEEDLE\nGEAR\nRPM\nSPEED";
#define BOOT_PAGE_COUNT 7

// Local references for settings widgets
static lv_obj_t *s_roller_page = NULL;
static lv_obj_t *s_roller_vehicle = NULL;
static lv_obj_t *s_roller_theme = NULL;
static lv_obj_t *s_slider_bright = NULL;
static lv_obj_t *s_label_bright_val = NULL;

// Callbacks
static void on_page_roller_change(lv_event_t *e)
{
    nvs_user_cfg_t cfg = *nvs_cfg_get();
    cfg.default_page = lv_roller_get_selected(s_roller_page);
    nvs_cfg_set(&cfg);
}

static void on_bright_slider_change(lv_event_t *e)
{
    int32_t val = lv_slider_get_value(s_slider_bright);
    if(val < 10) val = 10;
    lv_label_set_text_fmt(s_label_bright_val, "%ld%%", val);
    nvs_user_cfg_t cfg = *nvs_cfg_get();
    cfg.brightness_day = (uint8_t)val;
    nvs_cfg_set(&cfg);
    Set_Backlight((uint8_t)val);
}

static void on_vehicle_roller_change(lv_event_t *e)
{
    uint8_t selected = (uint8_t)lv_roller_get_selected(s_roller_vehicle);
    // vehicle_profile_set_active clamps out-of-range indices, no need here.
    nvs_user_cfg_t cfg = *nvs_cfg_get();
    cfg.vehicle_profile_idx = selected;
    nvs_cfg_set(&cfg);
    // Apply immediately so gear detection etc. picks it up without reboot.
    vehicle_profile_set_active(selected);
}

// Theme change: persist the selection, then reboot so every screen rebuilds
// with the new theme colors (screens are created once at boot).
static void on_theme_roller_change(lv_event_t *e)
{
    uint8_t selected = (uint8_t)lv_roller_get_selected(s_roller_theme);
    ui_theme_set_active(selected);   // writes theme_cfg.theme to NVS
    esp_restart();
}

void ui_ScreenPageSettings_screen_init(void)
{
    const nvs_user_cfg_t *cfg = nvs_cfg_get();

    ui_ScreenPageSettings = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageSettings, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_ScreenPageSettings, 360, LV_PART_MAIN);
    ui_helpers_style_screen_bg(ui_ScreenPageSettings);
    lv_obj_set_style_bg_opa(ui_ScreenPageSettings, 255, LV_PART_MAIN);

    // Bezel ring (color from the active theme)
    lv_obj_t *ring = ui_helpers_create_ring(ui_ScreenPageSettings, 10);

    // ====== Title ======
    lv_obj_t *title = lv_label_create(ui_ScreenPageSettings);
    lv_label_set_text(title, "SETTINGS");
    lv_obj_set_style_text_font(title, &ui_font_FontTypoderSize24, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, ui_theme_color_lv(UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -140);

    // ====== Row 1: Default Page (Boot Page) ======
    lv_obj_t *label_page = lv_label_create(ui_ScreenPageSettings);
    lv_label_set_text(label_page, "BOOT PAGE");
    lv_obj_set_style_text_font(label_page, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(label_page, ui_theme_color_lv(UI_COLOR_TEXT_SECONDARY), LV_PART_MAIN);
    lv_obj_align(label_page, LV_ALIGN_CENTER, 0, -116);

    s_roller_page = lv_roller_create(ui_ScreenPageSettings);
    lv_obj_clear_flag(s_roller_page, LV_OBJ_FLAG_GESTURE_BUBBLE); // don't trigger page swipe while scrolling
    lv_roller_set_options(s_roller_page, page_names, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(s_roller_page, 1);
    lv_roller_set_selected(s_roller_page, (cfg->default_page < BOOT_PAGE_COUNT) ? cfg->default_page : 0, LV_ANIM_OFF);
    lv_obj_set_width(s_roller_page, 140);
    ui_helpers_style_dark_roller(s_roller_page, &ui_font_FontTypoderSize20);
    lv_obj_align(s_roller_page, LV_ALIGN_CENTER, 0, -92);
    lv_obj_add_event_cb(s_roller_page, on_page_roller_change, LV_EVENT_VALUE_CHANGED, NULL);

    // ====== Row 2: Vehicle ======
    lv_obj_t *label_vehicle = lv_label_create(ui_ScreenPageSettings);
    lv_label_set_text(label_vehicle, "VEHICLE");
    lv_obj_set_style_text_font(label_vehicle, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(label_vehicle, ui_theme_color_lv(UI_COLOR_TEXT_SECONDARY), LV_PART_MAIN);
    lv_obj_align(label_vehicle, LV_ALIGN_CENTER, 0, -62);

    // Build vehicle options dynamically from the profile table (newline separated).
    uint8_t vehicle_count = 0;
    const vehicle_profile_t *vehicle_list = vehicle_profile_get_all(&vehicle_count);
    char vehicle_names[160] = {0};
    for (uint8_t i = 0; i < vehicle_count; i++) {
        if (i > 0) strlcat(vehicle_names, "\n", sizeof(vehicle_names));
        strlcat(vehicle_names, vehicle_list[i].name, sizeof(vehicle_names));
    }

    s_roller_vehicle = lv_roller_create(ui_ScreenPageSettings);
    lv_obj_clear_flag(s_roller_vehicle, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_roller_set_options(s_roller_vehicle, vehicle_names, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(s_roller_vehicle, 1);
    lv_roller_set_selected(s_roller_vehicle, (cfg->vehicle_profile_idx < vehicle_count) ? cfg->vehicle_profile_idx : 0, LV_ANIM_OFF);
    lv_obj_set_width(s_roller_vehicle, 210);   // wide enough for long names (e.g. "BMW X1 F48")
    ui_helpers_style_dark_roller(s_roller_vehicle, &ui_font_FontTypoderSize20);
    lv_obj_align(s_roller_vehicle, LV_ALIGN_CENTER, 0, -38);
    lv_obj_add_event_cb(s_roller_vehicle, on_vehicle_roller_change, LV_EVENT_VALUE_CHANGED, NULL);

    // ====== Row 3: UI Theme ======
    lv_obj_t *label_theme = lv_label_create(ui_ScreenPageSettings);
    lv_label_set_text(label_theme, "THEME");
    lv_obj_set_style_text_font(label_theme, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(label_theme, ui_theme_color_lv(UI_COLOR_TEXT_SECONDARY), LV_PART_MAIN);
    lv_obj_align(label_theme, LV_ALIGN_CENTER, 0, -8);

    // Theme options come from the generated registry, joined into an exactly
    // sized buffer — a fixed local array here used to silently truncate the
    // list once enough themes were registered.
    uint8_t theme_count = ui_theme_count();

    s_roller_theme = lv_roller_create(ui_ScreenPageSettings);
    lv_obj_clear_flag(s_roller_theme, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_roller_set_options(s_roller_theme, ui_theme_names_joined(), LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(s_roller_theme, 1);
    lv_roller_set_selected(s_roller_theme, (cfg->theme_cfg.theme < theme_count) ? cfg->theme_cfg.theme : 0, LV_ANIM_OFF);
    lv_obj_set_width(s_roller_theme, 160);
    ui_helpers_style_dark_roller(s_roller_theme, &ui_font_FontTypoderSize20);
    lv_obj_align(s_roller_theme, LV_ALIGN_CENTER, 0, 16);
    lv_obj_add_event_cb(s_roller_theme, on_theme_roller_change, LV_EVENT_VALUE_CHANGED, NULL);

    // ====== Row 4: Brightness ======
    lv_obj_t *label_bright = lv_label_create(ui_ScreenPageSettings);
    lv_label_set_text(label_bright, "BRIGHTNESS");
    lv_obj_set_style_text_font(label_bright, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(label_bright, ui_theme_color_lv(UI_COLOR_TEXT_SECONDARY), LV_PART_MAIN);
    lv_obj_align(label_bright, LV_ALIGN_CENTER, 0, 46);

    s_slider_bright = lv_slider_create(ui_ScreenPageSettings);
    lv_slider_set_range(s_slider_bright, 10, 100);
    lv_slider_set_value(s_slider_bright, cfg->brightness_day, LV_ANIM_OFF);
    lv_obj_set_width(s_slider_bright, 180);
    lv_obj_set_height(s_slider_bright, 10);
    lv_obj_align(s_slider_bright, LV_ALIGN_CENTER, 0, 68);
    lv_obj_set_style_bg_color(s_slider_bright, ui_theme_color_lv(UI_COLOR_ARC_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_slider_bright, 255, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_slider_bright, ui_theme_color_lv(UI_COLOR_ARC_INDICATOR), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_slider_bright, 255, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_slider_bright, ui_theme_color_lv(UI_COLOR_ARC_INDICATOR), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_slider_bright, 5, LV_PART_KNOB);
    lv_obj_clear_flag(s_slider_bright, LV_OBJ_FLAG_GESTURE_BUBBLE);      // avoid page swipe while dragging
    lv_obj_add_event_cb(s_slider_bright, on_bright_slider_change, LV_EVENT_VALUE_CHANGED, NULL);

    s_label_bright_val = lv_label_create(ui_ScreenPageSettings);
    lv_label_set_text_fmt(s_label_bright_val, "%d%%", cfg->brightness_day);
    lv_obj_set_style_text_font(s_label_bright_val, &ui_font_FontTypoderSize24, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_label_bright_val, ui_theme_color_lv(UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_align(s_label_bright_val, LV_ALIGN_CENTER, 0, 90);

    // ====== Hint ======
    lv_obj_t *hint = lv_label_create(ui_ScreenPageSettings);
    lv_label_set_text(hint, "Swipe down: multi-gauge\nL/R: back");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 118);

    // Black ear image at top
    lv_obj_t *ear = lv_img_create(ui_ScreenPageSettings);
    lv_img_set_src(ear, &ui_img_pngblackear_png);
    lv_obj_set_width(ear, LV_SIZE_CONTENT);
    lv_obj_set_height(ear, LV_SIZE_CONTENT);
    lv_obj_set_pos(ear, 0, -142);
    lv_obj_set_align(ear, LV_ALIGN_CENTER);
    lv_obj_add_flag(ear, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(ear, LV_OBJ_FLAG_SCROLLABLE);

    // Events - swipe to go back / down to multi-gauge
    lv_obj_move_foreground(ring);   // ring on top
    lv_obj_add_event_cb(ui_ScreenPageSettings, ui_event_settings_background, LV_EVENT_GESTURE, NULL);
}
