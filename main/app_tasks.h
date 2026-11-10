#pragma once
/*
 * app_tasks.h - single source of truth for firmware task policy.
 *
 * Every task name, stack size, priority and core affinity lives in
 * s_task_policy[] in app_main.c; modules only pick an id and call
 * app_task_start(). Modules never hard-code stack/prio/core again.
 *
 * Task lifetime stays with the owner module: per-session workers (video,
 * scan, SD jobs) are still created on demand, they only borrow the policy.
 */

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef enum {
    APP_TASK_LVGL = 0,             /* LVGL event loop, boot lifetime */
    APP_TASK_OBD_POLL_BLE,         /* ELM327-over-BLE single-thread state machine */
    APP_TASK_VIDEO_READER_BOOT,    /* fullscreen playback: SD reader (core 0) */
    APP_TASK_VIDEO_DECODER_BOOT,   /* fullscreen playback: decode/present (core 1) */
    APP_TASK_VIDEO_READER_THEME,   /* composite playback: SD reader (core 0) */
    APP_TASK_VIDEO_DECODER_THEME,  /* composite playback: decode/publish (core 1) */
    APP_TASK_ESPNOW_TX,            /* ESP-NOW master broadcast */
    APP_TASK_ESPNOW_PRES,          /* ESP-NOW slave presence/receive */
    APP_TASK_RC_STREAM,            /* RaceChrono BLE notification stream */
    APP_TASK_OIL_PRESSURE,         /* ADS1115 sampling (V1 board) */
    APP_TASK_THEME_BOOT,           /* transient boot theme preparation */
    APP_TASK_SD_JOB,               /* transient SD copy/import job */
    APP_TASK_THEME_LIB,            /* transient theme library write */
    APP_TASK_STORAGE,              /* storage service worker */
    APP_TASK_PERF_STATS,           /* performance logger */
    APP_TASK_OTA_VALID,            /* one-shot rollback confirmation */
    APP_TASK_OTA_TIMEOUT,          /* OTA session timeout guard */
    APP_TASK_BOOTMEDIA_USB,        /* USB boot-media receiver */
    APP_TASK_THEME_PAGE_PREP,      /* transient theme-navigation image preparation */
    APP_TASK_COUNT
} app_task_id_t;

typedef struct {
    const char *name;   /* FreeRTOS task name */
    uint32_t    stack;  /* stack bytes (internal RAM) */
    uint8_t     prio;   /* FreeRTOS priority */
    int8_t      core;   /* -1 = unpinned, 0/1 = pinned */
} app_task_policy_t;

/* Policy for id, or NULL when the id is out of range / not populated. */
const app_task_policy_t *app_task_policy(app_task_id_t id);

/* Create the task described by id and return its handle (NULL on failure).
 * Never falls back to a different stack/priority or another core. */
TaskHandle_t app_task_start(app_task_id_t id, TaskFunction_t fn, void *arg);
