#!/usr/bin/env python3
"""
Encode a video into boot_block.bin + boot_block.txt for the ESP32 boot_block_player.
Format: delta_varint_rgb565_black_v1 (same as hokori_vehicle_gauge).

Usage:
    python3 tools/make_boot_block.py <input_video> [--canvas 360] [--grid 240] [--fps 15] [--output bootmedia/slot_a]

Requirements:
    pip install Pillow
    ffmpeg in PATH
"""

import argparse
import math
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("ERROR: Pillow not installed. Run: pip install Pillow")
    sys.exit(1)


def rgb888_to_rgb565(r: int, g: int, b: int) -> int:
    r5 = (r * 31 + 127) // 255
    g6 = (g * 63 + 127) // 255
    b5 = (b * 31 + 127) // 255
    return ((r5 & 0x1F) << 11) | ((g6 & 0x3F) << 5) | (b5 & 0x1F)


def encode_varint(value: int) -> bytes:
    out = bytearray()
    while True:
        chunk = value & 0x7F
        value >>= 7
        if value > 0:
            chunk |= 0x80
        out.append(chunk)
        if value == 0:
            break
    return bytes(out)


def compute_grid_edges(canvas_size: int, grid_size: int) -> list:
    return [int(i * canvas_size / grid_size) for i in range(grid_size + 1)]


def frame_to_cells(img: Image.Image, grid_size: int, x_edges: list, y_edges: list,
                   threshold: float) -> list:
    """Return list of (cell_index, packed_rgb24) for changed cells."""
    cells = []
    pixels = img.load()
    w, h = img.size

    for gy in range(grid_size):
        y0 = y_edges[gy]
        y1 = y_edges[gy + 1]
        if y1 <= y0:
            continue
        for gx in range(grid_size):
            x0 = x_edges[gx]
            x1 = x_edges[gx + 1]
            if x1 <= x0:
                continue

            # 采样格子内所有像素, 用最大亮度判断是否保留 (捕捉淡入细线)
            max_lum = 0.0
            sum_r = 0.0
            sum_g = 0.0
            sum_b = 0.0
            bright_count = 0
            count = 0

            for y in range(y0, y1):
                for x in range(x0, x1):
                    if x >= w or y >= h:
                        continue
                    r, g, b = pixels[x, y][:3]
                    lum = 0.299 * r + 0.587 * g + 0.114 * b
                    if lum > max_lum:
                        max_lum = lum
                    count += 1
                    if lum >= threshold:
                        sum_r += r
                        sum_g += g
                        sum_b += b
                        bright_count += 1

            if count == 0 or max_lum < threshold:
                packed = 0
            elif bright_count > 0:
                r = int(sum_r / bright_count)
                g = int(sum_g / bright_count)
                b = int(sum_b / bright_count)
                packed = ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF)
            else:
                # 最大亮度够但没像素超 threshold: 用所有像素平均色 (极淡的情况)
                packed = 0

            idx = gy * grid_size + gx
            cells.append((idx, packed))

    return cells


def encode_frame_delta(prev_cells: dict, curr_cells: list) -> bytes:
    """Encode changed cells as delta_varint_rgb565_black_v1."""
    changes = []
    for idx, packed in curr_cells:
        if prev_cells.get(idx, -1) != packed:
            prev_cells[idx] = packed
            changes.append((idx, packed))

    buf = bytearray()
    change_count = len(changes)
    buf += struct.pack('<H', change_count)

    prev_idx = -1
    for idx, packed in changes:
        delta = idx if prev_idx < 0 else idx - prev_idx
        is_black = 1 if packed == 0 else 0
        delta_and_black = (delta << 1) | is_black
        buf += encode_varint(delta_and_black)

        if not is_black:
            r = (packed >> 16) & 0xFF
            g = (packed >> 8) & 0xFF
            b = packed & 0xFF
            rgb565 = rgb888_to_rgb565(r, g, b)
            buf += struct.pack('<H', rgb565)

        prev_idx = idx

    return bytes(buf)


