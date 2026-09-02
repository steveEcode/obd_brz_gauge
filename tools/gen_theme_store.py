#!/usr/bin/env python3
"""Generate theme_store/catalog.json from every theme_store/<id>/ folder.

Run after packing a theme into the store (see theme_store/README.md):

    python3 tools/theme_packer/pack_theme.py themes/<id> \\
        theme_store/<id>/theme.bin
    # ... create theme_store/<id>/info.json and preview.png ...
    python3 tools/gen_theme_store.py

Each theme_store/<id>/ folder must contain:
    info.json     - {id, title, description, author, version}
    preview.png   - 360x360 cover image shown in an app's theme picker
    theme.bin     - packed binary from pack_theme.py (exactly 4MB)

This script only reads those three files; it never builds theme.bin itself.
"""

import hashlib
import json
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
STORE_DIR = ROOT / "theme_store"
REQUIRED_INFO_FIELDS = ("id", "title", "description", "author", "version")
EXPECTED_BIN_SIZE = 4 * 1024 * 1024  # must match theme_0 in partitions.csv


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def load_theme_entry(theme_dir: Path) -> dict:
    info_path = theme_dir / "info.json"
    preview_path = theme_dir / "preview.png"
    bin_path = theme_dir / "theme.bin"

    required = (info_path, preview_path, bin_path)
    missing = [p.name for p in required if not p.is_file()]
    if missing:
        names = ', '.join(missing)
        raise ValueError(f"{theme_dir.name}: missing file(s): {names}")

    info = json.loads(info_path.read_text())
    for field in REQUIRED_INFO_FIELDS:
        if field not in info:
            raise ValueError(
                f"{theme_dir.name}/info.json: missing field '{field}'")

    if info["id"] != theme_dir.name:
        raise ValueError(
            f"{theme_dir.name}/info.json: id '{info['id']}' must match "
            f"folder name")

    bin_size = bin_path.stat().st_size
    if bin_size != EXPECTED_BIN_SIZE:
        raise ValueError(
            f"{theme_dir.name}/theme.bin: size {bin_size} != expected "
            f"{EXPECTED_BIN_SIZE} (re-run pack_theme.py?)")

    return {
        "id": info["id"],
        "title": info["title"],
        "description": info["description"],
        "author": info["author"],
        "version": info["version"],
        "preview": f"{theme_dir.name}/preview.png",
        "bin": {
            "path": f"{theme_dir.name}/theme.bin",
            "size": bin_size,
            "sha256": sha256(bin_path),
        },
    }


def main():
    if not STORE_DIR.is_dir():
        sys.exit(f"theme store directory not found: {STORE_DIR}")

    entries = []
    errors = []
    for theme_dir in sorted(STORE_DIR.iterdir()):
        if not theme_dir.is_dir():
            continue
        try:
            entries.append(load_theme_entry(theme_dir))
            print(f"ok: {theme_dir.name}")
        except ValueError as e:
            errors.append(str(e))
            print(f"SKIP: {e}")

    catalog = {
        "generated": time.strftime("%Y-%m-%d %H:%M:%S"),
        "themes": entries,
    }

    out_path = STORE_DIR / "catalog.json"
    with open(out_path, "w") as f:
        json.dump(catalog, f, indent=2)
        f.write("\n")

    count = len(entries)
    skipped = len(errors)
    print(f"\nwrote {out_path}  ({count} theme(s), {skipped} skipped)")
    if errors:
        sys.exit(1)


if __name__ == "__main__":
    main()
