#pragma once
#include <stdbool.h>
bool esp_ptr_dma_capable(const void *ptr);
bool esp_ptr_external_ram(const void *ptr);
