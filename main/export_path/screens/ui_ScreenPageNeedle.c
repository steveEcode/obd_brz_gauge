// Configurable needle-style gauge page (Needle)
//  - Classic 270° lv_meter needle dial, value + unit displayed at the center
//  - Swipe down to enter the data source selection page (ui_ScreenPageNeedleConfig)
//  - Entered only via the "default boot page" on the settings page; gestures in other directions return to the main page
//  Data reading/range/refresh logic reuses the disp_item system in ui.c (ui_needle_* interfaces)

#include "../ui.h"
#include <string.h>
#include "bsp_obd_dsp/nvs_storage.h"
#include "app_obd_dsp/vehicle_profiles.h"

// Selectable data sources for the needle page (disp_item_t values); RPM(5) is deliberately excluded -- RPM already has its own page.
// Order: CLT, IAT, OIL, LOAD, TPS, SPEED, BAT, OILP, BKT
#define NEEDLE_ITEM_BOOST 10   // value of disp_item_t DISP_ITEM_BOOST
#define NEEDLE_ITEM_AFR   11   // value of disp_item_t DISP_ITEM_AFR
static const uint8_t k_needle_sources_base[] = {0, 1, 2, 3, 4, 6, 7, 8, 9, NEEDLE_ITEM_AFR};
#define NEEDLE_BASE_COUNT (sizeof(k_needle_sources_base) / sizeof(k_needle_sources_base[0]))

// At runtime, dynamically decide the selectable sources based on whether the selected vehicle profile has a turbo (BOOST is appended only with turbo)
static uint8_t s_needle_sources[NEEDLE_BASE_COUNT + 1];
static uint8_t s_needle_source_count = 0;

static lv_obj_t *s_roller_source = NULL;

static void build_needle_sources(void)
{
    s_needle_source_count = 0;
    for (uint8_t i = 0; i < NEEDLE_BASE_COUNT; i++) {
        s_needle_sources[s_needle_source_count++] = k_needle_sources_base[i];
    }
    const vehicle_profile_t *vp = vehicle_profile_get_active();
    if (vp && vp->has_boost) {
        s_needle_sources[s_needle_source_count++] = NEEDLE_ITEM_BOOST;
    }
}

// Map the data source currently stored in NVS to the roller option index
static uint16_t source_to_roller_pos(uint8_t source_idx)
{
    for (uint16_t i = 0; i < s_needle_source_count; i++) {
        if (s_needle_sources[i] == source_idx) return i;
    }
    return 0;
}

static void on_needle_source_changed(lv_event_t *e)
{
    LV_UNUSED(e);
    uint16_t pos = lv_roller_get_selected(s_roller_source);
    if (pos >= s_needle_source_count) pos = 0;

    nvs_user_cfg_t cfg = *nvs_cfg_get();
    cfg.needle_source_idx = s_needle_sources[pos];
    nvs_cfg_set(&cfg);

    // Immediately rebuild the needle page range/name/unit (the page object already exists; takes effect on next display)
    ui_needle_apply_source();
}

