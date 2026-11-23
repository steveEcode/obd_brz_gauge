#pragma once
#include <stdint.h>
#include <stdlib.h>
#define MALLOC_CAP_DEFAULT 1u
#define MALLOC_CAP_DMA 2u
#define MALLOC_CAP_INTERNAL 4u
#define MALLOC_CAP_SPIRAM 8u
#define MALLOC_CAP_8BIT 16u
void *heap_caps_malloc(size_t size, uint32_t caps);
void *heap_caps_calloc(size_t count, size_t size, uint32_t caps);
void heap_caps_free(void *ptr);
void *heap_caps_aligned_alloc(size_t align, size_t size, uint32_t caps);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
