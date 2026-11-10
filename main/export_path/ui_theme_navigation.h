#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* LVGL task/mutex only. Keep the current theme page live while a worker
 * prepares target images. Only one request may be in flight. */
bool ui_theme_navigation_request(uint8_t index);
bool ui_theme_navigation_busy(void);

/* Invalidate the result without deleting a running worker or its resources.
 * The completion timer disposes of the job after the worker returns. */
void ui_theme_navigation_cancel(void);

#ifdef __cplusplus
}
#endif
