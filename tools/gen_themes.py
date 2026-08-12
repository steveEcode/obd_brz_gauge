#!/usr/bin/env python3
"""Generate the LVGL theme table from themes/registry.txt + themes/*/*/theme.toml.

Run automatically at CMake configure time; also runnable by hand:

    python3 tools/gen_themes.py                 # regenerate
    python3 tools/gen_themes.py --check         # verify the checked-in files are fresh

The manifest format is a deliberately RESTRICTED subset of TOML (sections,
`key = value`, quoted strings, decimal/hex ints). It is parsed by the small
parser below rather than `tomllib` so the generator works on the Python that
ESP-IDF ships (3.8+), where `tomllib` does not exist.

Artwork (PNG) is converted to LVGL C arrays under
main/export_path/theme_assets/. Conversion needs Pillow, but only when a PNG
actually changed: each generated file records the SHA-256 of its source, so a
normal build of unmodified themes never imports PIL.
"""

import argparse
import hashlib
import re
import struct
import sys
from pathlib import Path

# Color roles, in the exact order of ui_color_role_t in ui_theme.h.
# Manifest key -> C enum name. Adding a role means editing BOTH files.
COLOR_ROLES = [
    ("bg", "UI_COLOR_BG"),
    ("ring", "UI_COLOR_RING"),
    ("arc_track", "UI_COLOR_ARC_TRACK"),
    ("arc_indicator", "UI_COLOR_ARC_INDICATOR"),
    ("text_primary", "UI_COLOR_TEXT_PRIMARY"),
    ("text_secondary", "UI_COLOR_TEXT_SECONDARY"),
    ("needle", "UI_COLOR_NEEDLE"),
    ("panel", "UI_COLOR_PANEL"),
]

# Round display size. Full-screen artwork must match it exactly.
SCREEN_W, SCREEN_H = 360, 360

# Artwork kinds a theme may declare in [assets].
#
#   cf     LVGL color format of the emitted array
#   bpp    bytes per pixel of that format at LV_COLOR_DEPTH=16
#   size   required pixel size, or None for "any, up to the screen"
#   field  the ui_theme_t member that receives the pointer
#
# `ring` and `needle` keep an alpha channel so artwork can be any shape.
# LV_IMG_CF_ALPHA_8BIT would be 3x smaller and tintable by the theme color,
# BUT lv_draw_img.c falls back to a per-pixel path for rotated images and the
# built-in decoder only fills the alpha byte there — a rotated ALPHA_8BIT
# needle renders with undefined colors. TRUE_COLOR_ALPHA is correct on every
# draw path, so both use it. (An ALPHA_8BIT option for the static ring is a
# safe future optimization; the needle can never use it.)
ASSET_KINDS = {
    "ring": {
        "cf": "LV_IMG_CF_TRUE_COLOR_ALPHA", "bpp": 3,
        "size": (SCREEN_W, SCREEN_H), "field": "ring_img",
    },
    "needle": {
        "cf": "LV_IMG_CF_TRUE_COLOR_ALPHA", "bpp": 3,
        "size": None, "field": "needle_img",
    },
    "dial": {
        "cf": "LV_IMG_CF_TRUE_COLOR", "bpp": 2,
        "size": (SCREEN_W, SCREEN_H), "field": "dial_face",
    },
}

ASSET_EXTRA_KEYS = {"needle_pivot_x", "needle_pivot_y"}

TOP_KEYS_REQUIRED = {"id", "name"}
TOP_KEYS_OPTIONAL = {"author", "description"}
BEZEL_KEYS_OPTIONAL = {"style_id"}

ID_RE = re.compile(r"^[a-z0-9_-]+$")
NAME_RE = re.compile(r"^[A-Z0-9]{1,10}$")

# NVS stores theme_cfg.theme as a uint8.
MAX_SLOTS = 256

