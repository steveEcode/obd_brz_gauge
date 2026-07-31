#pragma once
// ================================================================
//  fsm.h — 轻量状态机框架
//
//  用法:
//    static const fsm_state_t my_states[] = {
//        [ST_IDLE]  = { .on_enter = idle_enter, .on_tick = idle_tick },
//        [ST_ACTIVE]= { .on_enter = active_enter, .on_tick = active_tick, .on_exit = active_exit },
//    };
//    static fsm_t my_fsm = { .states = my_states, .state_count = 2 };
//    fsm_init(&my_fsm, ST_IDLE);
//    // 每 100ms:
//    fsm_tick(&my_fsm);
//    // 切换状态:
//    fsm_goto(&my_fsm, ST_ACTIVE);
// ================================================================

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fsm_s fsm_t;

typedef struct {
    void (*on_enter)(fsm_t *fsm);
    void (*on_tick)(fsm_t *fsm);
    void (*on_exit)(fsm_t *fsm);
} fsm_state_t;

struct fsm_s {
    const fsm_state_t *states;
    uint8_t state_count;
    uint8_t current;
    uint32_t tick_count;     // 当前状态已持续的 tick 数
    int64_t  enter_us;       // 进入当前状态的时间戳 (us)
    void    *user_data;      // 用户自定义上下文
};

static inline void fsm_init(fsm_t *fsm, uint8_t initial_state)
{
    fsm->current = initial_state;
    fsm->tick_count = 0;
    fsm->enter_us = 0;
    if (fsm->states[initial_state].on_enter)
        fsm->states[initial_state].on_enter(fsm);
}

static inline void fsm_goto(fsm_t *fsm, uint8_t new_state)
{
    if (new_state >= fsm->state_count || new_state == fsm->current) return;
    if (fsm->states[fsm->current].on_exit)
        fsm->states[fsm->current].on_exit(fsm);
    fsm->current = new_state;
    fsm->tick_count = 0;
    fsm->enter_us = 0;  // 下次 tick 时设置
    if (fsm->states[new_state].on_enter)
        fsm->states[new_state].on_enter(fsm);
}

static inline void fsm_tick(fsm_t *fsm)
{
    if (fsm->enter_us == 0) {
        extern int64_t esp_timer_get_time(void);
        fsm->enter_us = esp_timer_get_time();
    }
    fsm->tick_count++;
    if (fsm->states[fsm->current].on_tick)
        fsm->states[fsm->current].on_tick(fsm);
}

static inline uint32_t fsm_elapsed_ms(const fsm_t *fsm)
{
    if (fsm->enter_us == 0) return 0;
    extern int64_t esp_timer_get_time(void);
    return (uint32_t)((esp_timer_get_time() - fsm->enter_us) / 1000);
}

#ifdef __cplusplus
}
#endif
