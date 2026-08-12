# UI Theming Guide

The gauge supports selectable UI themes (Settings → THEME, applied on reboot).
This document explains how the theme system is organized internally.

> **Adding a theme needs no C code and none of this page.** Copy a folder, fill
> in a manifest, append one registry line: see [`../themes/README.md`](../themes/README.md)
> (bilingual 中文/English). This document is for people changing the *framework*.

## Architecture

Themes are **declared as data and compiled in via codegen**. Authors touch only
`themes/`; the C side is generated.

```
themes/registry.txt              slot -> id  (append-only, pins the NVS value)
themes/*/<id>/theme.toml         per-theme manifest
themes/*/<id>/assets/*.png       optional artwork
        │
        ├── tools/gen_themes.py              (CMake configure step, validates + emits)
        ▼
main/export_path/ui_theme_generated.c        GENERATED — never edit
main/export_path/theme_assets/theme_*.c      GENERATED artwork — never edit
```

| File | Role |
|------|------|
| `themes/registry.txt` | Slot → id map. Append-only; the index is what NVS stores |
| `themes/*/<id>/theme.toml` | Theme manifest (id, name, eight colors, bezel, artwork) |
| `themes/*/<id>/assets/*.png` | Source artwork — the reviewable source of truth |
| `tools/gen_themes.py` | Validates manifests, converts artwork, emits the C |
| `main/export_path/ui_theme_generated.c` | Generated theme definitions + `g_ui_themes[]` |
| `main/export_path/theme_assets/` | Generated LVGL image arrays, one file per artwork |
| `main/export_path/ui_theme.h` | Theme struct, color-role enum, semantic colors, public API |
| `main/export_path/ui_theme.c` | Active-theme state, accessors, NVS persistence |
| `main/export_path/ui_helpers.c` | `ui_helpers_create_ring()`, `ui_helpers_style_screen_bg()` |

- Generation runs at **CMake configure time**, before `idf_component_register`
  (which globs `SRC_DIRS`). `registry.txt` and every `theme.toml` are listed in
  `CMAKE_CONFIGURE_DEPENDS`, and adding a theme necessarily edits `registry.txt`,
  so a new theme always triggers regeneration.
- The generator only rewrites the output when the content actually changes, so an
  unchanged reconfigure does not force a rebuild of everything downstream.
- The active theme is stored in the existing `theme_cfg_t.theme` byte in NVS
  (previously unused). No NVS layout change was needed.
- `ui_init()` calls `ui_theme_init()` **before** any screen is built, so every
  screen picks up the active theme's colors at creation time.
- Changing the theme saves the selection and reboots (`esp_restart`). Screens are
  created once at boot, so a reboot is the simplest reliable way to re-skin all of
  them.

### Why the slot registry exists

NVS stores a **slot number**, not an id string. If theme order were derived from
directory listing, adding a theme would shift later slots and silently re-skin
every existing device on the next OTA — invisible in local testing. `registry.txt`
pins slot → id, and `gen_themes.py` hard-fails on renumbering, reordering, gaps,
a non-`default` slot 0, or a registry line whose folder is missing.

The `id` field exists for this stability contract (and boot logging); the runtime
still selects by slot.

## Color roles

Each theme provides these **decorative** colors (`ui_color_role_t`):

| Role | Used for |
|------|----------|
| `UI_COLOR_BG` | Screen / page background |
| `UI_COLOR_RING` | Outer bezel ring |
| `UI_COLOR_ARC_TRACK` | Gauge arc track, slider groove, minor ticks |
| `UI_COLOR_ARC_INDICATOR` | Gauge arc progress, slider fill/knob |
| `UI_COLOR_TEXT_PRIMARY` | Values / main text |
| `UI_COLOR_TEXT_SECONDARY` | Hints / units / small labels |
| `UI_COLOR_NEEDLE` | Meter needle |
| `UI_COLOR_PANEL` | Panel / list / roller background |

### Semantic colors (NOT themed)

Warning/positive colors are global so their meaning never changes with the skin:

| Macro | Meaning |
|-------|---------|
| `UI_SEM_ALERT` | Alarm / over-threshold text |
| `UI_SEM_FLASH` | Full-screen RPM flash background |
| `UI_SEM_ON` | Toggle ON / positive state |
| `UI_SEM_WARN` | Caution (oil pressure etc.) |

Keep using these for anything that conveys status — do **not** route them through
the theme.

## How to add a new theme

See [`../themes/README.md`](../themes/README.md). Summary:

```bash
cp -r themes/_TEMPLATE themes/community/sunset
$EDITOR themes/community/sunset/theme.toml   # set id + name + eight colors
echo "3  sunset" >> themes/registry.txt      # append only
idf.py build
```

No C file is edited at any point. Validate without building:

```bash
python3 tools/gen_themes.py            # regenerate
python3 tools/gen_themes.py --check    # verify the checked-in file is fresh (CI)
```

## Changing the framework

**Adding a color role** — edit three places, in this order:

1. `ui_color_role_t` in `ui_theme.h` (append before `UI_COLOR__COUNT`)
2. `COLOR_ROLES` in `tools/gen_themes.py` (same order as the enum)
3. Every `theme.toml`, including `_TEMPLATE` — the generator hard-fails on a
   manifest missing a role, so nothing can silently default to black

**Adding a capability field** (image, font, …) — append to the end of
`ui_theme_t`, extend the manifest schema and the emitter in `gen_themes.py`.
Existing manifests keep working because omitted struct fields are zero-filled.

**Adding an artwork kind** — add an entry to `ASSET_KINDS` in `gen_themes.py`
(color format, required size, target struct field), add the field to
`ui_theme_t`, and give the consuming widget a NULL fallback so colour-only
themes keep working.

## Artwork pipeline notes

Two LVGL details constrain the format choice, both verified in the vendored
sources under `managed_components/lvgl__lvgl/`:

- **Byte order.** `CONFIG_LV_COLOR_16_SWAP=y`, so RGB565 must be emitted
  big-endian `(hi, lo)`. `tools/convert_rpm_flash.py` does this; the older
  `tools/png_to_lvgl.py` writes little-endian and is not a template to copy.
- **`LV_IMG_CF_ALPHA_8BIT` cannot be used for the needle.** It is 3× smaller and
  tintable from the theme color (`lv_draw_sw_img.c` sets
  `blend_dsc.color = draw_dsc->recolor`), but `lv_draw_img.c` falls back to the
  per-pixel path for any rotated or zoomed image, and the built-in decoder only
  fills the alpha byte there — a rotated ALPHA_8BIT needle draws with undefined
  colors. All artwork therefore uses `TRUE_COLOR_ALPHA` / `TRUE_COLOR`, which
  are correct on every draw path. Switching the *static* ring to ALPHA_8BIT is a
  safe future optimization; the needle can never use it.

Artwork is compiled in, and everything in `g_ui_themes[]` is referenced, so
`--gc-sections` cannot drop unused themes' art. `gen_themes.py` therefore
enforces a total budget (`--asset-budget`, default 1536 KB) and prints usage on
every build. When that becomes the binding constraint, move `dial` artwork into
the `bootmedia` SPIFFS image and load only the active theme's at boot — the
manifest format would not change, since packaging is the generator's concern.

Conversion imports Pillow lazily and only when a source PNG's SHA-256 differs
from the one recorded in the generated file, so ordinary builds need no
third-party Python. `--check` verifies freshness without writing; orphaned
generated artwork is deleted (only files carrying the generator's marker).

**The manifest parser** is a ~60-line restricted-TOML reader in `gen_themes.py`,
not `tomllib` — ESP-IDF ships Python 3.8+, where `tomllib` (3.11+) is absent.
Supported: comments, `[sections]`, `key = value` with quoted strings and
decimal/hex ints. Anything else is an error with a `file:line` pointer.

## Reserved extension points

`ui_theme_t` already carries two reserved fields for future capabilities — they are
`NULL` today and safe to ignore:

```c
const lv_img_dsc_t *dial_face;   // future: dial-face background image
const lv_font_t    *font;        // future: font override
ui_bezel_style_t    bezel;       // future: bezel/ring style variants
```

The struct is grown by **appending fields at the end**; existing theme instances
keep compiling because designated initializers zero-fill omitted fields. This is
what lets different developers add bezel styles, background images, or fonts
independently without stepping on each other.

## Conventions for contributors

- Route new decorative colors through an existing `ui_color_role_t` where possible;
  add a new role only when nothing fits, and update **every** theme definition.
- Never theme the `UI_SEM_*` semantic colors.
- Screens should read colors via `ui_theme_color_lv(role)` /
  `ui_theme_color(role)` instead of hard-coding `lv_color_hex(...)`.
- The bezel ring must be created with `ui_helpers_create_ring()` so it follows the
  theme. (Gear/Rpm/Speed previously had inline rings; they now use the helper.)