# Ceiling on the total size of compiled-in artwork, summed over ALL themes.
# The app partition is 4 MB and the firmware is ~2 MB, so this leaves room for
# code growth. Artwork is linked unconditionally (every registered theme is
# referenced by g_ui_themes[], so --gc-sections cannot drop it). When this
# starts to bite, the fix is to move `dial` artwork into the bootmedia SPIFFS
# image and load the active theme's at boot — the manifest would not change.
DEFAULT_ASSET_BUDGET = 1536 * 1024

SCAN_DIRS = ("builtin", "community")
SKIP_NAMES = {"_TEMPLATE"}

GEN_MARKER = "GENERATED-BY: tools/gen_themes.py"


class ThemeError(Exception):
    """A user-facing validation failure. Message is printed without a traceback."""


def fail(path, line, msg):
    where = f"{path}:{line}" if line else str(path)
    raise ThemeError(f"{where}: {msg}")


def warn(msg):
    print(f"gen_themes: warning: {msg}", file=sys.stderr)


# ---------------------------------------------------------------- manifest parse

def _split_value(raw, path, lineno):
    """Return the value text with any trailing comment removed."""
    raw = raw.strip()
    if not raw:
        fail(path, lineno, "missing value after '='")
    if raw[0] == '"':
        end = raw.find('"', 1)
        if end < 0:
            fail(path, lineno, "unterminated string (missing closing quote)")
        trailing = raw[end + 1:].strip()
        if trailing and not trailing.startswith("#"):
            fail(path, lineno, f"unexpected text after string: {trailing!r}")
        return raw[1:end]
    hash_pos = raw.find("#")
    if hash_pos >= 0:
        raw = raw[:hash_pos].strip()
    if not raw:
        fail(path, lineno, "missing value after '='")
    return raw


def parse_manifest(path):
    """Parse the restricted-TOML subset into {'': {...}, 'colors': {...}, ...}."""
    out = {"": {}}
    section = ""
    for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if stripped.startswith("["):
            if not stripped.endswith("]"):
                fail(path, lineno, f"malformed section header: {stripped!r}")
            section = stripped[1:-1].strip()
            if not section:
                fail(path, lineno, "empty section header")
            if section in out:
                fail(path, lineno, f"duplicate section [{section}]")
            out[section] = {}
            continue
        if "=" not in stripped:
            fail(path, lineno, f"expected 'key = value', got {stripped!r}")
        key, raw = stripped.split("=", 1)
        key = key.strip()
        if not key:
            fail(path, lineno, "missing key before '='")
        if key in out[section]:
            fail(path, lineno, f"duplicate key {key!r}")
        out[section][key] = (_split_value(raw, path, lineno), lineno)
    return out


def as_int(section, key, path):
    """Parse a manifest int (decimal or 0x-hex)."""
    text, lineno = section[key]
    try:
        return int(text, 0), lineno
    except ValueError:
        fail(path, lineno, f"{key}: expected a number (e.g. 0xRRGGBB), got {text!r}")


# ---------------------------------------------------------------- PNG helpers

def png_size(path):
    """Read (width, height) from a PNG IHDR without needing Pillow."""
    with path.open("rb") as fp:
        head = fp.read(24)
    if len(head) < 24 or head[:8] != b"\x89PNG\r\n\x1a\n" or head[12:16] != b"IHDR":
        raise ThemeError(f"{path}: not a PNG file")
    return struct.unpack(">II", head[16:24])


