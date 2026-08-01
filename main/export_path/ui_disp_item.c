// ================================================================
//  ui_disp_item.c — 可配置数据项系统实现 (从 ui.c 抽出)
// ================================================================

#include "ui_disp_item.h"
#include "ui.h"   // ui_font_FontTypoderSize40
#include "bsp_obd_dsp/nvs_storage.h"

const disp_item_meta_t s_disp_meta[DISP_ITEM_COUNT] = {
    {"CLT", "'C", 0x44AAFF},
    {"IAT", "'C", 0x44FF88},
    {"OIL", "'C", 0xFF7722},
    {"LOD", "%", 0xFFCC00},
    {"TPS", "%", 0xFF8844},
    {"RPM", "rpm", 0x66CCFF},
    {"SPD", "km/h", 0xFFFFFF},
    {"BAT", "V", 0xAACCFF},
    {"OIP", "bar", 0xFFD166},
    {"BKT", "'C", 0xFF5A5A},
    {"BST", "bar", 0x00DD88},
};

// 每个数据项的指针量程: nmin/nmax 为自然单位 (同时用于刻度数字与指针位置),
// div 把缓存原始值换算成自然单位 (BAT:mV→V, OILP/BKT:0.1单位→整数)。
const needle_scale_meta_t s_needle_scale_meta[DISP_ITEM_COUNT] = {
    [DISP_ITEM_CLT]   = {-20, 130, 1},
    [DISP_ITEM_IAT]   = {-20, 100, 1},
    [DISP_ITEM_OIL]   = {-20, 160, 1},
    [DISP_ITEM_LOAD]  = {0, 100, 1},
    [DISP_ITEM_TPS]   = {0, 100, 1},
    [DISP_ITEM_RPM]   = {0, 8000, 1},
    [DISP_ITEM_SPEED] = {0, 240, 1},
    [DISP_ITEM_BAT]   = {8, 16, 1000},
    [DISP_ITEM_OILP]  = {0, 10, 10},
    [DISP_ITEM_BKT]   = {0, 800, 10},
    [DISP_ITEM_BOOST] = {0, 20, 1},   // 量程以 0.1bar 计: 0 ~ +2.0 bar 表压(不显示负压)
};

bool disp_item_read_value(disp_item_t item,
                          int16_t clt, int16_t iat, int16_t oil,
                          int16_t load_pct, int16_t tps, int32_t bat_mv,
                          int16_t oilp_x10, int16_t brake_x10,
                          uint16_t rpm, uint16_t speed, int16_t boost_x10,
                          int32_t *out)
{
    if (!out) return false;
    switch (item) {
        case DISP_ITEM_CLT: if (clt > -40) { *out = clt; return true; } return false;
        case DISP_ITEM_IAT: if (iat > -40) { *out = iat; return true; } return false;
        case DISP_ITEM_OIL: if (oil > -41) { *out = oil; return true; } return false;
        case DISP_ITEM_LOAD: if (load_pct >= 0) { *out = load_pct; return true; } return false;
        case DISP_ITEM_TPS: if (tps >= 0) { *out = tps; return true; } return false;
        case DISP_ITEM_RPM: *out = rpm; return true;
        case DISP_ITEM_SPEED: *out = speed; return true;
        case DISP_ITEM_BAT: if (bat_mv > 0) { *out = bat_mv; return true; } return false;
        case DISP_ITEM_OILP: if (oilp_x10 >= 0) { *out = oilp_x10; return true; } return false;
        case DISP_ITEM_BKT: if (brake_x10 > -1000) { *out = brake_x10; return true; } return false;
        case DISP_ITEM_BOOST: if (boost_x10 != -32768) { *out = boost_x10; return true; } return false;
        default: return false;
    }
}

int32_t disp_item_sweep_value(disp_item_t item, float r)
{
    switch (item) {
        case DISP_ITEM_CLT:
        case DISP_ITEM_IAT:
        case DISP_ITEM_OIL:
            return (int32_t)(120.0f * r);
        case DISP_ITEM_LOAD:
        case DISP_ITEM_TPS:
            return (int32_t)(100.0f * r);
        case DISP_ITEM_RPM:
            return (int32_t)(8000.0f * r);   // = SWEEP_RPM_PEAK
        case DISP_ITEM_SPEED:
            return (int32_t)(999.0f * r);    // = SWEEP_SPEED_PEAK
        case DISP_ITEM_BAT:
            return (int32_t)(12000.0f + 2400.0f * r); // 12.0~14.4V
        case DISP_ITEM_OILP:
            return (int32_t)(100.0f * r); // 0.0~10.0bar (x10)
        case DISP_ITEM_BKT:
            return (int32_t)(600.0f * r); // 0.0~60.0'C (x10)
        case DISP_ITEM_BOOST:
            return (int32_t)(20.0f * r); // 0.0~2.0bar 表压 (x10)
        default:
            return 0;
    }
}

