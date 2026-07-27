#pragma once

#include <stdint.h>
#include <time.h>

/*
 * ESP-IDF esp_timer compatibility for the desktop simulator.
 * Returns monotonic time in microseconds.
 */
static inline int64_t esp_timer_get_time(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return ((int64_t)ts.tv_sec * 1000000LL) +
           ((int64_t)ts.tv_nsec / 1000LL);
}
