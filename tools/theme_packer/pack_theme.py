#!/usr/bin/env python3
"""
Theme Packer - Convert theme assets into binary partition image

Usage:
    python3 pack_theme.py <theme_dir> <output_bin>

Example:
    python3 pack_theme.py themes/boost_oil firmware/theme_boost_oil.bin

Theme directory structure:
    theme_dir/
    ├── theme_manifest.json  (required)
    ├── assets/
    │   ├── dial.png         (optional, 360x360)
    │   └── ring.png         (optional, 360x360 RGBA)
    └── layout.json          (optional, custom page layout)
"""

import json
import struct
import sys
from pathlib import Path
from typing import Dict, Optional

try:
    from PIL import Image
    PIL_AVAILABLE = True
except ImportError:
    PIL_AVAILABLE = False
    print("Warning: Pillow not installed, image conversion will be skipped")
    print("Install with: pip install Pillow")

PARTITION_SIZE = 4 * 1024 * 1024  # 4MB, must match theme_0 in partitions.csv
MANIFEST_RESERVED_SIZE = 8 * 1024  # First 8KB reserved for manifest JSON


def rgb888_to_rgb565(r: int, g: int, b: int) -> int:
    """Convert RGB888 to RGB565 format"""
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def pack_image_rgb565(img_path: Path, offset: int, data: bytearray) -> int:
    """Pack 360x360 RGB image as RGB565"""
    if not PIL_AVAILABLE:
        print(f"Skipping {img_path} (Pillow not installed)")
        return 0

    img = Image.open(img_path).convert('RGB')
    if img.size != (360, 360):
        raise ValueError(f"Image {img_path} must be 360x360, got {img.size}")

    print(f"  Packing {img_path.name} as RGB565...")
    pixels = img.load()

    for y in range(360):
        for x in range(360):
            r, g, b = pixels[x, y]
            rgb565 = rgb888_to_rgb565(r, g, b)
            pos = offset + (y * 360 + x) * 2
            struct.pack_into('<H', data, pos, rgb565)

    size = 360 * 360 * 2  # 259200 bytes
    print(f"    Packed {size} bytes at offset 0x{offset:06X}")
    return size


def pack_image_rgba8888(img_path: Path, offset: int, data: bytearray) -> int:
    """Pack 360x360 RGBA image as RGBA8888"""
    if not PIL_AVAILABLE:
        print(f"Skipping {img_path} (Pillow not installed)")
        return 0

    img = Image.open(img_path).convert('RGBA')
    if img.size != (360, 360):
        raise ValueError(f"Image {img_path} must be 360x360, got {img.size}")

    print(f"  Packing {img_path.name} as RGBA8888...")
    pixels = img.load()

    for y in range(360):
        for x in range(360):
            r, g, b, a = pixels[x, y]
            pos = offset + (y * 360 + x) * 4
            struct.pack_into('BBBB', data, pos, r, g, b, a)

    size = 360 * 360 * 4  # 518400 bytes
    print(f"    Packed {size} bytes at offset 0x{offset:06X}")
    return size


def pack_layout_json(layout_path: Path, offset: int, data: bytearray) -> int:
    """Pack layout JSON (optional custom page layout)"""
    if not layout_path.exists():
        return 0

    print(f"  Packing {layout_path.name}...")
    layout_bytes = layout_path.read_bytes()

    if len(layout_bytes) > 64 * 1024:
        raise ValueError(
            f"Layout JSON too large: {len(layout_bytes)} bytes (max 64KB)")

    data[offset:offset + len(layout_bytes)] = layout_bytes
    print(f"    Packed {len(layout_bytes)} bytes at offset 0x{offset:06X}")
    return len(layout_bytes)


def validate_manifest(manifest: Dict) -> None:
    """Validate theme manifest structure"""
    required_top = ["schema_version", "theme", "colors"]
    for key in required_top:
        if key not in manifest:
            raise ValueError(f"Missing required field: {key}")

    theme = manifest["theme"]
    required_theme = ["id", "name", "version", "author"]
    for key in required_theme:
        if key not in theme:
            raise ValueError(f"Missing required field in theme: {key}")

    colors = manifest["colors"]
    required_colors = [
        "bg", "ring", "arc_track", "arc_indicator",
        "text_primary", "text_secondary", "needle", "panel"
    ]
    for key in required_colors:
        if key not in colors:
            raise ValueError(f"Missing required color: {key}")


