// Settings Page
// Configure: default boot page, vehicle and screen brightness
// 下滑(LV_DIR_BOTTOM)进入三连表设置页 (见 ui_event_settings_background)

#include "../ui.h"
#include <string.h>
#include "bsp_obd_dsp/nvs_storage.h"
#include "bsp_obd_dsp/lcd_driver/ST77916.h"
#include "app_obd_dsp/vehicle_profiles.h"

// Page names for roller (刹车温已并入曲线页 CHART; 顺序须与 ui.c 开机默认页 switch 一致)
// 0=TEMP 1=INFO 2=CHART 3=NEEDLE 4=GEAR 5=RPM 6=SPEED
static const char *page_names = "TEMP\nINFO\nCHART\nNEEDLE\nGEAR\nRPM\nSPEED";
#define BOOT_PAGE_COUNT 7

// Local references for settings widgets
static lv_obj_t *s_roller_page = NULL;
static lv_obj_t *s_roller_vehicle = NULL;
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
    lv_label_set_text_fmt(s_label_bright_val, "%d%%", (int)val);
    nvs_user_cfg_t cfg = *nvs_cfg_get();
    cfg.brightness_day = (uint8_t)val;
    nvs_cfg_set(&cfg);
    Set_Backlight((uint8_t)val);
}

static void on_vehicle_roller_change(lv_event_t *e)
{
    uint8_t selected = (uint8_t)lv_roller_get_selected(s_roller_vehicle);
    // vehicle_profile_set_active 会对越界索引归零，这里无需再 clamp。

    nvs_user_cfg_t cfg = *nvs_cfg_get();
    cfg.vehicle_profile_idx = selected;
    nvs_cfg_set(&cfg);

    // 同步更新当前激活车型，档位识别等运行时逻辑立即生效。
    vehicle_profile_set_active(selected);
}

