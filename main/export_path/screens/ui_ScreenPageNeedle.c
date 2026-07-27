// 指针式可配置仪表页 (Needle)
//  - 经典 270° lv_meter 指针表盘，中心显示数值+单位
//  - 下滑进入数据源选择页 (ui_ScreenPageNeedleConfig)
//  - 仅通过设置页的“默认启动页”进入；其他方向手势返回主页面
//  数据读取/量程/刷新逻辑复用 ui.c 的 disp_item 系统 (ui_needle_* 接口)

#include "../ui.h"
#include <string.h>
#include "bsp_obd_dsp/nvs_storage.h"
#include "app_obd_dsp/vehicle_profiles.h"

// 指针页可选数据源 (disp_item_t 值)，刻意排除 RPM(5)——RPM 已有独立页面。
// 顺序: CLT, IAT, OIL, LOAD, TPS, SPEED, BAT, OILP, BKT
#define NEEDLE_ITEM_BOOST 10   // disp_item_t DISP_ITEM_BOOST 的值
static const uint8_t k_needle_sources_base[] = {0, 1, 2, 3, 4, 6, 7, 8, 9};
#define NEEDLE_BASE_COUNT (sizeof(k_needle_sources_base) / sizeof(k_needle_sources_base[0]))

// 运行时根据所选车型是否有涡轮，动态决定可选数据源（有涡轮才追加 BOOST）
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

// 把当前 NVS 中的数据源映射到 roller 选项序号
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

    // 立即重建指针页量程/名称/单位（页面对象已存在，下次显示即生效）
    ui_needle_apply_source();
}