def pack_theme(theme_dir: Path, output_bin: Path) -> None:
    """Main packer function"""
    print(f"Packing theme from: {theme_dir}")
    print(f"Output: {output_bin}")
    print("=" * 60)

    # Initialize partition data (all 0xFF)
    data = bytearray([0xFF] * PARTITION_SIZE)

    # Load and validate manifest
    manifest_path = theme_dir / "theme_manifest.json"
    if not manifest_path.exists():
        raise FileNotFoundError(f"Manifest not found: {manifest_path}")

    manifest = json.load(open(manifest_path, 'r'))
    validate_manifest(manifest)

    theme_meta = manifest['theme']
    print(f"\nTheme: {theme_meta['name']} v{theme_meta['version']}")
    print(f"  ID: {manifest['theme']['id']}")
    print(f"  Author: {manifest['theme']['author']}")

    # Track asset offsets
    current_offset = MANIFEST_RESERVED_SIZE  # Start after manifest area
    assets_info = {}

    # Pack dial background (360x360 RGB565)
    dial_path = theme_dir / "assets" / "dial.png"
    if dial_path.exists():
        size = pack_image_rgb565(dial_path, current_offset, data)
        if size > 0:
            assets_info["dial_background"] = {
                "offset": current_offset,
                "size": size,
                "format": "rgb565",
                "width": 360,
                "height": 360
            }
            current_offset += size
    else:
        print(f"\nWarning: dial.png not found, skipping")

    # Pack ring overlay (360x360 RGBA8888)
    ring_path = theme_dir / "assets" / "ring.png"
    if ring_path.exists():
        size = pack_image_rgba8888(ring_path, current_offset, data)
        if size > 0:
            assets_info["ring_overlay"] = {
                "offset": current_offset,
                "size": size,
                "format": "rgba8888",
                "width": 360,
                "height": 360
            }
            current_offset += size
    else:
        print(f"\nWarning: ring.png not found, skipping")

    # Pack layout JSON (optional)
    layout_path = theme_dir / "layout.json"
    if layout_path.exists():
        # Store layout data offset in manifest
        if "pages" not in manifest:
            manifest["pages"] = {}
        if "theme_pages" not in manifest["pages"]:
            manifest["pages"]["theme_pages"] = []

        size = pack_layout_json(layout_path, current_offset, data)
        if size > 0:
            # Update or add theme page entry
            if len(manifest["pages"]["theme_pages"]) == 0:
                manifest["pages"]["theme_pages"].append({
                    "id": "main_gauge",
                    "type": "custom_layout",
                    "replaces": ["page_rpm", "page_boost", "page_multi_gauge"]
                })

            manifest["pages"]["theme_pages"][0]["layout_data_offset"] = current_offset
            manifest["pages"]["theme_pages"][0]["layout_data_size"] = size
            current_offset += size

    # Add assets section to manifest
    if assets_info:
        manifest["assets"] = assets_info

    # Write final manifest to partition (first 8KB)
    manifest_bytes = json.dumps(manifest, indent=2).encode('utf-8')
    if len(manifest_bytes) >= MANIFEST_RESERVED_SIZE:
        raise ValueError(
            f"Manifest too large: {len(manifest_bytes)} bytes (max {MANIFEST_RESERVED_SIZE})")

    data[0:len(manifest_bytes)] = manifest_bytes

    # Write output file
    output_bin.parent.mkdir(parents=True, exist_ok=True)
    output_bin.write_bytes(data)

    # Print summary
    print("\n" + "=" * 60)
    print("Packing complete!")
    print(f"  Output file: {output_bin}")
    print(
        f"  Total size: {len(data)} bytes ({len(data) / (1024*1024):.2f} MB)")
    print(f"  Manifest: {len(manifest_bytes)} bytes")
    print(f"  Assets: {len(assets_info)} items")
    print(
        f"  Used: {current_offset} bytes ({current_offset / (1024*1024):.2f} MB)")
    free_bytes = PARTITION_SIZE - current_offset
    print(f"  Free: {free_bytes} bytes ({free_bytes / (1024*1024):.2f} MB)")


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)

    theme_dir = Path(sys.argv[1])
    output_bin = Path(sys.argv[2])

    if not theme_dir.is_dir():
        print(f"Error: Theme directory not found: {theme_dir}")
        sys.exit(1)

    try:
        pack_theme(theme_dir, output_bin)
    except Exception as e:
        print(f"\nError: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)


if __name__ == "__main__":
    main()
