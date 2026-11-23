#pragma once

#include "ui_disp_item.h"

/* Built-in TEMP rows / INFO cells only; never resize chart or theme values. */
void ui_metric_value_layout(lv_obj_t *label, disp_item_t item, bool temp_row);
void ui_metric_value_fit(lv_obj_t *label, disp_item_t item);
