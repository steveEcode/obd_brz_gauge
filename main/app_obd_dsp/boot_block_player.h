#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

// Create the boot_block player (creates an LVGL canvas on parent)
// Returns true on success; out_obj outputs the canvas object
bool boot_block_player_create(lv_obj_t *parent, lv_obj_t **out_obj);

// Destroy the player and free all resources
void boot_block_player_destroy(void);

// Whether playback has finished
bool boot_block_player_is_finished(void);

// Get the canvas size
bool boot_block_player_get_canvas_size(uint16_t *out_width, uint16_t *out_height);

// Call every frame with the milliseconds elapsed since boot to advance the animation
void boot_block_player_update(uint32_t elapsed_ms);

// Get the total animation duration (ms)
uint32_t boot_block_player_get_duration_ms(void);

// Set the manifest and data file paths (call before create)
void boot_block_player_set_paths(const char *manifest, const char *data);