void ui_ScreenPageSettings_screen_init(void)
{
    const nvs_user_cfg_t *cfg = nvs_cfg_get();

    ui_ScreenPageSettings = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageSettings, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_ScreenPageSettings, 360, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_ScreenPageSettings, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_ScreenPageSettings, 255, LV_PART_MAIN);

    // White border ring
    lv_obj_t *ring = lv_obj_create(ui_ScreenPageSettings);   // 白环: 静态圆形 border, 替代旋转 spinner, 消除弧接缝缺口
    lv_obj_set_size(ring, 360, 360);
    lv_obj_set_align(ring, LV_ALIGN_CENTER);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ring, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ring, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(ring, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_width(ring, 10, LV_PART_MAIN);
    lv_obj_set_style_border_opa(ring, 255, LV_PART_MAIN);

    // ====== Title ======
    lv_obj_t *title = lv_label_create(ui_ScreenPageSettings);
    lv_label_set_text(title, "SETTINGS");
    lv_obj_set_style_text_font(title, &ui_font_FontTypoderSize24, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -138);

    // ====== Row 1: Default Page (Boot Page) ======
    lv_obj_t *label_page = lv_label_create(ui_ScreenPageSettings);
    lv_label_set_text(label_page, "BOOT PAGE");
    lv_obj_set_style_text_font(label_page, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(label_page, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_align(label_page, LV_ALIGN_CENTER, 0, -112);

    s_roller_page = lv_roller_create(ui_ScreenPageSettings);
    lv_obj_clear_flag(s_roller_page, LV_OBJ_FLAG_GESTURE_BUBBLE); // 滚动选值时不触发页面手势
    lv_roller_set_options(s_roller_page, page_names, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(s_roller_page, 1);
    lv_roller_set_selected(s_roller_page, (cfg->default_page < BOOT_PAGE_COUNT) ? cfg->default_page : 0, LV_ANIM_OFF);
    lv_obj_set_width(s_roller_page, 140);
    lv_obj_set_style_text_font(s_roller_page, &ui_font_FontTypoderSize20, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_roller_page, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_roller_page, lv_color_hex(0x222222), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_roller_page, 255, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_roller_page, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_roller_page, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_radius(s_roller_page, 8, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_roller_page, &ui_font_FontTypoderSize20, LV_PART_SELECTED);
    lv_obj_set_style_text_color(s_roller_page, lv_color_hex(0x000000), LV_PART_SELECTED);
    lv_obj_set_style_bg_color(s_roller_page, lv_color_hex(0xFFFFFF), LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(s_roller_page, 255, LV_PART_SELECTED);
    lv_obj_align(s_roller_page, LV_ALIGN_CENTER, 0, -82);
    lv_obj_add_event_cb(s_roller_page, on_page_roller_change, LV_EVENT_VALUE_CHANGED, NULL);

    // ====== Divider ======
    lv_obj_t *div1 = lv_obj_create(ui_ScreenPageSettings);
    lv_obj_remove_style_all(div1);
    lv_obj_set_size(div1, 220, 1);
    lv_obj_set_align(div1, LV_ALIGN_CENTER);
    lv_obj_set_y(div1, -54);
    lv_obj_set_style_bg_color(div1, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(div1, 40, LV_PART_MAIN);
    lv_obj_clear_flag(div1, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    // ====== Row 2: Vehicle ======
    lv_obj_t *label_vehicle = lv_label_create(ui_ScreenPageSettings);
    lv_label_set_text(label_vehicle, "VEHICLE");
    lv_obj_set_style_text_font(label_vehicle, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(label_vehicle, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_align(label_vehicle, LV_ALIGN_CENTER, 0, -30);

    // 从车型配置动态生成 roller 选项（换行分隔），新增车型自动出现
    uint8_t vehicle_count = 0;
    const vehicle_profile_t *vehicle_list = vehicle_profile_get_all(&vehicle_count);
    char vehicle_names[160] = {0};
    for (uint8_t i = 0; i < vehicle_count; i++) {
        if (i > 0) strlcat(vehicle_names, "\n", sizeof(vehicle_names));
        strlcat(vehicle_names, vehicle_list[i].name, sizeof(vehicle_names));
    }

    s_roller_vehicle = lv_roller_create(ui_ScreenPageSettings);
    lv_obj_clear_flag(s_roller_vehicle, LV_OBJ_FLAG_GESTURE_BUBBLE); // 滚动选值时不触发页面手势
    lv_roller_set_options(s_roller_vehicle, vehicle_names, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(s_roller_vehicle, 1);
    lv_roller_set_selected(s_roller_vehicle, (cfg->vehicle_profile_idx < vehicle_count) ? cfg->vehicle_profile_idx : 0, LV_ANIM_OFF);
    lv_obj_set_width(s_roller_vehicle, 210);   // 加宽以容纳较长车型名(如 "BMW X1 F48")
    lv_obj_set_style_text_font(s_roller_vehicle, &ui_font_FontTypoderSize20, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_roller_vehicle, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_roller_vehicle, lv_color_hex(0x222222), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_roller_vehicle, 255, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_roller_vehicle, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_roller_vehicle, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_radius(s_roller_vehicle, 8, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_roller_vehicle, &ui_font_FontTypoderSize20, LV_PART_SELECTED);
    lv_obj_set_style_text_color(s_roller_vehicle, lv_color_hex(0x000000), LV_PART_SELECTED);
    lv_obj_set_style_bg_color(s_roller_vehicle, lv_color_hex(0xFFFFFF), LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(s_roller_vehicle, 255, LV_PART_SELECTED);
    lv_obj_align(s_roller_vehicle, LV_ALIGN_CENTER, 0, -2);
    lv_obj_add_event_cb(s_roller_vehicle, on_vehicle_roller_change, LV_EVENT_VALUE_CHANGED, NULL);

    // ====== Divider ======
    lv_obj_t *div2 = lv_obj_create(ui_ScreenPageSettings);
    lv_obj_remove_style_all(div2);
    lv_obj_set_size(div2, 220, 1);
    lv_obj_set_align(div2, LV_ALIGN_CENTER);
    lv_obj_set_y(div2, 26);
    lv_obj_set_style_bg_color(div2, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(div2, 40, LV_PART_MAIN);
    lv_obj_clear_flag(div2, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    // ====== Row 3: Brightness ======
    lv_obj_t *label_bright = lv_label_create(ui_ScreenPageSettings);
    lv_label_set_text(label_bright, "BRIGHTNESS");
    lv_obj_set_style_text_font(label_bright, &ui_font_FontTypoderSize16, LV_PART_MAIN);
    lv_obj_set_style_text_color(label_bright, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_align(label_bright, LV_ALIGN_CENTER, 0, 40);

    s_slider_bright = lv_slider_create(ui_ScreenPageSettings);
    lv_slider_set_range(s_slider_bright, 10, 100);
    lv_slider_set_value(s_slider_bright, cfg->brightness_day, LV_ANIM_OFF);
    lv_obj_set_width(s_slider_bright, 180);
    lv_obj_set_height(s_slider_bright, 10);
    lv_obj_align(s_slider_bright, LV_ALIGN_CENTER, 0, 64);
    lv_obj_set_style_bg_color(s_slider_bright, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_slider_bright, 255, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_slider_bright, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_slider_bright, 255, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_slider_bright, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_slider_bright, 5, LV_PART_KNOB);
    lv_obj_clear_flag(s_slider_bright, LV_OBJ_FLAG_GESTURE_BUBBLE);      // 防止滑块拖拽触发页面手势
    lv_obj_add_event_cb(s_slider_bright, on_bright_slider_change, LV_EVENT_VALUE_CHANGED, NULL);

    s_label_bright_val = lv_label_create(ui_ScreenPageSettings);
    lv_label_set_text_fmt(s_label_bright_val, "%d%%", cfg->brightness_day);
    lv_obj_set_style_text_font(s_label_bright_val, &ui_font_FontTypoderSize24, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_label_bright_val, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(s_label_bright_val, LV_ALIGN_CENTER, 0, 86);

    // ====== Hint ======
    lv_obj_t *hint = lv_label_create(ui_ScreenPageSettings);
    lv_label_set_text(hint, "Swipe down: multi-gauge\nL/R: back");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 124);

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
    lv_obj_move_foreground(ring);   // 圆环置顶
    lv_obj_add_event_cb(ui_ScreenPageSettings, ui_event_settings_background, LV_EVENT_GESTURE, NULL);
}
