/* Real built-in pages and fonts: compact values must never wrap or lose digits. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "export_path/ui.h"
#include "export_path/ui_metric_layout.h"

lv_obj_t *ui_ScreenPageTemp;
static lv_color_t frame[360 * 360];
static const uint8_t black_pixel[2];
const lv_img_dsc_t ui_img_pngblackear_png = {
    .header = {.cf = LV_IMG_CF_TRUE_COLOR, .w = 1, .h = 1},
    .data_size = sizeof(black_pixel), .data = black_pixel,
};
void ui_event_info_background(lv_event_t *event) { (void)event; }
void ui_event_temp_background(lv_event_t *event) { (void)event; }
void ui_helpers_style_screen_bg(lv_obj_t *screen)
{ lv_obj_set_style_bg_color(screen, lv_color_black(), 0); }
lv_obj_t *ui_helpers_create_ring(lv_obj_t *parent, uint8_t width)
{
    lv_obj_t *ring = lv_obj_create(parent);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, 360, 360);
    lv_obj_center(ring);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, width, 0);
    lv_obj_set_style_border_color(ring, lv_color_white(), 0);
    lv_obj_set_style_border_opa(ring, LV_OPA_COVER, 0);
    return ring;
}
static void flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *pixels)
{
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x) frame[y * 360 + x] = *pixels++;
    lv_disp_flush_ready(driver);
}
static void capture(const char *name)
{
    const char *dir = getenv("METRIC_CAPTURE_DIR");
    if (!dir) return;
    lv_refr_now(NULL);
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    FILE *out = fopen(path, "wb"); assert(out);
    fprintf(out, "P6\n360 360\n255\n");
    for (unsigned i = 0; i < 360 * 360; ++i) {
        lv_color32_t pixel = {.full = lv_color_to32(frame[i])};
        uint8_t rgb[] = {pixel.ch.red, pixel.ch.green, pixel.ch.blue};
        assert(fwrite(rgb, 1, 3, out) == 3);
    }
    assert(fclose(out) == 0);
}
static void update_value(lv_obj_t *label, disp_item_t item, const char *text)
{
    lv_label_set_text(label, text);
    ui_metric_value_fit(label, item);
    lv_obj_update_layout(label);
    const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    assert(strcmp(lv_label_get_text(label), text) == 0);
    assert(lv_label_get_long_mode(label) == LV_LABEL_LONG_CLIP);
    assert(lv_txt_get_width(text, (uint32_t)strlen(text), font, 0, LV_TEXT_FLAG_NONE)
           <= lv_obj_get_content_width(label) - 4);
    assert(lv_obj_get_height(label) == font->line_height);
}
static void set_value(lv_obj_t *label, disp_item_t item, const char *text, bool temp_row)
{
    ui_metric_value_layout(label, item, temp_row);
    update_value(label, item, text);
}
static void test_info(void)
{
    ui_ScreenPageInfo_screen_init(); lv_scr_load(ui_ScreenPageInfo);
    /* The old 40px-font / 120px cell really cannot hold four wide digits. */
    assert(lv_txt_get_width("8000", 4, &ui_font_FontTypoderSize40, 0, LV_TEXT_FLAG_NONE) > 120);
    const char *readings[] = {"--", "0", "800", "999", "1000", "1111", "8000", "9999", "10000", "65535"};
    for (unsigned i = 0; i < 5; ++i) {
        lv_label_set_text(ui_LabelInfoName[i], "RPM");
        lv_label_set_text(ui_LabelInfoUnit[i], "rpm");
        for (unsigned j = 0; j < sizeof(readings) / sizeof(readings[0]); ++j) {
            set_value(ui_LabelInfoValue[i], DISP_ITEM_RPM, readings[j], false);
            assert(lv_obj_get_width(ui_LabelInfoValue[i]) == 140);
            assert(lv_obj_get_style_text_font(ui_LabelInfoValue[i], 0) ==
                   (strlen(readings[j]) > 4 ? &ui_font_FontTypoderSize24 : &ui_font_FontTypoderSize36));
        }
        /* Live samples resize both ways without a source/layout reset. */
        update_value(ui_LabelInfoValue[i], DISP_ITEM_RPM, "8000");
        assert(lv_obj_get_style_text_font(ui_LabelInfoValue[i], 0) == &ui_font_FontTypoderSize36);
        update_value(ui_LabelInfoValue[i], DISP_ITEM_RPM, "10000");
        assert(lv_obj_get_style_text_font(ui_LabelInfoValue[i], 0) == &ui_font_FontTypoderSize24);
        update_value(ui_LabelInfoValue[i], DISP_ITEM_RPM, "8000");
        assert(lv_obj_get_style_text_font(ui_LabelInfoValue[i], 0) == &ui_font_FontTypoderSize36);
    }
    capture("info-rpm-8000");
    for (unsigned i = 0; i < 4; i += 2) {
        lv_area_t left, right;
        lv_obj_get_coords(ui_LabelInfoValue[i], &left);
        lv_obj_get_coords(ui_LabelInfoValue[i + 1], &right);
        assert(left.x2 < 180 && right.x1 > 180);
    }
    for (unsigned i = 0; i < 5; ++i) set_value(ui_LabelInfoValue[i], DISP_ITEM_RPM, "65535", false);
    capture("info-rpm-65535");
    /* Remapping restores the ordinary font/width; signed pressure still fits. */
    set_value(ui_LabelInfoValue[0], DISP_ITEM_CLT, "-40", false);
    assert(lv_obj_get_width(ui_LabelInfoValue[0]) == 120);
    assert(lv_obj_get_style_text_font(ui_LabelInfoValue[0], 0) == &ui_font_FontTypoderSize40);
    set_value(ui_LabelInfoValue[0], DISP_ITEM_OILP, "-327.68", false);
    assert(lv_obj_get_width(ui_LabelInfoValue[0]) == 140);
}
static void test_temp(void)
{
    ui_ScreenPageTemp_screen_init(); lv_scr_load(ui_ScreenPageTemp);
    for (unsigned i = 0; i < 3; ++i) {
        lv_label_set_text(ui_LabelTempName[i], "RPM");
        lv_label_set_text(ui_LabelTempUnit[i], "rpm");
        set_value(ui_LabelTempValue[i], DISP_ITEM_RPM, "8000", true);
        lv_area_t value, dot;
        lv_obj_get_coords(ui_LabelTempValue[i], &value);
        lv_obj_get_coords(ui_LabelTempDot[i], &dot);
        assert(value.x2 < dot.x1);
        lv_area_t name, unit;
        lv_obj_get_coords(ui_LabelTempName[i], &name);
        lv_obj_get_coords(ui_LabelTempUnit[i], &unit);
        assert(name.y2 < unit.y1);
        assert(value.x2 < name.x1 && value.x2 < unit.x1);
    }
    capture("temp-rpm-8000");
    for (unsigned i = 0; i < 3; ++i) set_value(ui_LabelTempValue[i], DISP_ITEM_RPM, "65535", true);
    capture("temp-rpm-65535");
    set_value(ui_LabelTempValue[0], DISP_ITEM_CLT, "-40", true);
    assert(lv_obj_get_width(ui_LabelTempValue[0]) == 110);
    assert(lv_obj_get_style_x(ui_LabelTempValue[0], 0) == 70);
    set_value(ui_LabelTempValue[0], DISP_ITEM_OILP, "12.34", true);
    assert(lv_obj_get_style_x(ui_LabelTempValue[0], 0) == 40);
}
int main(void)
{
    lv_init();
    static lv_color_t draw[360 * 20];
    static lv_disp_draw_buf_t buffer;
    lv_disp_draw_buf_init(&buffer, draw, NULL, 360 * 20);
    lv_disp_drv_t driver; lv_disp_drv_init(&driver);
    driver.hor_res = 360; driver.ver_res = 360; driver.draw_buf = &buffer; driver.flush_cb = flush;
    lv_disp_drv_register(&driver);
    test_info(); test_temp();
    ui_metric_value_layout(NULL, DISP_ITEM_RPM, false);
    ui_metric_value_fit(NULL, DISP_ITEM_RPM);
    lv_obj_del(ui_ScreenPageInfo); lv_obj_del(ui_ScreenPageTemp);
    puts("compact metric layout: all checks passed");
    return 0;
}