def sha256_of(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def recorded_sha(c_path):
    """Return the src-sha256 recorded in a previously generated asset file."""
    if not c_path.is_file():
        return None
    try:
        with c_path.open("r", encoding="utf-8") as fp:
            for _ in range(12):
                line = fp.readline()
                if not line:
                    break
                if "src-sha256:" in line:
                    return line.split("src-sha256:", 1)[1].strip()
    except OSError:
        return None
    return None


def rgb888_to_565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def convert_png(asset):
    """PNG -> LVGL C array. Needs Pillow; only called when the source changed."""
    try:
        from PIL import Image
    except ImportError:
        raise ThemeError(
            f"{asset['png']}: converting artwork needs Pillow, which is not installed.\n"
            f"  pip install Pillow\n"
            f"(Only needed when artwork changes — the generated C is checked in, so\n"
            f" building unmodified themes never requires it.)")

    has_alpha = asset["bpp"] == 3
    img = Image.open(asset["png"]).convert("RGBA" if has_alpha else "RGB")
    w, h = img.size
    px = img.load()
    sym = asset["symbol"]
    attr = sym.upper()

    rows = []
    for y in range(h):
        parts = []
        for x in range(w):
            if has_alpha:
                r, g, b, a = px[x, y]
            else:
                r, g, b = px[x, y]
                a = None
            # LV_COLOR_16_SWAP=1 in sdkconfig -> RGB565 stored big-endian (hi, lo).
            c = rgb888_to_565(r, g, b)
            parts.append(f"0x{(c >> 8) & 0xFF:02X}, 0x{c & 0xFF:02X}")
            if a is not None:
                parts.append(f"0x{a:02X}")
        rows.append("  " + ", ".join(parts) + ",")

    out = [
        "// ============================================================",
        f"//  {GEN_MARKER}",
        f"//  src: {asset['src_rel']}",
        f"//  src-sha256: {asset['sha']}",
        "//",
        "//  DO NOT EDIT. Regenerate with: python3 tools/gen_themes.py",
        f"//  {w}x{h}, {asset['cf']}, {asset['data_size']} bytes",
        "//  LV_COLOR_16_SWAP=1 -> RGB565 byte order is (hi, lo).",
        "// ============================================================",
        "",
        '#ifdef __has_include',
        '    #if __has_include("lvgl.h")',
        "        #ifndef LV_LVGL_H_INCLUDE_SIMPLE",
        "            #define LV_LVGL_H_INCLUDE_SIMPLE",
        "        #endif",
        "    #endif",
        "#endif",
        "",
        "#if defined(LV_LVGL_H_INCLUDE_SIMPLE)",
        '#include "lvgl.h"',
        "#else",
        '#include "lvgl/lvgl.h"',
        "#endif",
        "",
        f"#ifndef LV_ATTRIBUTE_IMG_{attr}",
        f"#define LV_ATTRIBUTE_IMG_{attr}",
        "#endif",
        "",
        f"const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST "
        f"LV_ATTRIBUTE_IMG_{attr} uint8_t {sym}_map[] = {{",
    ]
    out.extend(rows)
    out.extend([
        "};",
        "",
        f"const lv_img_dsc_t {sym} = {{",
        f"  .header.cf = {asset['cf']},",
        "  .header.always_zero = 0,",
        "  .header.reserved = 0,",
        f"  .header.w = {w},",
        f"  .header.h = {h},",
        f"  .data_size = sizeof({sym}_map),",
        f"  .data = {sym}_map,",
        "};",
    ])
    return "\n".join(out) + "\n"


# ---------------------------------------------------------------- validation

def load_theme(folder, themes_dir):
    path = folder / "theme.toml"
    if not path.is_file():
        raise ThemeError(f"{folder}: missing theme.toml")

    doc = parse_manifest(path)
    top = doc[""]

    unknown = set(top) - TOP_KEYS_REQUIRED - TOP_KEYS_OPTIONAL
    if unknown:
        fail(path, top[sorted(unknown)[0]][1],
             f"unknown key(s): {', '.join(sorted(unknown))}. "
             f"Allowed: {', '.join(sorted(TOP_KEYS_REQUIRED | TOP_KEYS_OPTIONAL))}")
    for key in sorted(TOP_KEYS_REQUIRED - set(top)):
        fail(path, None, f"missing required key: {key}")

    unknown_sections = set(doc) - {"", "colors", "bezel", "assets"}
    if unknown_sections:
        fail(path, None, f"unknown section(s): {', '.join(sorted(unknown_sections))}. "
                         f"Allowed: [colors], [bezel], [assets]")

    theme_id, id_line = top["id"]
    if not ID_RE.match(theme_id):
        fail(path, id_line, f"id {theme_id!r} must be lowercase letters/digits/'-'/'_'")
    if theme_id != folder.name:
        fail(path, id_line,
             f"id {theme_id!r} must match the folder name {folder.name!r}")

    name, name_line = top["name"]
    if not NAME_RE.match(name):
        fail(path, name_line,
             f"name {name!r} must be 1-10 uppercase ASCII letters/digits "
             f"(it is rendered in the Settings roller)")

    if "colors" not in doc:
        fail(path, None, "missing [colors] section")
    colors_sec = doc["colors"]
    known_color_keys = {k for k, _ in COLOR_ROLES}
    unknown = set(colors_sec) - known_color_keys
    if unknown:
        fail(path, colors_sec[sorted(unknown)[0]][1],
             f"unknown color role(s): {', '.join(sorted(unknown))}. "
             f"Allowed: {', '.join(k for k, _ in COLOR_ROLES)}")
    colors = {}
    for key, _enum in COLOR_ROLES:
        if key not in colors_sec:
            fail(path, None, f"[colors] missing required role: {key}")
        value, lineno = as_int(colors_sec, key, path)
        if not 0 <= value <= 0xFFFFFF:
            fail(path, lineno, f"{key}: 0x{value:X} out of range (0x000000-0xFFFFFF)")
        colors[key] = value

    bezel_sec = doc.get("bezel", {})
    unknown = set(bezel_sec) - BEZEL_KEYS_OPTIONAL
    if unknown:
        fail(path, bezel_sec[sorted(unknown)[0]][1],
             f"unknown [bezel] key(s): {', '.join(sorted(unknown))}")
    style_id = 0
    if "style_id" in bezel_sec:
        style_id, lineno = as_int(bezel_sec, "style_id", path)
        if not 0 <= style_id <= 255:
            fail(path, lineno, f"style_id {style_id} out of range (0-255)")

    assets, pivot = load_assets(doc.get("assets", {}), path, folder, theme_id)

    if colors["arc_track"] == colors["arc_indicator"]:
        warn(f"{path}: arc_track == arc_indicator, gauge progress will be invisible")
    if colors["text_primary"] == colors["bg"]:
        warn(f"{path}: text_primary == bg, main values will be invisible")

    return {
        "id": theme_id,
        "name": name,
        "author": top.get("author", ("", 0))[0],
        "description": top.get("description", ("", 0))[0],
        "colors": colors,
        "style_id": style_id,
        "assets": assets,
        "pivot": pivot,
        "path": path,
    }


def load_assets(sec, path, folder, theme_id):
    """Validate [assets] and return ({kind: assetdict}, (pivot_x, pivot_y))."""
    unknown = set(sec) - set(ASSET_KINDS) - ASSET_EXTRA_KEYS
    if unknown:
        fail(path, sec[sorted(unknown)[0]][1],
             f"unknown [assets] key(s): {', '.join(sorted(unknown))}. "
             f"Allowed: {', '.join(sorted(set(ASSET_KINDS) | ASSET_EXTRA_KEYS))}")

    assets = {}
    for kind, spec in ASSET_KINDS.items():
        if kind not in sec:
            continue
        rel, lineno = sec[kind]
        png = (folder / rel).resolve()
        if not png.is_file():
            fail(path, lineno, f"{kind}: artwork not found: {rel}")
        try:
            w, h = png_size(png)
        except ThemeError as exc:
            fail(path, lineno, f"{kind}: {exc}")

        want = spec["size"]
        if want and (w, h) != want:
            fail(path, lineno,
                 f"{kind}: artwork is {w}x{h}, must be exactly {want[0]}x{want[1]} "
                 f"(it covers the whole round display)")
        if not want and (w > SCREEN_W or h > SCREEN_H):
            fail(path, lineno,
                 f"{kind}: artwork is {w}x{h}, must fit within {SCREEN_W}x{SCREEN_H}")

        symbol = f"theme_{theme_id.replace('-', '_')}_{kind}"
        assets[kind] = {
            "kind": kind, "png": png, "w": w, "h": h,
            "cf": spec["cf"], "bpp": spec["bpp"], "field": spec["field"],
            "symbol": symbol,
            "data_size": w * h * spec["bpp"],
            "sha": sha256_of(png),
            "src_rel": f"themes/{folder.parent.name}/{folder.name}/{rel}",
            "c_name": f"{symbol}.c",
        }

    pivot = None
    has_px = "needle_pivot_x" in sec
    has_py = "needle_pivot_y" in sec
    if "needle" in assets:
        if not (has_px and has_py):
            fail(path, sec["needle"][1],
                 "needle artwork requires needle_pivot_x and needle_pivot_y "
                 "(the rotation center, in pixels from the artwork's top-left)")
        px, px_line = as_int(sec, "needle_pivot_x", path)
        py, py_line = as_int(sec, "needle_pivot_y", path)
        n = assets["needle"]
        if not 0 <= px < n["w"]:
            fail(path, px_line, f"needle_pivot_x {px} outside artwork width {n['w']}")
        if not 0 <= py < n["h"]:
            fail(path, py_line, f"needle_pivot_y {py} outside artwork height {n['h']}")
        pivot = (px, py)
    elif has_px or has_py:
        fail(path, sec["needle_pivot_x" if has_px else "needle_pivot_y"][1],
             "needle_pivot_* set but no `needle` artwork declared")

    return assets, pivot


def load_registry(path):
    """Parse registry.txt into an ordered [(slot, id, lineno)] list."""
    if not path.is_file():
        raise ThemeError(f"{path}: registry not found")
    entries = []
    for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        parts = stripped.split()
        if len(parts) != 2:
            fail(path, lineno, f"expected '<slot> <id>', got {stripped!r}")
        try:
            slot = int(parts[0], 10)
        except ValueError:
            fail(path, lineno, f"slot {parts[0]!r} is not a number")
        entries.append((slot, parts[1], lineno))

    if not entries:
        raise ThemeError(f"{path}: registry is empty")

    seen_slots, seen_ids = {}, {}
    for slot, theme_id, lineno in entries:
        if slot in seen_slots:
            fail(path, lineno, f"slot {slot} already used on line {seen_slots[slot]}")
        if theme_id in seen_ids:
            fail(path, lineno, f"id {theme_id!r} already used on line {seen_ids[theme_id]}")
        seen_slots[slot] = lineno
        seen_ids[theme_id] = lineno

    entries.sort(key=lambda e: e[0])
    for expected, (slot, _id, lineno) in enumerate(entries):
        if slot != expected:
            fail(path, lineno,
                 f"slots must be contiguous from 0 — expected {expected}, got {slot}")
    if entries[0][1] != "default":
        fail(path, entries[0][2], "slot 0 must be 'default' (the factory look)")
    if len(entries) > MAX_SLOTS:
        raise ThemeError(f"{path}: {len(entries)} themes exceeds the {MAX_SLOTS} "
                         f"limit (NVS stores the slot as a uint8)")
    return entries


def discover(themes_dir):
    """Map theme id -> folder for every manifest under the scanned directories."""
    found = {}
    for sub in SCAN_DIRS:
        root = themes_dir / sub
        if not root.is_dir():
            continue
        for folder in sorted(p for p in root.iterdir() if p.is_dir()):
            if folder.name in SKIP_NAMES:
                continue
            if not (folder / "theme.toml").is_file():
                warn(f"{folder}: no theme.toml, skipped")
                continue
            if folder.name in found:
                raise ThemeError(
                    f"theme id {folder.name!r} defined twice: "
                    f"{found[folder.name]} and {folder}")
            found[folder.name] = folder
    return found


def collect(themes_dir, budget):
    registry = load_registry(themes_dir / "registry.txt")
    folders = discover(themes_dir)

    themes = []
    for slot, theme_id, lineno in registry:
        if theme_id not in folders:
            fail(themes_dir / "registry.txt", lineno,
                 f"slot {slot} refers to id {theme_id!r} but no folder defines it. "
                 f"Expected themes/builtin/{theme_id}/theme.toml or "
                 f"themes/community/{theme_id}/theme.toml. "
                 f"Never delete a registry line — it would shift every later slot.")
        themes.append((slot, load_theme(folders[theme_id], themes_dir)))

    unregistered = sorted(set(folders) - {e[1] for e in registry})
    if unregistered:
        next_slot = len(registry)
        lines = "\n".join(f"  {next_slot + i}  {tid}"
                          for i, tid in enumerate(unregistered))
        raise ThemeError(
            f"theme folder(s) not in registry.txt: {', '.join(unregistered)}\n"
            f"Append to themes/registry.txt (order matters, append only):\n{lines}")

    names = [t["name"] for _s, t in themes]
    dupes = sorted({n for n in names if names.count(n) > 1})
    if dupes:
        raise ThemeError(f"duplicate theme name(s) in the Settings roller: "
                         f"{', '.join(dupes)}. Names must be unique.")

    total = sum(a["data_size"] for _s, t in themes for a in t["assets"].values())
    if total > budget:
        detail = "\n".join(
            f"  {t['name']:<10} {a['kind']:<7} {a['data_size'] // 1024:>5} KB  {a['src_rel']}"
            for _s, t in themes for a in t["assets"].values())
        raise ThemeError(
            f"compiled-in artwork is {total // 1024} KB, over the "
            f"{budget // 1024} KB budget:\n{detail}\n"
            f"Artwork for every registered theme is linked in unconditionally.\n"
            f"Drop a theme, shrink artwork, or raise --asset-budget if the app\n"
            f"partition can take it (it is 4 MB; check build/*.bin size first).")
    return themes, total


# ---------------------------------------------------------------- C emission

def c_ident(theme_id):
    return theme_id.replace("-", "_")


def render(themes):
    out = []
    w = out.append
    w("// ============================================================")
    w("//  ui_theme_generated.c — GENERATED FILE, DO NOT EDIT")
    w("//")
    w("//  Produced by tools/gen_themes.py from:")
    w("//    themes/registry.txt          (slot -> id, append only)")
    w("//    themes/*/<id>/theme.toml     (per-theme manifest)")
    w("//")
    w("//  To add a theme: create the folder + manifest, append one line to")
    w("//  registry.txt, rebuild. Do not edit this file — it is overwritten at")
    w("//  every CMake configure.")
    w("//")
    w(f"//  {len(themes)} theme(s), slots 0..{len(themes) - 1}.")
    w("// ============================================================")
    w("")
    w('#include "ui_theme.h"')
    w("")

    externs = [a for _s, t in themes for a in t["assets"].values()]
    if externs:
        w("// Artwork lives in main/export_path/theme_assets/ (also generated).")
        for a in externs:
            w(f"extern const lv_img_dsc_t {a['symbol']};")
        w("")

    for slot, theme in themes:
        rel = Path(*theme["path"].parts[-4:])
        w(f"// slot {slot} — {rel}")
        if theme["description"]:
            w(f"// {theme['description']}")
        if theme["author"]:
            w(f"// author: {theme['author']}")
        w(f"static const ui_theme_t s_theme_{c_ident(theme['id'])} = {{")
        w(f'    .id   = "{theme["id"]}",')
        w(f'    .name = "{theme["name"]}",')
        w("    .colors = {")
        width = max(len(enum) for _k, enum in COLOR_ROLES)
        for key, enum in COLOR_ROLES:
            w(f"        [{enum}]{' ' * (width - len(enum))} = "
              f"0x{theme['colors'][key]:06X},")
        w("    },")
        w(f"    .bezel = {{ .style_id = {theme['style_id']} }},")
        for kind in ASSET_KINDS:
            a = theme["assets"].get(kind)
            if a:
                w(f"    .{a['field']} = &{a['symbol']},")
        if theme["pivot"]:
            w(f"    .needle_pivot_x = {theme['pivot'][0]},")
            w(f"    .needle_pivot_y = {theme['pivot'][1]},")
        w("};")
        w("")

    w("// Registry — index IS the slot number stored in NVS (theme_cfg.theme).")
    w("const ui_theme_t *const g_ui_themes[] = {")
    ident_width = max(len(c_ident(t["id"])) for _s, t in themes)
    for slot, theme in themes:
        ident = c_ident(theme["id"])
        w(f"    &s_theme_{ident},{' ' * (ident_width - len(ident))}  // slot {slot}")
    w("};")
    w("")
    w(f"const uint8_t g_ui_theme_count = {len(themes)};")
    return "\n".join(out) + "\n"


def write_if_changed(path, text):
    """Write only on a real change so unchanged runs do not trigger rebuilds."""
    if path.is_file() and path.read_text(encoding="utf-8") == text:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return True


def sync_assets(themes, assets_dir, check_only):
    """Convert changed artwork and drop orphaned generated files."""
    wanted, stale, written = {}, [], []
    for _slot, theme in themes:
        for a in theme["assets"].values():
            c_path = assets_dir / a["c_name"]
            wanted[a["c_name"]] = a
            if recorded_sha(c_path) != a["sha"]:
                stale.append(a)
                if not check_only:
                    write_if_changed(c_path, convert_png({**a, "c_path": c_path}))
                    written.append(a)

    orphans = []
    if assets_dir.is_dir():
        for existing in sorted(assets_dir.glob("theme_*.c")):
            if existing.name in wanted:
                continue
            # Only ever remove files this generator wrote.
            head = existing.read_text(encoding="utf-8", errors="replace")[:600]
            if GEN_MARKER in head:
                orphans.append(existing)
                if not check_only:
                    existing.unlink()
    return stale, written, orphans


# ---------------------------------------------------------------- main

def main(argv=None):
    repo_root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--themes-dir", type=Path, default=repo_root / "themes")
    parser.add_argument("--output", type=Path,
                        default=repo_root / "main/export_path/ui_theme_generated.c")
    parser.add_argument("--assets-dir", type=Path,
                        default=repo_root / "main/export_path/theme_assets")
    parser.add_argument("--asset-budget", type=int, default=DEFAULT_ASSET_BUDGET,
                        help="max total compiled-in artwork, in bytes")
    parser.add_argument("--check", action="store_true",
                        help="fail if any generated file is missing or stale; writes nothing")
    args = parser.parse_args(argv)

    try:
        themes, asset_bytes = collect(args.themes_dir, args.asset_budget)
        text = render(themes)
        stale, written, orphans = sync_assets(themes, args.assets_dir, args.check)
    except ThemeError as exc:
        print(f"gen_themes: error: {exc}", file=sys.stderr)
        return 1

    n_assets = sum(len(t["assets"]) for _s, t in themes)
    summary = (f"{len(themes)} themes, {n_assets} artwork file(s), "
               f"{asset_bytes // 1024} KB of {args.asset_budget // 1024} KB budget")

    if args.check:
        problems = []
        if not args.output.is_file():
            problems.append(f"{args.output} does not exist")
        elif args.output.read_text(encoding="utf-8") != text:
            problems.append(f"{args.output} is stale")
        problems += [f"{a['c_name']} is stale or missing" for a in stale]
        problems += [f"{p.name} is orphaned" for p in orphans]
        if problems:
            print("gen_themes: error: run tools/gen_themes.py and commit the result:",
                  file=sys.stderr)
            for p in problems:
                print(f"  - {p}", file=sys.stderr)
            return 1
        print(f"gen_themes: up to date ({summary})")
        return 0

    changed = write_if_changed(args.output, text)
    for a in written:
        print(f"gen_themes: converted {a['src_rel']} -> {a['c_name']} "
              f"({a['w']}x{a['h']}, {a['data_size'] // 1024} KB)")
    for p in orphans:
        print(f"gen_themes: removed orphaned {p.name}")
    print(f"gen_themes: {summary}"
          f"{'' if changed or written or orphans else ' (unchanged)'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
