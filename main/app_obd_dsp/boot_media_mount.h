#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 挂载 bootmedia 分区 (SPIFFS), 返回 true 表示成功
bool boot_media_mount(void);

// 卸载
void boot_media_unmount(void);

// 检查 boot_block 媒体文件是否存在
bool boot_media_has_block_video(void);

#ifdef __cplusplus
}
#endif
