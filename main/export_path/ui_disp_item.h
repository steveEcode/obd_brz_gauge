#pragma once
// ================================================================
//  ui_disp_item.h — 可配置数据项系统 (从 ui.c 抽出)
//
//  温度页 / Info 页 / 指针页 / 曲线页共用的数据项元数据与工具:
//  名称/单位/颜色、原始值读取、刷表动画值、文本格式化、报警着色、自然量程。
//
//  原始值约定: 温度 °C、百分比 %、转速 rpm、车速 km/h、电压 mV、
//              机油压/涡轮压/刹车温均为 ×10 整数。
//  自然量程: nmin/nmax 为自然单位, div 满足 原始值 = 自然值 × div。
// ================================================================

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DISP_ITEM_CLT = 0,
    DISP_ITEM_IAT,
    DISP_ITEM_OIL,
    DISP_ITEM_LOAD,
    DISP_ITEM_TPS,
    DISP_ITEM_RPM,
    DISP_ITEM_SPEED,
    DISP_ITEM_BAT,
    DISP_ITEM_OILP,
    DISP_ITEM_BKT,
    DISP_ITEM_BOOST,
    DISP_ITEM_COUNT
} disp_item_t;

// 通用曲线页无效样本哨兵 (区别于合法负值如水温 -10)
#define CHART_INVALID (-32768)

typedef struct {
    int32_t nmin;
    int32_t nmax;
    int32_t div;
} needle_scale_meta_t;

typedef struct {
    const char *name;
    const char *unit;
    uint32_t color;
} disp_item_meta_t;

// 全局元数据表 (ui.c 的页面刷新逻辑直接按索引访问)
extern const disp_item_meta_t s_disp_meta[DISP_ITEM_COUNT];
extern const needle_scale_meta_t s_needle_scale_meta[DISP_ITEM_COUNT];

// 从 OBD 缓存原始值读取指定数据项, 成功返回 true 并写 *out
bool disp_item_read_value(disp_item_t item,
                          int16_t clt, int16_t iat, int16_t oil,
                          int16_t load_pct, int16_t tps, int32_t bat_mv,
                          int16_t oilp_x10, int16_t brake_x10,
                          uint16_t rpm, uint16_t speed, int16_t boost_x10,
                          int32_t *out);

// 刷表动画值: r ∈ [0,1] → 该数据项的扫表峰值原始值
int32_t disp_item_sweep_value(disp_item_t item, float r);

// 按数据项格式化数值文本写入 label (含单位换算与字体设置)
void disp_item_set_text(lv_obj_t *label, disp_item_t item, int32_t value, bool valid);

// 按 NVS 报警阈值 (nvs_chart_alarm_get) 着色: 超阈值红, 否则白
void disp_item_set_value_color(lv_obj_t *label, disp_item_t item, int32_t value, bool valid);

// 一体化更新: 自适应步进平滑 + 文本格式化 + 报警着色
// *state 维护当前显示值, valid=false 时保持上次有效值
void disp_item_update(int32_t *state, lv_obj_t *label, disp_item_t item,
                      int32_t raw, bool valid, int32_t threshold);

// 访问器 (越界回退到 CLT; name 越界返回 "")
const char *ui_disp_item_name(uint8_t item);
const char *ui_disp_item_unit(uint8_t item);
uint32_t ui_disp_item_color(uint8_t item);
void ui_disp_item_range(uint8_t item, int32_t *nmin, int32_t *nmax, int32_t *div);

// 直接取自然量程元数据 (item 须 < DISP_ITEM_COUNT)
const needle_scale_meta_t *ui_disp_item_scale(disp_item_t item);

#ifdef __cplusplus
}
#endif