void ui_ScreenPageNeedle_screen_init(void)
{
    ui_ScreenPageNeedle = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageNeedle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_ScreenPageNeedle, 360, LV_PART_MAIN);
    ui_helpers_style_screen_bg(ui_ScreenPageNeedle);
    lv_obj_set_style_bg_opa(ui_ScreenPageNeedle, 255, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_ScreenPageNeedle, 0, LV_PART_MAIN);  // disable the default theme border, use a white ring instead
    lv_obj_set_style_pad_all(ui_ScreenPageNeedle, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(ui_ScreenPageNeedle, 0, LV_PART_MAIN);

    // ====== Outer white ring (consistent with other pages) ======
    lv_obj_t *ring = ui_helpers_create_ring(ui_ScreenPageNeedle, 8);   // white ring: static circular border, replaces the rotating spinner, removes the arc seam gap

    // ====== Needle dial ======
    ui_NeedleMeter = lv_meter_create(ui_ScreenPageNeedle);
    lv_obj_set_size(ui_NeedleMeter, 320, 320);
    lv_obj_center(ui_NeedleMeter);
    lv_obj_clear_flag(ui_NeedleMeter, LV_OBJ_FLAG_CLICKABLE);
    // Dial background transparent, blends into the black page
    lv_obj_set_style_bg_opa(ui_NeedleMeter, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_NeedleMeter, 0, LV_PART_MAIN);
    // Tick number font
    lv_obj_set_style_text_font(ui_NeedleMeter, &ui_font_FontTypoderSize16, LV_PART_TICKS);
    lv_obj_set_style_text_color(ui_NeedleMeter, lv_color_hex(0x666666), LV_PART_TICKS);

    ui_NeedleScale = lv_meter_add_scale(ui_NeedleMeter);
    // Dim the ticks so the needle stands out
    lv_meter_set_scale_ticks(ui_NeedleMeter, ui_NeedleScale, 21, 1, 6, ui_theme_color_lv(UI_COLOR_ARC_TRACK));
    lv_meter_set_scale_major_ticks(ui_NeedleMeter, ui_NeedleScale, 5, 2, 10, lv_color_hex(0x555555), 14);
    lv_meter_set_scale_range(ui_NeedleMeter, ui_NeedleScale, 0, 100, 270, 135); // placeholder, apply_source resets it

    // Needle: themed artwork if the active theme ships some, otherwise a drawn
    // line (thicker + longer + bright color, more eye-catching).
    // Artwork must point RIGHT at rest: lv_meter maps the value onto the scale
    // rotation and hands that straight to lv_draw_img, whose angle 0 draws the
    // bitmap unrotated (see lv_meter.c, LV_METER_INDICATOR_TYPE_NEEDLE_IMG).
    {
        const ui_theme_t *th = ui_theme_active();
        if(th->needle_img) {
            ui_NeedleIndic = lv_meter_add_needle_img(ui_NeedleMeter, ui_NeedleScale,
                                                     th->needle_img,
                                                     th->needle_pivot_x, th->needle_pivot_y);
        }
        else {
            ui_NeedleIndic = lv_meter_add_needle_line(ui_NeedleMeter, ui_NeedleScale, 10,
                                                      ui_theme_color_lv(UI_COLOR_NEEDLE), -10);
        }
    }

    // The three center labels are centered on themselves to avoid horizontal offset/overlap caused by different text lengths
    // ====== Name label (top) ======
    ui_NeedleNameLabel = lv_label_create(ui_ScreenPageNeedle);
    lv_label_set_text(ui_NeedleNameLabel, "");
    lv_obj_set_style_text_font(ui_NeedleNameLabel, &ui_font_FontTypoderSize24, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_NeedleNameLabel, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
    lv_obj_set_style_text_align(ui_NeedleNameLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(ui_NeedleNameLabel, LV_ALIGN_CENTER, 0, -52);

    // ====== Value label (bottom, in the opening at the bottom of the 270° dial; the needle never sweeps here, so no overlap) ======
    ui_NeedleValueLabel = lv_label_create(ui_ScreenPageNeedle);
    lv_label_set_text(ui_NeedleValueLabel, "--");
    lv_obj_set_style_text_font(ui_NeedleValueLabel, &ui_font_FontTypoderSize40, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_NeedleValueLabel, ui_theme_color_lv(UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_text_align(ui_NeedleValueLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(ui_NeedleValueLabel, LV_ALIGN_CENTER, 0, 54);

    // ====== Unit label (below the value) ======
    ui_NeedleUnitLabel = lv_label_create(ui_ScreenPageNeedle);
    lv_label_set_text(ui_NeedleUnitLabel, "");
    lv_obj_set_style_text_font(ui_NeedleUnitLabel, &ui_font_FontTypoderSize20, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_NeedleUnitLabel, ui_theme_color_lv(UI_COLOR_TEXT_SECONDARY), LV_PART_MAIN);
    lv_obj_set_style_text_align(ui_NeedleUnitLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(ui_NeedleUnitLabel, LV_ALIGN_CENTER, 0, 94);

    // Apply the current data source (set range/name/unit)
    ui_needle_apply_source();

    lv_obj_move_foreground(ring);   // bring the ring to the front
    lv_obj_add_event_cb(ui_ScreenPageNeedle, ui_event_needle_background, LV_EVENT_GESTURE, NULL);
}

void ui_ScreenPageNeedleConfig_screen_init(void)
{
    ui_ScreenPageNeedleConfig = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageNeedleConfig, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_ScreenPageNeedleConfig, 360, LV_PART_MAIN);
    ui_helpers_style_screen_bg(ui_ScreenPageNeedleConfig);
    lv_obj_set_style_bg_opa(ui_ScreenPageNeedleConfig, 255, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_ScreenPageNeedleConfig, 0, LV_PART_MAIN);  // remove the default theme white border
    lv_obj_set_style_pad_all(ui_ScreenPageNeedleConfig, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(ui_ScreenPageNeedleConfig, 0, LV_PART_MAIN);

    // Title
    lv_obj_t *title = lv_label_create(ui_ScreenPageNeedleConfig);
    lv_label_set_text(title, "DATA SOURCE");
    lv_obj_set_style_text_font(title, &ui_font_FontTypoderSize24, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -110);

    // Build the selectable sources for the current vehicle profile (includes BOOST only with turbo), then generate the options string
    build_needle_sources();
    char options[176] = {0};
    for (uint16_t i = 0; i < s_needle_source_count; i++) {
        if (i > 0) strlcat(options, "\n", sizeof(options));
        strlcat(options, ui_disp_item_name(s_needle_sources[i]), sizeof(options));
    }

    const nvs_user_cfg_t *cfg = nvs_cfg_get();

    s_roller_source = lv_roller_create(ui_ScreenPageNeedleConfig);
    lv_obj_clear_flag(s_roller_source, LV_OBJ_FLAG_GESTURE_BUBBLE); // scrolling the selection must not trigger the page gesture (back)
    lv_roller_set_options(s_roller_source, options, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(s_roller_source, 3);
    lv_roller_set_selected(s_roller_source, source_to_roller_pos(cfg->needle_source_idx), LV_ANIM_OFF);
    lv_obj_set_width(s_roller_source, 160);
    ui_helpers_style_dark_roller(s_roller_source, &ui_font_FontTypoderSize24);
    lv_obj_align(s_roller_source, LV_ALIGN_CENTER, 0, 8);
    lv_obj_add_event_cb(s_roller_source, on_needle_source_changed, LV_EVENT_VALUE_CHANGED, NULL);

    // Hint
    lv_obj_t *hint = lv_label_create(ui_ScreenPageNeedleConfig);
    lv_label_set_text(hint, "Swipe to go back");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 120);

    lv_obj_add_event_cb(ui_ScreenPageNeedleConfig, ui_event_needle_config_background, LV_EVENT_GESTURE, NULL);
}
