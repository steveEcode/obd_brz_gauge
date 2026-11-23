#include "ui_metric_layout.h"
#include "ui.h"

#include <string.h>

static const lv_font_t *metric_base_font(disp_item_t item)
{
    return item == DISP_ITEM_RPM || item == DISP_ITEM_OILP
               ? &ui_font_FontTypoderSize36 : &ui_font_FontTypoderSize40;
}

void ui_metric_value_fit(lv_obj_t *label, disp_item_t item)
{
    if (!label) return;
    const char *text = lv_label_get_text(label);
    const lv_font_t *base = metric_base_font(item);
    /* Keep four-digit RPM at a stable size even for narrow digits (1111).
     * Five-digit readings still show the full integer, not a rounded kRPM. */
    if (item == DISP_ITEM_RPM && strlen(text) > 4) base = &ui_font_FontTypoderSize24;
    const lv_font_t *fonts[] = {
        &ui_font_FontTypoderSize40, &ui_font_FontTypoderSize36,
        &ui_font_FontTypoderSize24, &ui_font_FontTypoderSize20,
    };
    /* These labels have fixed pixel widths. Read the style rather than stale
     * coordinates after a source change; no synchronous layout per sample. */
    lv_coord_t room = lv_obj_get_style_width(label, LV_PART_MAIN)
                      - lv_obj_get_style_pad_left(label, LV_PART_MAIN)
                      - lv_obj_get_style_pad_right(label, LV_PART_MAIN) - 4;
    lv_coord_t spacing = lv_obj_get_style_text_letter_space(label, LV_PART_MAIN);
    const lv_font_t *font = fonts[3];
    for (unsigned i = 0; i < sizeof(fonts) / sizeof(fonts[0]); ++i) {
        if (fonts[i]->line_height > base->line_height) continue;
        font = fonts[i];
        if (lv_txt_get_width(text, (uint32_t)strlen(text), font, spacing, LV_TEXT_FLAG_NONE) <= room) break;
    }
    if (lv_obj_get_style_text_font(label, LV_PART_MAIN) != font)
        lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
}

void ui_metric_value_layout(lv_obj_t *label, disp_item_t item, bool temp_row)
{
    if (!label) return;
    bool wide = item == DISP_ITEM_OILP || item == DISP_ITEM_RPM;
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(label, metric_base_font(item), LV_PART_MAIN);
    lv_obj_set_width(label, wide ? 140 : (temp_row ? 110 : 120));
    if (temp_row) lv_obj_align(label, LV_ALIGN_LEFT_MID, wide ? 40 : 70,
                              lv_obj_get_style_y(label, LV_PART_MAIN));
}
