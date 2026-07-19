#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 由桌面模拟器主循环调用。
 * 真机 ESP32 不使用此接口。
 */
void espnow_link_simulator_update(uint32_t elapsed_ms);

void espnow_link_simulator_shutdown(void);

#ifdef __cplusplus
}
#endif
