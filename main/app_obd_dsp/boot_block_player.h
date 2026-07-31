#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

// 创建 boot_block 播放器 (在 parent 上创建 LVGL canvas)
// 返回 true 表示成功, out_obj 输出 canvas 对象
bool boot_block_player_create(lv_obj_t *parent, lv_obj_t **out_obj);

// 销毁播放器, 释放所有资源
void boot_block_player_destroy(void);

// 是否播放完毕
bool boot_block_player_is_finished(void);

// 获取 canvas 尺寸
bool boot_block_player_get_canvas_size(uint16_t *out_width, uint16_t *out_height);

// 每帧调用, 传入开机以来经过的毫秒数, 推进动画
void boot_block_player_update(uint32_t elapsed_ms);

// 获取动画总时长 (ms)
uint32_t boot_block_player_get_duration_ms(void);

// 设置 manifest 和 data 文件路径 (在 create 之前调用)
void boot_block_player_set_paths(const char *manifest, const char *data);
