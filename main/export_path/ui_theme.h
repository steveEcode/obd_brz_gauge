#pragma once
// ============================================================
//  ui_theme.h — theming framework
//
//  A theme bundles the DECORATIVE look of the gauge (colors, bezel
//  style, and later dial-face image / font). SEMANTIC colors such as
//  alert-red or toggle-on-green are global and never themed, so a red
//  warning always reads as a warning regardless of skin.
//
//  Adding a new theme (for any developer) — NO C CODE REQUIRED:
//    1. cp -r themes/_TEMPLATE themes/community/<your-id>
//    2. Edit theme.toml (id, name, eight colors).
//    3. Append one line to themes/registry.txt.
//    4. Rebuild — it appears in Settings > THEME.
//  See themes/README.md. tools/gen_themes.py turns the manifests into
//  ui_theme_generated.c at CMake configure time; never edit that file.
//
//  The struct is designed to grow: new capability fields are appended at
//  the end and zero-initialised, so existing theme instances keep
//  compiling unchanged.
// ============================================================

#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Decorative color roles every theme provides.
typedef enum {
    UI_COLOR_BG = 0,          // screen / page background
    UI_COLOR_RING,            // outer bezel ring
    UI_COLOR_ARC_TRACK,       // gauge arc track / slider groove
    UI_COLOR_ARC_INDICATOR,   // gauge arc progress
    UI_COLOR_TEXT_PRIMARY,    // values / main text
    UI_COLOR_TEXT_SECONDARY,  // hints / units / small labels
    UI_COLOR_NEEDLE,          // meter needle
    UI_COLOR_PANEL,           // panel / list / roller background
    UI_COLOR__COUNT
} ui_color_role_t;

// Semantic colors — identical across ALL themes. Do not theme these.
#define UI_SEM_ALERT   0xFF4D4D   // alarm / over-threshold text
#define UI_SEM_FLASH   0xFF0000   // full-screen RPM flash background
#define UI_SEM_ON      0x06D6A0   // toggle ON / positive state
#define UI_SEM_WARN    0xFFD166   // caution (oil pressure etc.)

// Bezel (outer ring) style. Reserved for future variants — today only the
// ring color is themed (see ui_helpers_create_ring). Extend here later with
// e.g. a style id, double ring, or tick decoration. Zero = default.
typedef struct {
    uint8_t style_id;         // reserved: 0 = plain solid ring
} ui_bezel_style_t;

// A theme = name + decorative colors + bezel style, plus reserved extension
// points for future dial-face image and font (NULL = use default behavior).
typedef struct {
    const char *id;                      // stable id, matches themes/registry.txt
    const char *name;                    // display name (Settings roller)
    uint32_t colors[UI_COLOR__COUNT];    // decorative colors, 0xRRGGBB
    ui_bezel_style_t bezel;              // bezel / ring style
    const lv_img_dsc_t *dial_face;       // dial-face background (NULL = plain UI_COLOR_BG)
    const lv_font_t    *font;            // reserved: font override (NULL = default)
    // ---- appended after the initial release; keep new fields at the end ----
    const lv_img_dsc_t *ring_img;        // bezel artwork (NULL = drawn ring in UI_COLOR_RING)
    const lv_img_dsc_t *needle_img;      // needle artwork (NULL = drawn line in UI_COLOR_NEEDLE)
    int16_t needle_pivot_x;              // needle rotation center, px from artwork top-left
    int16_t needle_pivot_y;              // only meaningful when needle_img != NULL
} ui_theme_t;

// ---- generated theme table ----
// Defined in ui_theme_generated.c. The array index IS the slot number stored
// in NVS (theme_cfg.theme), pinned by themes/registry.txt — which is why that
// file is append-only: reordering it re-skins every existing device on update.
extern const ui_theme_t *const g_ui_themes[];
extern const uint8_t g_ui_theme_count;

// ---- registry / active theme ----
void ui_theme_init(void);                 // call once at boot; loads saved index from NVS
uint8_t ui_theme_count(void);             // number of registered themes
const ui_theme_t *ui_theme_get(uint8_t idx);
const ui_theme_t *ui_theme_active(void);  // currently active theme
void ui_theme_set_active(uint8_t idx);    // persist selection to NVS (caller restarts to apply)

// Newline-separated theme names for lv_roller_set_options(), built once on
// first call and sized exactly — so the roller can never truncate, however
// many themes are registered.
const char *ui_theme_names_joined(void);

// ---- convenience accessors (read from the active theme) ----
uint32_t   ui_theme_color(ui_color_role_t role);     // 0xRRGGBB
lv_color_t ui_theme_color_lv(ui_color_role_t role);  // ready for lv_obj_set_style_*

#ifdef __cplusplus
}
#endif
