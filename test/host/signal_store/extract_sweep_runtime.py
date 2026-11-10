#!/usr/bin/env python3
"""Compile real UI sweep wrappers/state with only clock/backlight/boot stubs."""
from pathlib import Path
import re
import sys

source = Path(sys.argv[1]).read_text()
start = source.index('static _Atomic int s_sweep_step')
end = source.index('/* ================================================================', start)
parts = [source[start:end]]
# Functions below contain no brace-bearing string/comment; fail if definitions move.
for name in ('ui_sweep_get_step', 'ui_showroom_set_page_from_sync',
             'ui_ext_sweep_active', 'ui_ext_sweep_get_step',
             'ui_ext_sweep_trigger', 'ui_ext_sweep_tick'):
    match = re.search(r'^(?:int|void|bool|float)\s+' + name + r'\([^)]*\)\s*\{',
                      source, re.MULTILINE)
    if not match:
        raise SystemExit(f'Missing production sweep definition: {name}')
    pos = match.end()
    depth = 1
    while depth:
        if source[pos] == '{':
            depth += 1
        elif source[pos] == '}':
            depth -= 1
        pos += 1
    parts.append(source[match.start():pos])
# Include the real timer cadence policy; screens are opaque identity tokens.
ui = Path(sys.argv[1]).with_name('ui.c').read_text()
a = ui.index('static uint32_t ui_refresh_period_ms_for_screen(')
b = ui.index('static uint16_t ui_data_mask_for_screen(', a)
policy = ui[a:b]
names = sorted(set(re.findall(r'\bui_ScreenPage\w+', policy)))
parts.append('typedef int lv_obj_t;\nstatic lv_obj_t policy_screens[%d];' % len(names))
for i, name in enumerate(names):
    parts.append('#define %s (&policy_screens[%d])' % (name, i))
header = Path(sys.argv[1]).with_name('ui_ext.h').read_text()
parts.append(re.search(r'^#define RPM_FLASH_PERIOD_MS[^\n]+', header, re.MULTILINE)[0])
parts.append(policy)
Path(sys.argv[2]).write_text('\n\n'.join(parts) + '\n')
