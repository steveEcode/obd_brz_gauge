#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * ESP-IDF esp_random.h compatibility for the desktop simulator.
 * This is sufficient for UI effects and animation timing.
 */
static inline uint32_t esp_random(void)
{
    static uint32_t state = 0x9E3779B9u;

    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;

    return state;
}

static inline void esp_fill_random(void *buffer, size_t length)
{
    uint8_t *out = (uint8_t *)buffer;

    while (length > 0) {
        uint32_t value = esp_random();

        for (size_t i = 0; i < sizeof(value) && length > 0; ++i) {
            *out++ = (uint8_t)(value >> (i * 8));
            --length;
        }
    }
}
