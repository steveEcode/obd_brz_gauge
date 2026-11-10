#!/usr/bin/env python3
"""Compile actual boot routing and NVS page repair; check persisted roller indices."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[3]
settings = (root / 'main/export_path/screens/ui_ScreenPageSettings.c').read_text()
options = re.search(r'static const char \*page_names = "([^"]+)";', settings).group(1)
assert options.split(r'\n') == ['TEMP', 'INFO', 'CHART', 'NEEDLE', 'GEAR', 'RPM', 'SPEED', 'TPMS']
assert int(re.search(r'#define BOOT_PAGE_COUNT (\d+)', settings).group(1)) == 8
nvs = (root / 'main/bsp_obd_dsp/nvs_storage.c').read_text()
repair = re.search(r'if\(s_cfg.default_page[^\n]+', nvs).group(0)
source = (root / 'main/export_path/ui_ext.c').read_text()


def function(name):
    match = re.search(r'^static (?:void|bool) ' + name + r'\([^)]*\)\s*\{', source, re.M)
    assert match, name
    pos, depth = match.end(), 1
    while depth:
        if source[pos] == '{':
            depth += 1
        elif source[pos] == '}':
            depth -= 1
        pos += 1
    return source[match.start():pos]


screens = ['Temp', 'Info', 'OilPressure', 'Needle', 'Gear', 'Rpm', 'Speed', 'TPMS', 'ThemeGauge']
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef int lv_obj_t;
typedef struct { uint8_t default_page, device_role; char ble_device_name[2]; } nvs_user_cfg_t;
#define ESPNOW_ROLE_SLAVE 1
#define ESP_LOGI(...) ((void)0)
static nvs_user_cfg_t cfg, s_cfg;
static lv_obj_t tokens[9], *destination, *ui_ScreenPageLogo;
static bool s_boot_theme_job, s_boot_theme_attempted, s_boot_done, s_sweep_pending;
static unsigned custom_pages, sweeps, scan_entries, init_calls;
static uint8_t ui_theme_gauge_page_index, master_mac[6];
static nvs_user_cfg_t nvs_cfg_read(void) { return cfg; }
static uint8_t theme_page_list_count(void) { return custom_pages; }
static uint8_t theme_page_default_index(void) { return 2; }
static void espnow_link_get_bound_master_mac(uint8_t mac[6]) { memcpy(mac, master_mac, 6); }
static void boot_load_destination(lv_obj_t *screen) { destination = screen; }
static void boot_goto_ble_scan(void) { ++scan_entries; }
static void sweep_start(void) { ++sweeps; }
'''
for i, screen in enumerate(screens):
    harness += f'''
static lv_obj_t *ui_ScreenPage{screen};
static void ui_ScreenPage{screen}_screen_init(void)
{{ ++init_calls; ui_ScreenPage{screen} = &tokens[{i}]; }}
'''
harness += function('boot_needs_ble_scan') + '\n' + function('boot_enter_default_page')
harness += '\nstatic void repair_page(void) {\n' + repair + '\n}\n'
harness += r'''
int main(void)
{
    cfg.ble_device_name[0] = 'A';
    for (unsigned page = 0; page < 256; ++page) {
        s_cfg.default_page = (uint8_t)page;
        repair_page();
        assert(s_cfg.default_page == (page < 8 ? page : 0));
        cfg.default_page = (uint8_t)page;
        destination = NULL; s_boot_done = false; s_sweep_pending = true;
        unsigned old_sweeps = sweeps;
        boot_enter_default_page();
        assert(destination == &tokens[page < 8 ? page : 0]);
        assert(s_boot_done && !s_sweep_pending && sweeps == old_sweeps + 1);
    }
    assert(init_calls == 8); /* Pages are lazy-created once, including TPMS. */
    cfg.default_page = 7;
    custom_pages = 3;
    boot_enter_default_page();
    assert(destination == &tokens[8] && ui_theme_gauge_page_index == 2);
    custom_pages = 0;
    destination = NULL; s_boot_theme_job = true;
    boot_enter_default_page(); assert(!destination);
    s_boot_theme_job = false;
    cfg.ble_device_name[0] = '\0';
    boot_enter_default_page(); assert(scan_entries == 1 && !destination);
    cfg.device_role = ESPNOW_ROLE_SLAVE;
    boot_enter_default_page(); assert(scan_entries == 2 && !destination);
    master_mac[0] = 1;
    boot_enter_default_page(); assert(destination == &tokens[7]);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='boot-page-test-') as tmp:
    path = Path(tmp)
    (path / 'test.c').write_text(harness)
    subprocess.run([sys.argv[1], '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Boot page: stable roller indices, NVS repair, routing and priority checks passed')
