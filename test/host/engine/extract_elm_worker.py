#!/usr/bin/env python3
"""Extract real ELM wake, registration and worker control paths for host tests."""
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text()
sections = [
    ('static void wake_task(void)', 'static bool mac_nonzero('),
    ('static bool ensure_registered(void)', '/* ------------------------------------------------------------------ */\n/* vp_engine sink'),
    ('static void obd_task(void *arg)', '/* ------------------------------------------------------------------ */\n/* public API'),
]
parts = []
parts.append(source[source.index('static bool mac_nonzero('):source.index('static uint16_t obd_demand_mask_get(')])
for start, end in sections:
    i = source.index(start)
    parts.append(source[i:source.index(end, i)])
parts.append(source[source.index('void elm327_ble_copy_connected_name('):])
Path(sys.argv[2]).write_text('\n'.join(parts))
