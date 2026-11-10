#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef uint16_t lv_color_t;
typedef struct { int unused; } lv_obj_t;
struct lv_timer_t;
typedef void (*lv_timer_cb_t)(struct lv_timer_t *);
typedef struct lv_timer_t {
    void *user_data;
    lv_timer_cb_t timer_cb;
    bool paused;
    bool ready;
} lv_timer_t;
typedef struct { void *buf1, *buf2; uint32_t size; volatile int flushing; } lv_disp_draw_buf_t;
typedef struct { lv_disp_draw_buf_t *draw_buf; lv_timer_t *timer; lv_obj_t *screen; } lv_disp_t;
lv_disp_t *lv_disp_get_default(void);
lv_disp_draw_buf_t *lv_disp_get_draw_buf(lv_disp_t *disp);
lv_timer_t *_lv_disp_get_refr_timer(lv_disp_t *disp);
lv_obj_t *lv_disp_get_scr_act(lv_disp_t *disp);
void lv_obj_invalidate(lv_obj_t *obj);
lv_timer_t *lv_timer_create(void (*cb)(lv_timer_t *), uint32_t period, void *data);
void lv_timer_del(lv_timer_t *timer);
void lv_timer_set_cb(lv_timer_t *timer, lv_timer_cb_t cb);
void lv_timer_pause(lv_timer_t *timer);
void lv_timer_resume(lv_timer_t *timer);
void lv_timer_ready(lv_timer_t *timer);

uint32_t lv_timer_handler(void);

uint32_t lv_tick_get(void);
uint32_t lv_tick_elaps(uint32_t previous);