def main():
    parser = argparse.ArgumentParser(description="Encode video to boot_block format")
    parser.add_argument("input", help="Input video file (mp4/avi/etc)")
    parser.add_argument("--canvas", type=int, default=360, help="Canvas size in pixels (default: 360)")
    parser.add_argument("--grid", type=int, default=240, help="Grid resolution (default: 240)")
    parser.add_argument("--fps", type=int, default=15, help="Target fps (default: 15)")
    parser.add_argument("--threshold", type=float, default=40.0,
                        help="Luminance threshold for black detection (default: 40)")
    parser.add_argument("--shift-up", type=int, default=0, help="Shift content up by N pixels")
    parser.add_argument("--output", "-o", default="", help="Output directory (default: bootmedia/slot_a)")
    parser.add_argument("--no-audio", action="store_true", help="Skip audio extraction")
    parser.add_argument("--duration", type=float, default=0,
                        help="Force trim video to this duration in seconds (0=auto)")
    args = parser.parse_args()

    input_path = Path(args.input).resolve()
    if not input_path.exists():
        print(f"ERROR: Input not found: {input_path}")
        sys.exit(1)

    # Determine output dir
    if args.output:
        output_dir = Path(args.output).resolve()
    else:
        output_dir = Path(__file__).parent.parent / "bootmedia" / "slot_a"
    output_dir.mkdir(parents=True, exist_ok=True)

    canvas = args.canvas
    grid = args.grid
    fps = args.fps

    print(f"Encoding boot block media")
    print(f"  input    : {input_path}")
    print(f"  output   : {output_dir}")
    print(f"  canvas   : {canvas}x{canvas}")
    print(f"  grid     : {grid}x{grid}")
    print(f"  fps      : {fps}")
    print(f"  threshold: {args.threshold}")

    # Extract frames with ffmpeg
    with tempfile.TemporaryDirectory() as tmpdir:
        frame_pattern = os.path.join(tmpdir, "frame_%04d.png")

        # Build ffmpeg video filter: scale to canvas, pad to square, shift up
        vf = f"fps={fps},scale={canvas}:{canvas}:force_original_aspect_ratio=decrease," \
             f"pad={canvas}:{canvas}:(ow-iw)/2:(oh-ih)/2-{args.shift_up}:black"

        cmd = [
            "ffmpeg", "-y", "-i", str(input_path),
        ]
        if args.duration > 0:
            cmd += ["-t", str(args.duration)]
        cmd += [
            "-vf", vf,
            frame_pattern
        ]
        print(f"  Running ffmpeg...")
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            print(f"ERROR: ffmpeg failed:\n{result.stderr[-500:]}")
            sys.exit(1)

        # Collect frame files
        frame_files = sorted(Path(tmpdir).glob("frame_*.png"))
        if not frame_files:
            print("ERROR: No frames extracted")
            sys.exit(1)

        print(f"  Extracted {len(frame_files)} frames")

        # Extract audio if requested
        if not args.no_audio:
            pcm_path = output_dir / "boot.pcm"
            cmd_audio = [
                "ffmpeg", "-y", "-i", str(input_path),
                "-vn", "-ac", "1", "-ar", "16000", "-f", "s16le", str(pcm_path)
            ]
            subprocess.run(cmd_audio, capture_output=True)
            if pcm_path.exists():
                print(f"  Audio: {pcm_path} ({pcm_path.stat().st_size} bytes)")

        # Encode frames
        x_edges = compute_grid_edges(canvas, grid)
        y_edges = compute_grid_edges(canvas, grid)
        prev_cells = {}
        data_path = output_dir / "boot_block.bin"
        total_changes = 0
        max_changes = 0

        with open(data_path, "wb") as f:
            for i, frame_path in enumerate(frame_files):
                img = Image.open(frame_path).convert("RGB")
                curr_cells = frame_to_cells(img, grid, x_edges, y_edges, args.threshold)

                # Only track changes
                changed = [(idx, packed) for idx, packed in curr_cells
                           if prev_cells.get(idx, -1) != packed]

                frame_data = encode_frame_delta(prev_cells, curr_cells)
                f.write(frame_data)

                change_count = len(changed)
                total_changes += change_count
                max_changes = max(max_changes, change_count)

                if (i + 1) % 20 == 0 or i == len(frame_files) - 1:
                    print(f"  Frame {i+1}/{len(frame_files)}: {change_count} changes")

        # Write manifest
        frame_count = len(frame_files)
        duration_ms = int(round(frame_count * 1000.0 / fps))
        manifest_path = output_dir / "boot_block.txt"
        manifest_lines = [
            f"canvas_width={canvas}",
            f"canvas_height={canvas}",
            f"grid_width={grid}",
            f"grid_height={grid}",
            f"fps={fps}",
            f"frame_count={frame_count}",
            f"duration_ms={duration_ms}",
            "stream_format=delta_varint_rgb565_black_v1",
            "data_file=boot_block.bin",
        ]
        manifest_path.write_text("\n".join(manifest_lines) + "\n")

        avg_changes = total_changes / frame_count
        print(f"\nDone!")
        print(f"  {manifest_path} ({manifest_path.stat().st_size} bytes)")
        print(f"  {data_path} ({data_path.stat().st_size} bytes)")
        print(f"  frames         : {frame_count}")
        print(f"  duration       : {duration_ms}ms ({duration_ms/1000:.1f}s)")
        print(f"  avg changes    : {avg_changes:.1f}")
        print(f"  peak changes   : {max_changes}")
        print(f"\nNext steps:")
        print(f"  1. Copy {output_dir}/ to your bootmedia partition")
        print(f"  2. Flash with: python -m espflash write_partition bootmedia {output_dir}/")
        print(f"     Or use parttool.py / esptool.py to write the bootmedia partition")


if __name__ == "__main__":
    main()
