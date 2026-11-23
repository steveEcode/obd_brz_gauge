#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Caller-owned, task-scoped diagnostics. No shared "last error": a background
 * probe must never be attributed to an upload. Pair begin/end, including errors. */
typedef struct sd_io_trace {
    struct sd_io_trace *next;
    void *owner;
    esp_err_t error;
    uint32_t sector, count, khz;
    size_t internal_free, internal_largest, dma_largest;
    char operation;
} sd_io_trace_t;

void sd_io_guard_init(void);
void sd_io_trace_begin(sd_io_trace_t *trace);
void sd_io_trace_end(sd_io_trace_t *trace);
/* Copies the current task's first scoped SD failure, if any. */
int sd_io_trace_current(sd_io_trace_t *trace);
/* Appends a bounded, NUL-terminated suffix; returns no text on success. */
void sd_io_trace_format(const sd_io_trace_t *trace, char *out, size_t capacity);