void ui_ScreenPageNeedle_screen_init(void)
{
    ui_ScreenPageNeedle = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageNeedle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_ScreenPageNeedle, 360, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_ScreenPageNeedle, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_ScreenPageNeedle, 255, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_ScreenPageNeedle, 0, LV_PART_MAIN);  // 关默认主题边框，改用白环
    lv_obj_set_style_pad_all(ui_ScreenPageNeedle, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(ui_ScreenPageNeedle, 0, LV_PART_MAIN);

    // ====== 外圈白环（与其它页面一致）======
    lv_obj_t *ring = lv_obj_create(ui_ScreenPageNeedle);   // 白环: 静态圆形 border, 替代旋转 spinner, 消除弧接缝缺口
    lv_obj_set_size(ring, 360, 360);
    lv_obj_set_align(ring, LV_ALIGN_CENTER);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ring, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ring, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(ring, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_width(ring, 8, LV_PART_MAIN);
    lv_obj_set_style_border_opa(ring, 255, LV_PART_MAIN);

    // ====== 指针表盘 ======
    ui_NeedleMeter = lv_meter_create(ui_ScreenPageNeedle);
    lv_obj_set_size(ui_NeedleMeter, 320, 320);
    lv_obj_center(ui_NeedleMeter);
    lv_obj_clear_flag(ui_NeedleMeter, LV_OBJ_FLAG_CLICKABLE);
    // 表盘背景透明，融入黑色页面
    lv_obj_set_style_bg_opa(ui_NeedleMeter, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_NeedleMeter, 0, LV_PART_MAIN);
    // 刻度数字字体
    lv_obj_set_style_text_font(ui_NeedleMeter, &ui_font_FontTypoderSize16, LV_PART_TICKS);
    lv_obj_set_style_text_color(ui_NeedleMeter, lv_color_hex(0x666666), LV_PART_TICKS);

    ui_NeedleScale = lv_meter_add_scale(ui_NeedleMeter);
    // 刻度调暗, 突出指针
    lv_meter_set_scale_ticks(ui_NeedleMeter, ui_NeedleScale, 21, 1, 6, lv_color_hex(0x333333));
    lv_meter_set_scale_major_ticks(ui_NeedleMeter, ui_NeedleScale, 5, 2, 10, lv_color_hex(0x555555), 14);
    lv_meter_set_scale_range(ui_NeedleMeter, ui_NeedleScale, 0, 100, 270, 135); // 占位，apply_source 重设

    // 指针: 加粗+加长+亮色, 更醒目
    ui_NeedleIndic = lv_meter_add_needle_line(ui_NeedleMeter, ui_NeedleScale, 10, lv_color_hex(0xFF1010), -10);

    // 三个中心标签居中对齐自身，避免不同文本长度造成左右偏移/重合
    // ====== 名称标签（上方）======
    ui_NeedleNameLabel = lv_label_create(ui_ScreenPageNeedle);
    lv_label_set_text(ui_NeedleNameLabel, "");
    lv_obj_set_style_text_font(ui_NeedleNameLabel, &ui_font_FontTypoderSize24, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_NeedleNameLabel, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
    lv_obj_set_style_text_align(ui_NeedleNameLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(ui_NeedleNameLabel, LV_ALIGN_CENTER, 0, -52);

    // ====== 数值标签（下方，位于 270° 表盘底部开口处，指针扫不到此处，不与指针重叠）======
    ui_NeedleValueLabel = lv_label_create(ui_ScreenPageNeedle);
    lv_label_set_text(ui_NeedleValueLabel, "--");
    lv_obj_set_style_text_font(ui_NeedleValueLabel, &ui_font_FontTypoderSize40, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_NeedleValueLabel, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_align(ui_NeedleValueLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(ui_NeedleValueLabel, LV_ALIGN_CENTER, 0, 54);

    // ====== 单位标签（数值下方）======
    ui_NeedleUnitLabel = lv_label_create(ui_ScreenPageNeedle);
    lv_label_set_text(ui_NeedleUnitLabel, "");
    lv_obj_set_style_text_font(ui_NeedleUnitLabel, &ui_font_FontTypoderSize20, LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_NeedleUnitLabel, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_set_style_text_align(ui_NeedleUnitLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(ui_NeedleUnitLabel, LV_ALIGN_CENTER, 0, 94);

    // 应用当前数据源（设置量程/名称/单位）
    ui_needle_apply_source();

    lv_obj_move_foreground(ring);   // 圆环置顶
    lv_obj_add_event_cb(ui_ScreenPageNeedle, ui_event_needle_background, LV_EVENT_GESTURE, NULL);
}

void ui_ScreenPageNeedleConfig_screen_init(void)
{
    ui_ScreenPageNeedleConfig = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageNeedleConfig, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_ScreenPageNeedleConfig, 360, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_ScreenPageNeedleConfig, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_ScreenPageNeedleConfig, 255, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_ScreenPageNeedleConfig, 0, LV_PART_MAIN);  // 去掉默认主题白边
    lv_obj_set_style_pad_all(ui_ScreenPageNeedleConfig, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(ui_ScreenPageNeedleConfig, 0, LV_PART_MAIN);

    // 标题
    lv_obj_t *title = lv_label_create(ui_ScreenPageNeedleConfig);
    lv_label_set_text(title, "DATA SOURCE");
    lv_obj_set_style_text_font(title, &ui_font_FontTypoderSize24, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -110);

    // 按当前车型构建可选数据源(有涡轮才含 BOOST)，再生成选项字符串
    build_needle_sources();
    char options[176] = {0};
    for (uint16_t i = 0; i < s_needle_source_count; i++) {
        if (i > 0) strlcat(options, "\n", sizeof(options));
        strlcat(options, ui_disp_item_name(s_needle_sources[i]), sizeof(options));
    }

    const nvs_user_cfg_t *cfg = nvs_cfg_get();

    s_roller_source = lv_roller_create(ui_ScreenPageNeedleConfig);
    lv_obj_clear_flag(s_roller_source, LV_OBJ_FLAG_GESTURE_BUBBLE); // 滚动选值时不触发页面手势(返回)
    lv_roller_set_options(s_roller_source, options, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(s_roller_source, 3);
    lv_roller_set_selected(s_roller_source, source_to_roller_pos(cfg->needle_source_idx), LV_ANIM_OFF);
    lv_obj_set_width(s_roller_source, 160);
    lv_obj_set_style_text_font(s_roller_source, &ui_font_FontTypoderSize24, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_roller_source, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_roller_source, lv_color_hex(0x222222), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_roller_source, 255, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_roller_source, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_roller_source, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_radius(s_roller_source, 8, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_roller_source, &ui_font_FontTypoderSize24, LV_PART_SELECTED);
    lv_obj_set_style_text_color(s_roller_source, lv_color_hex(0x000000), LV_PART_SELECTED);
    lv_obj_set_style_bg_color(s_roller_source, lv_color_hex(0xFFFFFF), LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(s_roller_source, 255, LV_PART_SELECTED);
    lv_obj_align(s_roller_source, LV_ALIGN_CENTER, 0, 8);
    lv_obj_add_event_cb(s_roller_source, on_needle_source_changed, LV_EVENT_VALUE_CHANGED, NULL);

    // 提示
    lv_obj_t *hint = lv_label_create(ui_ScreenPageNeedleConfig);
    lv_label_set_text(hint, "Swipe to go back");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x555555), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 120);

    lv_obj_add_event_cb(ui_ScreenPageNeedleConfig, ui_event_needle_config_background, LV_EVENT_GESTURE, NULL);
}
