#pragma once
// ================================================================
//  ui_ext.h — ui.c 的手写扩展逻辑 (不被 SquareLine 覆盖)
//
//  所有手写的 UI 逻辑 (showroom / 开机动画 / 刷表 / 转速报警)
//  都应放在 ui_ext.c 中, ui.c 只通过 ui_ext_tick() 调用。
//  这样 SquareLine Studio 重新导出 ui.c 时不会丢失手写代码。
// ================================================================

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 每 50ms 由 my_timerMain 调用一次
void ui_ext_tick(void);

// 初始化 (app_main 中 ui_init 之后调用)
void ui_ext_init(void);

// Showroom 状态查询 (poll task 用)
bool ui_ext_showroom_is_active(void);

// Showroom 假数据生成 (showroom 模式下由 poll task 调用)
void ui_ext_showroom_fake_data(void);

// ESP-NOW 同步接口 (espnow_link.c 调用)
void ui_ext_showroom_sync_slot(uint8_t slot);
void ui_ext_showroom_request_enter(void);

// 开机动画同步接口
int  ui_ext_intro_get_step(void);
void ui_ext_intro_set_step(int step);
int  ui_ext_sweep_get_step(void);

// 转速报警测试 (设置页调用)
void ui_ext_rpm_flash_test(void);

#ifdef __cplusplus
}
#endif
