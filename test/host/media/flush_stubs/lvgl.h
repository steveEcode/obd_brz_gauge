#pragma once
#include <stdatomic.h>

typedef struct { _Atomic int flushing; } lv_disp_draw_buf_t;
typedef struct { lv_disp_draw_buf_t *draw_buf; } lv_disp_drv_t;
void lv_disp_flush_ready(lv_disp_drv_t *drv);
