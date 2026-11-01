# Theme System

English | [简体中文](THEMES.md)

This project has **two coexisting theme systems** for different scenarios:

The App tries Wi-Fi first for theme installation, falls back to USB on Android, and uses Wi-Fi only on iOS. Both paths carry the same `OBDT` stream, receiving, verifying and unpacking into staging before replacement protected by a recovery journal. Failure preserves the current theme; the App does not delete it first. Firmware automatically reboots after confirming installation; the App sends no duplicate reboot. Current-theme deletion uses the same transport selection and requests reboot separately after deletion succeeds; reboot failure only prompts a manual restart.

| | Compiled-in theme (TOML) | Runtime theme (SD card) |
|---|---|---|
| What changes | 8 decorative colors + bezel/needle/dial artwork (reskin, no layout) | Colors + artwork + **custom gauge pages and layout** (layout.json) |
| How it installs | The theme folder compiles into the firmware; reflash | Export a theme ZIP and install to SD over web USB; App WiFi OTA remains compatible |
| How it switches | Fixed by the firmware build; not selectable on the gauge | Install and select through the web UI or App; read from SD after reboot |
| Audience | Reskin built-in pages | Custom page layout and data bindings |
| Source | [themes/](../themes/) | Theme editor (`obd_theme_store`), `<id>.zip` (containing `theme.bin`, rules, and video resources); [contract](../contracts/theme-editor-contract.md) |
| Install paths | Developer rebuild | One-click web USB install, or App OTA (firmware manages the SD library) |
| Takes effect | Reboot | USB install and activation, APPLY, and OTA install automatically reboot |

