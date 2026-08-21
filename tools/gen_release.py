#!/usr/bin/env python3
"""Generate firmware/release/latest.json from git metadata + the release binaries.

Run from anywhere (uses the repo root relative to this file). It assumes the
project has already been built, so `build/` holds the fresh *.bin files:

    idf.py build
    python3 tools/gen_release.py

It does two things:
  1. copies build/{bootloader,partition_table,ota_data_initial,obd_brz_gauge,bootmedia}
     into firmware/release/ (the static release directory the app reads);
  2. writes firmware/release/latest.json with the git build tag and the
     sha256/size of every release binary.

The companion app compares `firmware.count` here against the device's own
manifest to decide whether an update is available. `count` comes from the
current git HEAD, so remember to `git commit` your code *before* building
(see docs/APP_INTEGRATION.md).
"""

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(ROOT, "build")
RELEASE_DIR = os.path.join(ROOT, "firmware", "release")

# Device block must stay in sync with main/app_obd_dsp/device_identity.c.
DEVICE = {
    "board": "Waveshare ESP32-S3-Touch-LCD-1.85",
    "variant": "obd_brz_gauge",
    "lcd": "ST77916",
    "screen": {"w": 360, "h": 360, "bpp": 16},
    "flash_mb": 16,
    "psram_mb": 8,
    "ota_slots": 2,
    "bootmedia_slots": 1,
    "bootmedia_format": 1,
}

# Firmware metadata not derivable from git. Update IDF_VER when you upgrade ESP-IDF.
PROJECT = "obd_brz_gauge"
VERSION = "1"
IDF_VER = "v5.5.3"
SLOT = "ota_0"

# build/ source -> firmware/release/ destination (relative to RELEASE_DIR).
BINS = {
    "firmware": "obd_brz_gauge.bin",
    "bootmedia": "bootmedia.bin",
    "partition_table": "partition_table/partition-table.bin",
    "bootloader": "bootloader/bootloader.bin",
    "ota_data_initial": "ota_data_initial.bin",
}


def git(*args):
    return subprocess.check_output(["git", "-C", ROOT, *args], text=True).strip()


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    # 1. Copy the fresh build artifacts into the release directory.
    for rel in BINS.values():
        src = os.path.join(BUILD_DIR, rel)
        dst = os.path.join(RELEASE_DIR, rel)
        if not os.path.isfile(src):
            sys.exit(f"missing build artifact: {src}\nRun `idf.py build` first.")
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)
        print(f"copied {rel}")

    # 2. Build the version metadata from the current git HEAD.
    branch = git("rev-parse", "--abbrev-ref", "HEAD")
    count = int(git("rev-list", "--count", "HEAD"))
    short = git("rev-parse", "--short=12", "HEAD")
    branch_safe = re.sub(r"[^A-Za-z0-9._-]", "_", branch)

    # built timestamp from the freshly copied firmware image (its build mtime).
    fw_path = os.path.join(RELEASE_DIR, "obd_brz_gauge.bin")
    built = time.strftime("%b %d %Y %H:%M:%S", time.localtime(os.path.getmtime(fw_path)))

    files = {}
    for key, rel in BINS.items():
        p = os.path.join(RELEASE_DIR, rel)
        files[key] = {"path": rel, "size": os.path.getsize(p), "sha256": sha256(p)}

    manifest = {
        "device": DEVICE,
        "firmware": {
            "project": PROJECT,
            "version": VERSION,
            "build_tag": f"{branch_safe}-{count}-{short}",
            "git": short,
            "branch": branch,
            "count": count,
            "built": built,
            "idf": IDF_VER,
            "slot": SLOT,
        },
        "files": files,
    }

    out = os.path.join(RELEASE_DIR, "latest.json")
    with open(out, "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    print(f"wrote latest.json  count={count}  build_tag={manifest['firmware']['build_tag']}")
    print("next: git add firmware/release/ && git commit -m \"build: update release firmware binaries\"")


if __name__ == "__main__":
    main()
