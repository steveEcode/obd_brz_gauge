#pragma once

#include <stdbool.h>
#include <stddef.h>

// On failure, reason receives a static stage-specific message (also safe with NULL).
// Does not commit/remove the staged file or alter the installed animation.
bool boot_obv_validate_file(const char *path, const char **reason);

// Recommended caller-owned capacity for a complete diagnostic message.
#define BOOT_OBV_DETAIL_SIZE 480
// Preserves the static reason contract above. On failure, detail receives the
// reason plus frame/IO/parser/decoder context; on success it is empty. NULL and
// zero-capacity buffers are supported; smaller buffers are safely truncated.
bool boot_obv_validate_file_detailed(const char *path, const char **reason,
                                    char *detail, size_t detail_size);