void disp_item_set_text(lv_obj_t *label, disp_item_t item, int32_t value, bool valid)
{
    if (!label) return;
    lv_obj_set_style_text_font(label, &ui_font_FontTypoderSize40, LV_PART_MAIN);

    if (!valid) {
        lv_label_set_text(label, "--");
        return;
    }

    if (item == DISP_ITEM_BAT) {
        lv_label_set_text_fmt(label, "%d.%d", (int)(value / 1000), (int)((value % 1000) / 100));
    } else if (item == DISP_ITEM_OILP) {
        int32_t abs_val = (value < 0) ? -value : value;
        lv_label_set_text_fmt(label, "%d.%d", (int)(value / 10), (int)(abs_val % 10));
    } else if (item == DISP_ITEM_BKT) {
        lv_label_set_text_fmt(label, "%ld", (long)(value / 10));
    } else if (item == DISP_ITEM_BOOST) {
        // 表压可为负(真空)，带符号显示一位小数, e.g. -0.6 / 1.2
        int32_t a = (value < 0) ? -value : value;
        lv_label_set_text_fmt(label, "%s%d.%d", (value < 0) ? "-" : "", (int)(a / 10), (int)(a % 10));
    } else {
        lv_label_set_text_fmt(label, "%ld", (long)value);
    }
}

void disp_item_set_value_color(lv_obj_t *label, disp_item_t item, int32_t value, bool valid)
{
    if (!label) return;
    int16_t thr = nvs_chart_alarm_get((uint8_t)item);   // 原始值单位; 32767=关闭
    lv_color_t color = (valid && value >= (int32_t)thr) ? lv_color_hex(0xFF4D4D) : lv_color_hex(0xFFFFFF);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
}

// 自适应步进: 差值 ≤ threshold 逐 ±1, 差值 > threshold 按比例快速逼近
static int32_t anim_step_i32(int32_t displayed, int32_t target, int32_t threshold)
{
    int32_t diff = target - displayed;
    if (diff == 0) return displayed;
    int32_t abs_diff = (diff > 0) ? diff : -diff;
    int32_t step = (diff > 0) ? 1 : -1;

    if (abs_diff > threshold) {
        int32_t rapid = abs_diff / 3;     // 每拍吃掉 ~33% 差距
        if (rapid < 2) rapid = 2;          // 最小 2 步
        if (rapid > abs_diff) rapid = abs_diff;
        step = (diff > 0) ? rapid : -rapid;
    }
    return displayed + step;
}

void disp_item_update(int32_t *state, lv_obj_t *label, disp_item_t item,
                      int32_t raw, bool valid, int32_t threshold)
{
    if (!state || !label) return;
    if (valid) {
        // RPM 直出, 不走 +1/+1 递进动画: 量程大、变化快, 平滑只会看起来像"卡住不动"
        *state = (item == DISP_ITEM_RPM) ? raw : anim_step_i32(*state, raw, threshold);
    }
    // invalid: 保持 *state 不变, 避免恢复时从 0 爬升
    disp_item_set_text(label, item, *state, valid);
    disp_item_set_value_color(label, item, *state, valid);
}

const char *ui_disp_item_name(uint8_t item)
{
    if (item >= DISP_ITEM_COUNT) return "";
    return s_disp_meta[item].name;
}

const char *ui_disp_item_unit(uint8_t item)
{
    if (item >= DISP_ITEM_COUNT) item = 0;
    return s_disp_meta[item].unit;
}

uint32_t ui_disp_item_color(uint8_t item)
{
    if (item >= DISP_ITEM_COUNT) item = 0;
    return s_disp_meta[item].color;
}

void ui_disp_item_range(uint8_t item, int32_t *nmin, int32_t *nmax, int32_t *div)
{
    if (item >= DISP_ITEM_COUNT) item = 0;
    const needle_scale_meta_t *ns = &s_needle_scale_meta[item];
    if (nmin) *nmin = ns->nmin;
    if (nmax) *nmax = ns->nmax;
    if (div)  *div  = ns->div;
}

const needle_scale_meta_t *ui_disp_item_scale(disp_item_t item)
{
    return &s_needle_scale_meta[item];
}