The systems coexist: when the runtime theme declares pages, boot skips built-in gauge pages
(saving 40–60 KB RAM), and theme pages enter the navigation ring (swipe down from GEAR;
see [USER_GUIDE.en.md](USER_GUIDE.en.md#page-navigation)). A theme without pages only supplies
color defaults for its own elements; built-in pages keep their compiled palette.
No selection, missing card/file, corrupt manifest or parse failure falls back to the built-in
default theme while preserving the saved selection.

Web theme installation and “Delete current theme” both use ESP32-S3 native USB, requiring no device address, token or WiFi OTA. Deletion releases theme pages and resources, removes the current `/OBD/THEMES/<key>/` directory (including `theme.bin`, rules, images, fonts and video) and saved selection, then the web client separately requests reboot when “Delete current theme” is clicked. Installing a new theme instead automatically clears the current theme, skips deletion when none exists, and reboots only after uploading and enabling the replacement. Other themes and the shared boot animation remain. Update to USB-management firmware first; missing SD and deletion failures report errors, while temporary contention can be retried automatically.

---

## 1. Compiled-in themes (TOML)

**No C code required** — one folder, one manifest, one registry line.

```
themes/
├── registry.txt           # slot -> id, append only
├── _TEMPLATE/             # copy this to start
├── builtin/               # shipped with the firmware (default / amber / ocean)
└── community/             # ← your theme goes here
```

### What a theme controls

**Skin only, not layout.** Eight decorative color roles (required) plus three
kinds of artwork (optional). Element positions, font sizes and page
composition are out of scope (they belong to the ~20 `ui_ScreenPageXxx.c`
files); fonts are deliberately excluded — swapping a font changes glyph
metrics, which changes layout.

| Role | Used for |
|------|----------|
| `bg` | Screen / page background |
| `ring` | Outer bezel ring |
| `arc_track` | Gauge arc track, slider groove (the "not reached" part) |
| `arc_indicator` | Gauge arc progress, slider fill (the "reached" part) |
| `text_primary` | Values, main text |
| `text_secondary` | Hints, units, small labels |
| `needle` | Needle-page needle |
| `panel` | Panel / list / roller background |

### What a theme must NOT control

Semantic colors are global and off-limits (`ui_theme.h`) — this is a gauge
bolted into a car, and red must always mean "something is wrong":

| Macro | Value | Meaning |
|-------|-------|---------|
| `UI_SEM_FLASH` | `0xFF0000` | Full-screen RPM warning flash |

### Build one in four steps

```bash
# 1. copy the template
cp -r themes/_TEMPLATE themes/community/sunset

# 2. fill in themes/community/sunset/theme.toml
```

```toml
id   = "sunset"        # must match the folder name
name = "SUNSET"        # Settings roller label, 1-10 uppercase chars

author      = "your-github-handle"
description = "Warm orange face with a deep purple ring."

[colors]
bg             = 0x140800
ring           = 0x6A2C8A
arc_track      = 0x3A1810
arc_indicator  = 0xFF7A18
text_primary   = 0xFFE0C0
text_secondary = 0x9A6440
needle         = 0xFF3020
panel          = 0x2A1408

[bezel]
style_id = 0

# Optional artwork (declare what you have; anything omitted stays drawn + colored)
[assets]
ring   = "assets/ring.png"
needle = "assets/needle.png"
needle_pivot_x = 24      # rotation center (the needle hub), pixels from top-left
needle_pivot_y = 12
dial   = "assets/dial.png"
```

```bash
# 3. register: append one line to registry.txt (append only!)
echo "3  sunset" >> themes/registry.txt

# 4. build
idf.py build flash monitor
```

Compile-time themes ship with the firmware; users cannot switch them on the gauge.
Developers can validate a theme by running the generator standalone (errors point
at the exact file and line):

```bash
python3 tools/gen_themes.py            # regenerate
python3 tools/gen_themes.py --check    # validate only, writes nothing (CI)
```

### Why registry.txt is append-only

NVS stores a **slot number** (`theme_cfg.theme`), not a theme name. Reorder
the file and every later theme shifts a slot → **every upgraded device
silently switches to a different theme**, and local testing never catches it. So:

- Append only — never renumber, reorder or delete a line
- Slot 0 must stay `default`
- To retire a theme, keep both its line and folder; deleting just the folder
  fails the build

The generator enforces all of this at build time and tells you how to fix it.

### Artwork specs

| Asset | Size | Alpha | Compiled size | Replaces |
|-------|------|-------|---------------|----------|
| `ring` | **exactly 360×360** | required (transparent center) | 380 KB | Outer bezel ring |
| `needle` | ≤ 360×360 | required | w×h×3 B | Needle-page needle |
| `dial` | **exactly 360×360** | ignored | 253 KB | Page background |

- **Needle art must point RIGHT (east)**: LVGL angle 0 draws unrotated, so
  art pointing up ends up 90° off
- **Space is a hard constraint**: every registered theme's artwork is linked
  in (`g_ui_themes[]` references them all, so `--gc-sections` cannot drop
  them). A full set is ~645 KB against a total budget of **1536 KB**
  (`gen_themes.py --asset-budget`); exceeding it fails the build with a
  per-file breakdown. ota_0/ota_1 are 3 MB each and the firmware is ~2 MB
- Converting artwork needs Pillow (`pip install Pillow`), but **only when a
  PNG changes** — the converted C files are checked in and the generator
  decides via the source SHA-256; never hand-edit
  `main/export_path/theme_assets/`

### Palette guidance

- Keep `arc_indicator` clearly brighter than `arc_track`, or progress becomes
  unreadable (the generator warns on identical values)
- `text_secondary` dimmer than `text_primary` but still legible at a glance
  while driving
- Keep `bg` very dark — bright backgrounds are harsh at night

### Sharing

Drop the folder into [`themes/community/`](../themes/community/), optionally
with a `preview.png` beside the manifest (a photo of the screen is fine);
open a PR titled `theme: add SUNSET`.

### Framework internals (only for framework changes)

- **Codegen pipeline**: `registry.txt` + `theme.toml` → `tools/gen_themes.py`
  (at CMake configure time, before `idf_component_register`) →
  `main/export_path/ui_theme_generated.c` + `theme_assets/theme_*.c`.
  `registry.txt` and every `theme.toml` are listed in
  `CMAKE_CONFIGURE_DEPENDS`, so adding a theme always triggers regeneration;
  output is rewritten only when content changes
- **Theme state**: `ui_theme.c` owns the active theme and NVS persistence;
  `ui_init()` calls `ui_theme_init()` before any screen is built. Screens
  read colors via `ui_theme_color_lv(role)` / `ui_theme_color(role)` — never
  hard-code `lv_color_hex()`
- **Adding a color role**: append to `ui_color_role_t` in `ui_theme.h`
  (before `UI_COLOR__COUNT`) → `COLOR_ROLES` in `gen_themes.py` (same order)
  → every `theme.toml` (a manifest missing the role fails the build instead
  of silently defaulting to black)
- **Adding an artwork kind**: add an `ASSET_KINDS` entry in
  `gen_themes.py` + append the field to `ui_theme_t` + give the consumer a
  NULL fallback
- **Reserved fields**: `ui_theme_t` already carries `dial_face` / `font` /
  `bezel` (NULL today); new capabilities are always appended at the end of
  the struct — designated initializers zero-fill, so old themes keep
  compiling
- **Format details**: `CONFIG_LV_COLOR_16_SWAP=y` means RGB565 is emitted
  big-endian `(hi, lo)`; the needle can never use `LV_IMG_CF_ALPHA_8BIT`
  (undefined colors on the rotated draw path), so all artwork uses
  `TRUE_COLOR_ALPHA` / `TRUE_COLOR`. `theme.toml` is parsed by a built-in
  restricted-TOML parser (ESP-IDF ships Python 3.8 without `tomllib`);
  supported: comments, `[section]`, `key = value` (strings / decimal / 0x
  hex); anything else is an error with a line pointer

---

## 2. Runtime themes (SD card)

Firmware supplies data (`signal_store`), the theme engine and system pages (Settings/OTA/Bluetooth).
Custom images, fonts, layouts and theme videos are read from SD, with no Flash media cache. Boot
animation media is independent of the selected theme and is read only from the shared SD boot
directory. The legacy web uploader writes to that same directory.
Built-in default UI and compiled themes remain in firmware for fallback.
NVS stores the selected key; the SD sidecar is authoritative for rules and label overrides. A legacy config sidecar is migrated once when the SD copy is missing.

```text
/OBD/THEMES/<key>/
  theme.bin
  sidecar.json
  rpm.obv                  # optional ordinary theme video
/OBD/BOOT/boot.obv         # boot video (existing web upload workflow)
/OBD/BOOT/boot.jpg         # optional boot still
/OBD/BOOT/bootmedia.raw    # legacy boot animation
/OBD/VIDEO/rpm.obv        # optional shared fallback
```

`theme.bin` contains the 16 KB manifest, images/fonts and page layouts. Images/fonts must still
use the binary package format; arbitrary PNG/TTF files are not loaded directly. Theme videos
belong in their theme directory first; `/OBD/VIDEO` is an optional shared fallback directory.
VIDEO mode reads `/OBD/BOOT/boot.obv`, then `/OBD/BOOT/boot.jpg`, then legacy
`/OBD/BOOT/bootmedia.raw`. The existing web upload workflow continues to write to `/OBD/BOOT/`,
removing other old-format boot animations after the new file commits. Transfer interruption or validation failure preserves existing animations; old-file cleanup failure is reported as an error.
A still is displayed for 2.5 seconds.
Legacy raw animation duration is computed from `frame_count/fps` rather than manifest `duration_ms`, avoiding an extra last-frame hold when metadata disagrees.
JPEGs must be baseline and no larger than 360×360.
Packages no longer have the old 4 MB partition limit; SD space, format count
limits and runtime RAM budgets still apply.

### Protected boot pages

The following pages are **never themed** and always use the firmware
implementation; listing them under `theme_pages` is skipped and logged:

| Page ID | Content |
|---------|---------|
| `logo` | Sky Gauge logo (brand consistency) |
| `intro` | RACE AS ONE boot animation (NVS `intro_enable`: 0=OFF 1=RACE 2=VIDEO) |
| `boot_video` | Protected boot-media controller; reads shared SD media independently of the theme; player: `media/boot_video` |

The boot controller remains protected firmware code and is independent of theme selection. OBV1
files are played only after full validation. Missing or corrupt shared SD media falls back to the
built-in startup screen. Applying a theme does not change boot media.

### theme.bin format

`theme.bin` is exported by the theme editor (`obd_theme_store`), not a
hand-written directory; the firmware only knows the file's own binary layout
(code: `main/theme_engine/theme_loader.c`, `theme_internal.h`):

```
[0, 16 KB)   manifest JSON (parsed with cJSON_ParseWithLength, 0xFF-padded tail)
[16 KB, …)   images + fonts + per-page layout (offset/size given by the manifest)
```

Key fields and limits:

- First byte `0xFF` or `0x00` → the theme is treated as unavailable, built-in
  default theme is used
- `schema_version` must equal `"1.0"` exactly, otherwise
  `ESP_ERR_NOT_SUPPORTED` and the default theme is used
- `theme.id` is required, ≤64 chars; `name` / `version` / `author` are
  truncated on UTF-8 character boundaries when too long
- `assets.*`: `format` is `rgb565` / `rgba8888` / `lv_font_bin`, unknown
  formats are skipped entirely; width/height ≤2047 px; `size` must equal
  `width*height*bytes-per-pixel`; offset ≥16 KB; duplicate names keep the first
- `pages.theme_pages[]`: `{id, layout_data_offset, layout_data_size}`, one
  page's layout ≤64 KB; duplicate ids keep the first; ids `logo` / `intro` /
  `boot_video` are skipped per the previous section
- Colors fall back to the built-in default palette when missing
- Capacity: ≤32 pages, ≤16 images, ≤16 fonts; ≤128 elements and ≤32 data
  bindings per page

Fonts are `lv_font_bin`. The 'F' filesystem driver reads the font segment from SD into temporary
PSRAM during `lv_font_load`, then frees the segment on close; LVGL font objects remain until
unload. Failure logs "font '%s': lv_font_load failed, built-in font used" and uses a built-in font.
`theme_load()` refuses reload while theme page objects exist or a background boot-page
preparation is active. Theme switching/deletion waits for image reads to finish so it
cannot race the SD files used by the worker.

### Theme mode: with pages vs. without (`main/export_path/ui.c`)

The built-in gauge pages (Gear/Rpm/Speed/Temp/OilPressure/Needle) are skipped
only when the theme's **id is not `default` and it declares at least one
`theme_pages` entry** (saving 40–60 KB RAM); boot goes straight to the
theme's default page (the page with id `main_gauge`, else the first page),
falling back to the built-in default page if building it fails. Inside the
theme page carousel, swiping left/right moves between theme pages, and past
either end lands on the Info (EasterEgg) page; swiping down from the Gear
page also opens the theme's first page (it only requires a non-empty
`theme_pages`). The default page's images are read and converted by a background
task during the Logo / boot animation, leaving the boot screen visible. The
boot video replaces the Logo only after its first frame finishes transferring to the LCD.
If the video ends before theme preparation, playback releases its buffers and the gauge
returns to the Logo until the page is ready. Fullscreen playback guards the
LVGL refresh callback so page or layout invalidations cannot resume rendering, clear the
panel or overwrite borrowed DMA buffers. Normal refresh resumes after the decoder releases
the panel and buffers. LVGL creates the theme
widgets after video playback and preparation finish, then switches pages, so
neither transparent-image conversion nor widget creation interrupts video. If preparation finishes before playback starts, widgets can be created during the Logo. Theme-package streaming uses file-descriptor `read` calls and an internal DMA buffer for bulk transfers, avoiding byte-by-byte refills from `_IONBF` + `fread` on this target. Slow-read logs separate buffer waits, actual reads and callbacks. A
boot-video decode failure or interruption restores the Logo; failed theme
preparation falls back to the built-in page. Images, videos and other theme child widgets
bubble gestures to the page root, so swiping anywhere in the theme content
continues the carousel. Full navigation: [USER_GUIDE.en.md](USER_GUIDE.en.md#page-navigation).

LCD output also includes the upstream ESP-IDF 5.5.3 SPI bus-lock wakeup fix, preventing a cross-core request from waiting indefinitely at DMA interrupt exit. The low-speed LCD probe handle is released before the normal handle is registered. Only a build-local SDK source copy is replaced; the installed SDK is unchanged. On the tested device, preparation of the same 360×360 RGBA image takes about 238–240 ms; 20 consecutive USB resets each completed the 100-frame OBV2 video and entered the theme page. These measurements apply to that device and those resources, not every SD card, image or video.

**A theme with no pages**: in the current code, `colors.*` only supplies
default colors for the theme's own page elements — moot when it has no
pages. It does **not** change the built-in pages' colors; those always use
the compiled-in theme palette.

### Sidecar (rules + overlay)

Editor rules (alarms / recoloring / animation) and label overlays (rotation,
direction, numeric display format) do not go into `theme.bin`; they are kept
as a separate slim JSON:

- Location: SD `/OBD/THEMES/<key>/sidecar.json` (`<key>` =
  `storage_theme_key(theme.id)`), the only authoritative copy. Install/delete requires an SD card.
  A legacy config partition `/themes/<key>/sidecar.json` is migrated once when the SD file is
  missing, then the old copy is removed
- Input is either the reduced format itself (`"format":"gauge-sidecar/1"`) or
  a full `<id>.gaugeproj.json` (streamed and filtered, dropping large fields
  like images/fonts as it reads)
- Limits (`theme_sidecar_core.h`): reduced form ≤64 KB; ≤32 pages; ≤128
  element ids per page; one element id ≤128 chars; one element/rule/overlay
  entry ≤16 KB; overlong strings (>4096) are dropped while streaming
- Optional `asset_files`: asset name → gaugeproj file name, used by `vid_` /
  `video_` elements to switch videos with an `asset` rule
- Install failures: `ESP_ERR_INVALID_ARG` (bad JSON / format / theme id),
  `ESP_ERR_INVALID_SIZE` ("sidecar too big: reduced form exceeds 64 KB"),
  `ESP_ERR_NO_MEM`
- Install returns a pairing report: page/rule counts, truncation counts,
  `not in theme.bin: ...`, per-page matched/missing/kind-mismatch, unknown
  assets/sources, elements expected to be skipped by the PSRAM budget (up to
  8 names); the last line always reads "applies on the next page build;
  bin-only installs (app/store/web flash) have no overlay"; an overlong
  report ends in "..."
- API: `POST /api/v1/theme/sidecar` (chunked; ≤64 KB kept in PSRAM, larger
  streams to an SD temp file; an SD card is required for every save, otherwise
  409 `sd_unavailable`; 32 MB overall cap, 413 if exceeded; 422 `invalid_sidecar`
  on bad input; success `{"ok":true,"size":N,"report":"..."}`),
  `DELETE /api/v1/theme/sidecar[?theme=<id>]` (no parameter removes the
  current theme's). `/api/v1` requires `X-OTA-Token`. SD import of
  `*.gaugeproj.json` is installed the same way as a sidecar (`sd_import.c`)

### PSRAM budget and skip order

Building a page sizes its artwork budget by whether it has a video element
(`theme_img_cache.h`): **no video 2560 KB, with video 1536 KB**. Allocation
order when over budget:

1. Pointers and all their variants — required, kept
2. Rule-target images (images a rule can change the look of) — required,
   kept; their tint variants also count against the budget
3. Decorative images, bottom-to-top in stacking order, topmost dropped
   first when the budget runs out; decorative images must also leave 512 KB
   of PSRAM free (`TE_PSRAM_RESERVE`)

Dropped images log "'%s' skipped: over the page budget / PSRAM reserve". All images, including
untinted `rgb565`, own RAM pixels read from SD and count against the cache budget; Flash mmap
zero-copy is retired. Cache misses also need temporary source data and possibly resize buffers,
so package size differs from page RAM use. Tint variants over 128 KB (`TE_TINT_BAKE_MAX`) use
runtime recoloring instead of pre-baking.

### SD theme library

`/OBD/THEMES/<key>/theme.bin` is the firmware-managed runtime source. Users install and select
themes through the web UI or App and do not need direct SD-card access.
`storage_theme_key(theme.id)` generates
`<key>` (unsafe characters, leading/trailing dots and DOS reserved names handled; ≤64 chars).
Copy directly into that directory or import `*.bin` through `/OBD/IMPORT/`, leaving a `.ok` note.
WiFi OTA also installs directly to SD. Old Flash themes are migration sources only; existing
SD packages and selections are preserved.

The theme editor's `*-sd.zip` export is **not directly readable by firmware**. On a computer,
import the archive through the web UI or App; users do not need to organize SD files manually.
The archive places `theme.bin` in
`/OBD/THEMES/<key>/`, the rules sidecar in `/OBD/IMPORT/`, and theme videos beside the package;
firmware imports the sidecar and reads the package and videos directly from the SD theme folder.
Do not put the ZIP itself in `/OBD/IMPORT/`. Theme videos are exported by asset key and keep the
names referenced by the manifest. Theme images, fonts and videos are stored on SD; images and fonts
are inside `theme.bin`, while ordinary theme videos sit beside it. The editor's Media panel can use
an image or video for preview, but startup media is not part of the theme bundle. The existing web
uploader remains compatible and writes the independent boot media to `/OBD/BOOT/`.

Themes are selected through the web UI or App; the gauge has no theme picker or SD management page.
The following describes the firmware's internal theme-application behavior:

1. Deletes theme pages and calls `theme_unload()`.
2. Shows "Loading theme..." / "Loading theme NN%"; a worker validates the SD package and commits the NVS selection, polled every 500 ms.
3. Shows "Theme selected, restarting" and reboots after about 1.5 s to read SD directly.
4. On failure shows "Theme selection failed: <reason>" and reloads the old selection or built-in default.
5. Busy shows "Busy, try again"; an unusable package shows "Not a usable theme.bin".

Switching does not erase/write Flash media or copy the entire package into PSRAM. OTA, no card
or another storage job prevents switching. Interrupted old Flash writes become SD selections;
without a card the marker waits for retry. Ordinary SD import cannot replace the selected package:
switch themes first. OTA can replace it after theme pages are released. Boot without a card uses
the built-in UI; apply again after insertion. Removing a card during use may leave cached images
visible, but later page/asset reads fail; immediate whole-UI fallback is not guaranteed.

### Theme video

Master and slave roles use two 20-line LCD DMA buffers, saving 28,800 bytes of internal RAM compared with standalone mode's 40-line buffers to leave room for ESP-NOW and on-demand video tasks. Standalone mode still prefers 40-line buffers. Video task stacks remain in internal RAM; task-creation failures report the requested stack size, free internal RAM and largest contiguous block.

A theme image element whose id starts with `vid_` or `video_` is treated as a
video element: the image itself is the poster, and the key is the id with
that prefix stripped. Lookup order is
`/OBD/THEMES/<theme_key>/<key>.obv`, then `/OBD/VIDEO/<key>.obv` (for sharing
across themes); if neither exists, the poster stays. A `visible` rule can pause the video, and an
`asset` rule switches to another file named by the sidecar's `asset_files`
(no folder or extension). Theme videos loop by default or select frames from a bound numeric signal. Only one video decodes at a time: the topmost visible video layer with nonzero opacity owns the player, while the others show their posters.
Theme directory keys and video keys support up to 64 characters; the player path buffer accommodates
the complete SD path for a theme video.

Looping theme videos target approximately 2.5 seconds of compressed-frame SD prefetch plus two frames, with a 2 MiB ring ceiling; short clips do not allocate more than the space needed for the complete stream. Before allocating, reserve PSRAM for both display buffers, the OBV3 temporary alpha plane and an additional 512 KiB, reducing the ring when memory is limited. Actual buffered duration depends on compressed bitrate and available memory. Initial playback still starts at approximately 400 ms of buffered frames, a full ring or end of input, rather than waiting for the entire cache to fill. After playback has started, a theme video resumes as soon as the next frame arrives after a ring underrun, rather than waiting to refill the 400 ms watermark each time, avoiding repeated playback bursts and pauses under sustained slow reads. Looping playback follows the source timeline and applies a frame-rate cap only when `target_fps` is explicitly set; the default 24 fps ceiling and coarse every-second, third or fourth-frame tiers are removed. After waiting for the UI to accept the previous frame, recheck its deadline and skip only frames more than one source interval late; sustained severe lateness rebases the clock to keep the image updating. Longer prefetch absorbs SD read jitter but cannot increase hardware JPEG decoding or alpha-compositing throughput; value-driven videos and OBV2 boot animations retain their existing buffering and timing policies.

Theme-video reader and decoder workers run below LVGL and the OBD poller, so continuous composite playback cannot starve both real-time tasks and leave UI handoff stuck or live signals stale; the PSRAM ring still provides read-ahead, and a full ring blocks the reader until the consumer releases space. Fullscreen boot playback keeps its dedicated high-priority path because it writes the panel directly. Video files use file-descriptor `read` calls into an internal DMA buffer before copying to the PSRAM ring. The player prefers a 16 KiB buffer and falls back through 8, 4, 2, 1 and 0.5 KiB if the internal DMA heap is fragmented; it reports free space plus the largest contiguous block if all attempts fail. Frame padding and seeks within that buffer preserve read-ahead; only out-of-range seeks reset it, avoiding byte-by-byte stdio refills in the target newlib and repeated loss of prefetched data. Boot animations can decode in blocks and send them directly to the panel through DMA; theme videos decode full frames and pass through LVGL to composite with other layers, so buffering duration alone does not explain their performance. An OBV3 frame with a valid, entirely 255 alpha plane stays in RGB565 without per-pixel alpha expansion; genuinely transparent frames still use RGB565+alpha. A video can switch formats frame by frame, and existing themes need no re-export. Accepting a video frame in the UI immediately readies the display refresh timer, avoiding an additional ordinary refresh interval. Approximately every five seconds, the player logs the `queued` frame rate, decode time, UI wait time and cumulative dropped frames / underruns; `queued` measures decoder publication rather than actual panel refresh FPS; `UI frame gap` records the average / maximum interval between UI frame handoffs to identify long stalls, and is not a count of completed LCD refreshes. The same interval also logs `SD` active-read throughput, average / maximum per-frame read time and frame / byte counts; read timing includes seeks and waits for the SD buffer, excludes ring-space waits, and throughput uses accumulated read time.

When videos switch rapidly, the next reader waits for the previous reader to release its file and DMA buffer. This wait happens in the background, without blocking the UI thread on SD I/O. If the SD card is unavailable or the video asset name is invalid, the poster remains visible and playback can resume when the asset is restored.

Theme videos use the custom **OBV1/OBV3** containers (`main/media/obv1_format.h`).
Legacy or external opaque videos use OBV1, stored as
`[64 B header][8 B × frame_count frame index][JPEG frame data]` (version 1). New editor imports
use OBV3, with each frame stored as
`[u32 JPEG size][u32 alpha size][JPEG][PackBits alpha plane]`. Width/height are 8..360
and multiples of 8; there are up to 36000 frames. OBV1 JPEG frames are ≤128 KiB and
OBV3 frames are ≤256 KiB, checked by CRC-32. Firmware decodes OBV3 only in the composite
theme player and combines JPEG color with alpha as LVGL RGB565+alpha. OBV1 and existing
themes remain compatible. Web boot-animation uploads use OBV2 below. Commands:

The theme editor's toolbar Import Video button opens a configuration dialog. Local FFmpeg WebAssembly converts videos to 360×360 at 24 fps while preserving the source alpha channel in a separate PackBits plane in OBV3. Opaque sources also use OBV3, whose constant alpha plane compresses efficiently. Posters are composed as PNG, and the editor canvas and firmware player render per-pixel transparency. Supported encodings depend on the browser FFmpeg build; damaged or unsupported inputs report an error. Source video is not uploaded.

The editor no longer has a separate resource-library entry. Images and videos imported from the toolbar appear directly on the canvas and in the layer list, and are included in the theme ZIP. Images and videos with the same filename can be imported repeatedly: after decoding, the editor allocates a unique internal identifier against the current project (adding `_2`, `_3`, etc. for duplicate video names), preserving existing media and event references. Reimporting after deleting a layer also requires no manual file renaming. Click the theme name at the top to edit metadata; select a text element and use the font-property control to import or manage custom fonts.

The editor follows Design → Data → Events → Preview, with contextual instructions for each step. Insertion tools have visible text labels. With nothing selected, the inspector offers RPM, speed and coolant starter components with bindings already configured; adding them preserves existing content and is undone in one step. Design properties show component names and appearance first, with precise coordinates and internal IDs under Position and Advanced Settings. The data inspector can bind static text directly to vehicle signals; numeric transformations and text mappings are collapsed by default. Expand the bottom simulator in Preview and drag values to check behavior. The canvas fits the available space automatically, with manual zoom and a reset-to-fit control. Display options, dimensions and zoom occupy separate toolbar rows, while alignment and arrangement are collapsed by default to prevent overlap in narrower windows.

Editor drafts save the project and video files together in browser IndexedDB; after “Saved” appears, reloading restores them, and save failures are shown explicitly. Save downloads a `<id>.gaugeproj.zip` editable backup containing images, fonts and videos; Open restores this archive or a legacy `.gaugeproj.json`. Project ZIPs are for editing, while exported theme ZIPs are for device installation. Missing videos in old drafts or JSON files each get a Relink Video button: select the source video or a theme OBV1 `.obv` to retain layer positions and event settings. Theme exports require and package only videos referenced by layers or events; unreferenced records retained after deleting layers no longer block export.

The import dialog shows conversion progress, the current stage, percentage and frame counts when available, covering decoding, frame encoding and theme-video generation. Stages without measurable totals, such as loading the decoder, show an indeterminate bar. Success reveals the timeline preview; failures show their cause, and duplicate additions are disabled while processing. Relinking a video still shows progress above the canvas.

#### Value-driven animation

In the import dialog, choose Loop or Follow Value. For the latter, select a signal, start value, end value and numeric step; the end must exceed the start, and the step cannot be negative. Aligned video and numeric timelines show thumbnails and the corresponding value. Scrub or play to check the mapping, then choose Add to Canvas. Select an existing video layer to edit its binding and inspect the timeline in its properties.

The start value selects the first frame, the end value the last, and intermediate values select the nearest proportional frame. Decreasing values move backward, and out-of-range values clamp to the endpoints. Invalid data holds the last position, initially the first frame. Both editor simulation and the firmware BLE connection sweep drive bindings; visibility, opacity and asset-switch events still apply to the layer. The device reads and decodes target frames asynchronously and may skip obsolete targets during rapid changes; displaying every intermediate frame is not guaranteed. Value changes that still map to the same frame no longer wake the SD reader and decoder. Frame changes, including rapid round trips, still notify the workers, reducing unnecessary scheduling from small driver-input fluctuations. This does not increase source frame rate or change existing target-frame deduplication.

Step `0` uses all existing frames. A positive step first rounds the value to the nearest increment from the start, then selects a frame; the endpoint always remains reachable. For example, bind a six-second video to `0–6000` RPM with step `100`: `3000` RPM selects a frame near the midpoint and `6000` RPM selects the last frame. A step does not create frames: six seconds at the imported 24 fps has approximately 144 frames, and available images still depend on the source and conversion.

Layout JSON adds these optional fields to a video image element (geometry and poster fields omitted below):

```json
{
  "type": "image",
  "id": "vid_rpm",
  "video_playback": "value",
  "data_source": "obd.rpm",
  "range_min": 0,
  "range_max": 6000,
  "video_step": 100
}
```

Without `video_playback: "value"`, existing loop behavior is preserved; bindings apply only to `vid_` / `video_` image elements. `data_source` uses a standard signal name, while `range_min`, `range_max` and `video_step` use raw signal units. The web controls use display units; for example, raw voltage `120` is shown as `12.0 V`. Invalid ranges do not enable value-driven playback. Both theme playback modes use indexed OBV1; OBV2 remains exclusive to fullscreen boot animations.

```bash
python3 tools/make_video.py boot INPUT -o boot.obv            # boot video, defaults 360px/4:2:2/Q75/24fps
python3 tools/make_video.py theme INPUT -o rpm.obv --size 240x240 [--poster rpm.png]
python3 tools/make_video.py inspect boot.obv                  # print the header
python3 tools/make_video.py verify boot.obv                   # check CRCs and every frame
python3 tools/make_video.py legacy INPUT -o bootmedia/slot_a [--raw bootmedia.raw]  # old delta_varint boot animation
```

Needs Pillow (`pip install Pillow`); video inputs also need ffmpeg.

Local web boot animation and new community uploads use **OBV2**; theme videos continue to use OBV1. OBV2 keeps the `OBV1` magic and 64 B header,
with `version=2`, `fps=0`, and `timescale=1000000`; each 16 B index entry is `offset,size,timestamp_us,duration_us`.
Timestamps start at 0 and are contiguous, frame intervals are ≥16666 µs, and `duration_ms` rounds the total duration up.
Limits are 36000 frames, ≤128 KiB per frame, ≤256 MiB per file, and ≤1.25 MiB for a sliding one-second compressed window plus wrap headroom. Frames larger than 48 KiB require firmware with the 128 KiB frame limit; existing smaller-frame animations remain compatible.
Header, index, and frame data each have CRC-32. The browser converts videos to 360×360 Q90 baseline JPEG 4:2:0, retaining the source timeline by default.
Local USB import preserves aspect ratio without cropping; community uploads apply crop, zoom and trim settings and allow an explicit frame rate. Budget overflow is reported. Firmware prebuffers about one second of compressed frames and reads SD sequentially.
360×360/4:2:0 uses whole-frame decoding and two DMA strip buffers for display output; MCU-aligned assets with a passing decoder self-test can use block decoding.
OBV2 supports only non-looping fullscreen boot playback. When decoding or SD reads fall behind, only intermediate frames whose entire display interval has expired are skipped.
The first and last frames are retained while catching up on the source timeline; consecutive late frames no longer abort playback. Actual read or decode errors still stop playback. These are format limits; actual frame rate still requires device testing.
USB `BEGIN_OBV=7` stages, validates, and installs `/OBD/BOOT/boot.obv`. The tool's `boot/theme` commands still produce OBV1;
`inspect/verify` read both versions, and legacy BIN/raw animations remain compatible.

### Install / push

The theme editor merges the former BIN and SD-bundle exports into **Export theme ZIP**, downloading one `<id>.zip`.
The ZIP contains `/OBD/THEMES/<key>/theme.bin`, the theme's `*.obv` videos, and
`/OBD/IMPORT/<key>.gaugeproj.json`; images, fonts, and layouts are in `theme.bin`, while rules come from gaugeproj.
Export is blocked when referenced video files need relinking. Boot animation is independent and stays under shared `/OBD/BOOT/`.

Recommended workflow: insert an SD card, connect the ESP32-S3 native USB port to the web flasher, and choose **From community** or
**Upload local files** in the **Theme** section. The page checks paths, CRCs, theme IDs, data ranges, and video dependencies, unpacks it, detects and deletes the current theme directory and selection over USB, then uploads files in order. When no current theme exists, upload proceeds directly. Installation stops if deletion is not confirmed.
Firmware verifies SHA-256, stages files, and generates `sidecar.json`; after all steps succeed it saves the theme selection and restarts.
No manual extraction or separate activation is needed. First install firmware with USB theme-bundle support; older firmware prompts an upgrade.
Devices with an existing theme can upload a replacement directly. Switching from flashing mode to the application first restarts the gauge; cleanup then waits for boot theme preparation or lock contention,
retrying every 500 ms with a two-minute waiting budget and a visible waiting message. Manual deletion is unnecessary. Firmware replies with `retryable:true` may be retried;
legacy replies without that field are also supported when the exact error is `theme delete failed` or `display busy`. Legacy errors cannot distinguish startup contention from actual deletion failure; persistent failure stops at the deadline and prompts a firmware upgrade for detailed diagnostics.
New firmware includes failure stage `stage` and error code `code`; explicit `retryable:false`, other business failures or communication errors stop installation immediately. Upload BEGIN retains its separate 30-second storage-contention wait.

New complete ZIPs, old `*-sd.zip` files, and STORE / DEFLATE compression are supported. Legacy BIN, companion gaugeproj / sidecar JSON,
and `.obv` files can also be selected together. A BIN alone retains its images, fonts, and layouts but cannot restore missing rules or videos;
upload is rejected if a declared video is missing. New community publications store complete ZIPs; historical BIN files remain installable,
with format detected from content. After a complete firmware flash, reconnect before installing themes or animations.

Updates to an existing theme ID are supported: before commit, release active theme resources, then replace the whole directory using staging,
a backup, and a recovery journal. Failed installation rolls back; if power fails during commit, the next boot with SD mounted attempts to restore
old files and selection. Before upload, at least `2 × upload stream size + 1 MiB` of free space is required. Both the web ZIP and upload stream
are limited to 256 MiB, with at most 128 installed files. Host tests cover directory replacement, validation failure, selection rollback, and
interruption recovery; they do not establish actual SD-card power-loss reliability.
Web replacement deletes the current theme before upload, so the installation transaction cannot restore that deleted package. If upload fails or is interrupted after cleanup, the next restart uses the built-in default theme; upload again to install a replacement.

Alternatively extract the ZIP at the SD root, preserving existing files. Rules are imported automatically at boot, but copying does not activate the theme.
Web theme installation uses ESP32-S3 native USB; the App WiFi OTA path remains compatible.

**WiFi OTA push** (enter OTA mode; protocol:
[APP_PROTOCOL.en.md](APP_PROTOCOL.en.md#wifi-ota-http-api)):

- `POST /ota/theme/prepare` — check SD mount and space; returns `storage:"sd"`. PSRAM fields are diagnostic.
- `POST /ota/theme` — stream to an SD temporary file, verify SHA and manifest, import, save selection and automatically reboot.
- `POST /ota/theme/erase` — delete the selected theme's entire `/OBD/THEMES/<key>/` directory (including `theme.bin`, rules, images, fonts, and dedicated videos) and clear selection; the built-in theme loads on next boot. Requires a mounted SD card; other themes and the shared `/OBD/BOOT/` boot animation are untouched. Returns `{"ok":true,"deleted":true|false}`.

While theme selection runs these endpoints and the first `/ota/bootmedia` chunk return
"theme library busy". Themes transfer via USB, SD, or WiFi; no BLE theme upload. Without a card,
media upload returns "SD card required".

### Runtime behavior

Image and video properties in the editor expose 0–100% opacity. The event panel summarizes each rule as “When / Action / Restore”; opacity events directly expose the base value, target value and transition duration in seconds, with the base value shared with layer properties. Browser previews can play multiple videos concurrently, retain the poster until the first frame decodes, and keep the previous frame while attempting subsequent frames after a decode failure.

The browser event and preview workspaces advance debounce and transitions on an independent wall clock. Even while the preview timeline is paused, manually changing simulated RPM or other data triggers events once their conditions and debounce are satisfied; releasing the condition restores the layer’s base properties. Pausing the timeline still pauses time for timer triggers.

The preview rule status distinguishes disabled, waiting and active rules and displays data conditions, current simulated values and action targets. Disabled rules have an explicit enable button in the preview. Event-list status dots only indicate state; clicking them does not toggle enablement. Use the enable control in the event details. To fade a video out when RPM reaches 3500, enable the rule, set its condition to `obd.rpm >= 3500`, set the video’s base opacity to `100` and the action target opacity to `0`, and select a fade transition.

- At boot `theme_engine_init()` reads the SD package selected in NVS: missing card/file,
  corrupt manifest or parse failure falls back to the built-in default.
- While a theme page is on screen, the LVGL task normally calls `theme_tick()`
  every 16 ms (animations/alarms use their respective refresh periods): it reads `signal_store` once (keys defined in
  `contracts/signals.json`), updates data bindings (only changed values touch
  LVGL objects), then evaluates sidecar rules on that same snapshot and
  applies the diff; it is a no-op with no theme page on screen
- While a theme page is on screen during the Bluetooth-connect gauge sweep, numeric labels (including custom fonts) and rule evaluation share a simulated snapshot of all 12 standard
  `obd.*` signals, so displayed numbers, threshold conditions and `change` rules for every signal can respond to the sweep.
  Binding and rule inputs immediately return to real `signal_store` data when the sweep ends; simulated values are never written to or used
  to overwrite real OBD data. Vehicle-specific `veh.*` signals have no shared range and continue using their live data during sweep
- Sweep rules still follow their actual condition interval and animation duration: during an approximately one-second ramp from 0 to 8000 rpm, `obd.rpm <= 3000` holds for only about 0.4 seconds at the start, which cannot complete a one-second fade. Use `obd.rpm >= 3500` to fade out above 3500 rpm. Uploading a theme ZIP updates theme resources, not firmware. Page-creation logs include rule and target counts; sweep start and active-rule changes log `sweep rules` with simulated RPM, active mask, target count and alarm suppression, and sweep end logs the return to live inputs.
- Rule conditions/hysteresis/debounce/timing state still advance on each call. An unchanged
  active set reuses the sorted action coverage; changes rebuild it and remove overrides of
  rules that exit. Rebuilding a page or theme clears the cache.
  When both the set and system-alarm suppression stay unchanged, target look assembly/comparison
  is skipped as well. Bindings continue updating; rule exits, suppression changes, and the first
  update after reset still apply looks. Ending an alarm still restores the page background.
- For arcs and bars, rule-action `set.color` changes only the indicator/fill color; the track retains the element's `bgColor`. Immediate changes and color animations follow the same rule, so crossing a sweep threshold does not light the unfilled track and appear to jump to full scale. When the rule exits, the original indicator color returns. Existing theme packages only need a firmware update, with no re-export.
- Arc progress uses the full 0–10000 value range without the 8191 pixel-coordinate limit; both sweep and live readings can reach full scale.
- Rule-action `set.color` supports `animation` modes `fade`, `keyframes` and `strobe`, updating colors
  or image tint on each theme tick. `change` and `timer` rules remain active through the longest
  animation duration. Frames are not pre-generated; `text`, `visible` and `asset` still change
  immediately, and image-resource changes are not crossfaded.
- Image and video layers accept a layout `opacity` from 0 to 100 (default 100). Rule-action
  `set.opacity` selects a target opacity; `animation.mode: "fade"` and `durationMs` interpolate to it
  over the specified duration. When the condition clears, opacity fades back to the layer's base
  value over the same duration. Theme videos share one JPEG decoder: firmware plays only the topmost
  visible, non-transparent, unpaused video. Hiding the upper layer or setting its opacity to 0 hands
  playback to the lower video, and also hides the upper video's poster so it cannot mask that layer;
  every video poster is kept in required image-cache priority so lower video layers retain a static
  fallback even when decorative images exceed the cache budget. Two videos cannot be decoded
  simultaneously or live-crossfaded.
- Label fonts: try the manifest's `font_asset` first, fall back to built-in
  Montserrat 12/14/16/26/32/48 on failure (exact match first, else the
  nearest smaller size, else 12 as a last resort); sizes above 48 use the 48
  font with `transform_zoom = size*256/48` (LVGL's zoom, unrelated to the
  compiled-in theme's artwork scaling)
- A missing value or NaN shows `--` on the label (no unit)
- System alarms take priority over theme rules: while the RPM warning flash
  or the triple-gauge linked red owns the background, theme rules are
  suppressed (data bindings keep refreshing); when it ends, built-in pages
  get their palette background + dial face back, and theme pages get their
  own layout's `bg_color` back
- Layouts and assets are read from SD in segments; cache hits do not read SD. Image conversion,
  cropping, tinting and pointer resizing use PSRAM. Identity-size images still own RAM pixels;
  referenced cache entries remain alive. Font/layout source segments are freed after parsing.
  SD lock waits are bounded to one second; failure ends the current load. Host tests cover
  pixel/geometry compatibility and read/allocation cleanup, not physical SD/LCD performance.
- API reference: [main/theme_engine/theme_interface.h](../main/theme_engine/theme_interface.h)

### Partition background

Physical layout v2 is preserved: `theme_0` (0x620000, 4 MB) and `bootmedia` (0xA20000, 4.75 MB)
are legacy migration sources / reserved space totaling 8.75 MiB. New themes and animations do
not use them; this change does not reassign the space to firmware or config.
See [partitions.csv](../partitions.csv). Devices already on layout v2 can update by OTA;
older partition tables need one full reflash because OTA cannot change the table.

## Video decode ahead and double buffering

Playback retains sequential SD read-ahead, a PSRAM compressed-frame ring and a persistent JPEG decoder. Fullscreen mode decodes the next frame's first MCU strip or whole image ahead of time and starts panel transfer at its source deadline. Two internal DMA strips alternate by parity; submission waits for the previous transfer and never overwrites an in-flight buffer. Composite themes retain front/back frame buffers and LVGL handoff. OBV2 also decodes ahead, retaining compressed frames and shifting the timeline on pause; expired frames follow existing skip rules. Decode timing excludes deadline sleep. Actual FPS still requires device measurement; host tests do not establish a fixed hardware frame rate.

## Arc and bar track opacity

Runtime arc/bar layouts accept optional `bg_opacity` (0–100, default 100, clamped). Zero hides the track and 100 keeps it opaque. Arcs use LVGL arc opacity on `LV_PART_MAIN`; bars set only the container background opacity, leaving the indicator arc, foreground bar and child object opacity unchanged. Editor projects, previews and exports use the same track opacity. Older themes retain opaque defaults; older firmware ignores the new field, so transparent tracks require upgraded firmware.

Community publication and ZIP export now share image/video sizing, including layer-sized video posters. Videos padded to multiples of 8 use integer centering consistent with device cropping. Older community packages whose assets differ from layout sizes must be exported/published again from the original editable project; firmware upgrades do not rewrite those files. Automatic community cover PNG pixels are clipped to the round dial.
