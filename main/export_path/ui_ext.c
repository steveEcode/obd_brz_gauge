// ================================================================
//  ui_ext.c — ui.c 的手写扩展逻辑
//
//  迁移计划: 将 ui.c my_timerMain() 中的手写逻辑逐步移入此文件。
//  每次迁移一个模块 (showroom / boot_anim / sweep / rpm_warn),
//  移入后把对应的 static 变量也搬过来, ui.c 通过 extern 访问。
//
//  已完成迁移:
//  - disp_item 系统 → ui_disp_item.c/h (数据项元数据与 helper)
//
//  当前状态: 框架已建立, ui_ext_tick() 为空, 逻辑仍在 ui.c。
//  TODO: 迁移 showroom 状态机 (~200行)
//  TODO: 迁移 boot animation (~200行)
//  TODO: 迁移 sweep animation (~150行)
//  TODO: 迁移 rpm warning flash (~50行)
// ================================================================

#include "ui_ext.h"
#include "ui.h"
#include "bsp_obd_dsp/nvs_storage.h"
#include "bsp_obd_dsp/espnow_link.h"
#include "app_obd_dsp/obd_data_cache.h"
#include "app_obd_dsp/vehicle_profiles.h"
#include "app_obd_dsp/boot_block_player.h"
#include "app_obd_dsp/boot_media_mount.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_random.h"
#include "lvgl.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ui_ext";

void ui_ext_init(void)
{
    ESP_LOGI(TAG, "ui_ext initialized");
}

void ui_ext_tick(void)
{
    // TODO: 从 ui.c my_timerMain() 迁移以下逻辑到此函数:
    // 1. Showroom 状态机 (showroom_active, slot, tick, sync)
    // 2. 开机动画 (video, RACE/AS/ONE, logo delay)
    // 3. 刷表动画 (sweep_step, backlight)
    // 4. 转速报警闪烁 (rpm_flash)
    //
    // 迁移方法:
    //   a) 将相关 static 变量从 ui.c 剪切到此文件顶部
    //   b) 将相关函数从 ui.c 剪切到此文件
    //   c) ui.c 中需要访问的变量/函数加 extern 声明
    //   d) 在 my_timerMain() 中删除已迁移的代码块
    //   e) 编译验证
}

bool ui_ext_showroom_is_active(void)
{
    // TODO: 迁移后返回 ui_ext.c 内部的 s_showroom_active
    extern bool ui_showroom_is_active(void);
    return ui_showroom_is_active();
}

void ui_ext_showroom_fake_data(void)
{
    // TODO: 迁移 showroom_fake_data 从 ui.c 到此处后, 去掉 extern
    // 当前: ui.c 的 my_timerMain 直接调用 showroom_fake_data(), 此处为空
}

void ui_ext_showroom_sync_slot(uint8_t slot)
{
    // TODO: 迁移后直接操作 ui_ext.c 内部状态
    extern void ui_showroom_set_page_from_sync(int sweep_step);
    ui_showroom_set_page_from_sync(200 + slot);
}

void ui_ext_showroom_request_enter(void)
{
    // TODO: 迁移后直接设置 ui_ext.c 内部的 pending_enter
    extern void ui_showroom_set_active(bool en);
    ui_showroom_set_active(true);
}

int ui_ext_intro_get_step(void)
{
    extern int ui_intro_get_step(void);
    return ui_intro_get_step();
}

void ui_ext_intro_set_step(int step)
{
    extern void ui_intro_set_step(int step);
    ui_intro_set_step(step);
}

int ui_ext_sweep_get_step(void)
{
    extern int ui_sweep_get_step(void);
    return ui_sweep_get_step();
}

void ui_ext_rpm_flash_test(void)
{
    // TODO: 迁移后直接操作 ui_ext.c 内部的测试计数器
}
