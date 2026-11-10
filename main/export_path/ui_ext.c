// ================================================================
//  ui_ext.c — hand-written extension logic for ui.c
//
//  Migration plan: gradually move the hand-written logic from ui.c my_timerMain() into this file.
//  Migrate one module at a time (showroom / boot_anim / sweep / rpm_warn);
//  move the corresponding static variables along with it, ui.c accesses them via the API in ui_ext.h.
//
//  Migration completed:
//  - disp_item system → ui_disp_item.c/h (data-item metadata and helpers)
//  - rpm warning flash → ui_ext_rpm_flash_tick()
//  - sweep animation → ui_ext_sweep_*()
//  - showroom state machine → ui_ext_showroom_*()
//  - boot animation (RACE/AS/ONE + video) → ui_ext_boot_video_tick() / ui_ext_intro_tick()
// ================================================================

#include "ui_ext.h"
#include "ui_theme_navigation.h"
#include "ui_tpms.h"
#include "ui.h"
#include "bsp_obd_dsp/nvs_storage.h"
#include "bsp_obd_dsp/espnow_link.h"
#include "bsp_obd_dsp/lcd_driver/ST77916.h"
#include "app_obd_dsp/obd_data_cache.h"
#include "app_obd_dsp/signal_store.h"
#include "app_obd_dsp/theme_library.h"
#include "media/boot_video.h"
#include "theme_engine/theme_interface.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_log.h"
#include "lvgl.h"
#include <stdatomic.h>
#include "freertos/task.h"
#include "app_obd_dsp/lvgl_runtime.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ui_ext";

/* ================================================================
 *  Sweep animation state
 *  Sweep progress only advances inside the LVGL task; the master's espnow TX task
 *  broadcasts it read-only via ui_sweep_get_step().
 *  Values: 0=off, 1~SWEEP_TOTAL=sweep animation running (slaves mirror it as-is),
 *  200~209=current showroom-mode slot (showroom reuses this variable to encode the broadcast value).
 * ================================================================ */
static _Atomic int s_sweep_step = 0;
static ui_sweep_clock_t s_sweep_clock;
static int  s_sweep_bl_last = -1;       // backlight (%) already applied during sweep, -1=not sweeping; write LEDC only on change, restore configured brightness at the end
static bool s_sweep_pending = false;    // BLE connected while the Logo was showing; trigger after the Logo goes away
static bool s_prev_ble_connected = false;

static void sweep_start(void)
{
    ui_sweep_clock_start(&s_sweep_clock, esp_timer_get_time() / 1000);
    s_sweep_step = 1;
}

static void sweep_stop(void)
{
    ui_sweep_clock_sync(&s_sweep_clock, 0, 0);
    s_sweep_step = 0;
}

/* ================================================================
 *  Showroom mode state
 * ================================================================ */
static volatile bool s_showroom_active = false;
static uint8_t s_showroom_tap_cnt = 0;
static uint32_t s_showroom_last_tap_ms = 0;
#define SHOWROOM_TAP_TIMEOUT_MS  1500
#define SHOWROOM_TAP_NEED        10
#define SHOWROOM_SLOT_COUNT      8
#define SHOWROOM_DATA_PAGES      4   // number of random data pages

// Fixed loop: Logo → animation → rand0 → Needle → rand1 → Chart → rand2 → rand3
// Ticks per slot (1 tick = 100ms), 0 = wait for the animation to finish
static const uint8_t s_showroom_slot_ticks[SHOWROOM_SLOT_COUNT] = {
    20,  // 0: Logo 2s
    0,   // 1: animation (wait until played)
    30,  // 2: random[0] 3s
    30,  // 3: Needle 3s
    30,  // 4: random[1] 3s
    30,  // 5: Chart 3s
    30,  // 6: random[2] 3s
    30,  // 7: random[3] 3s
};
static uint8_t  s_showroom_slot = 0;
static uint16_t s_showroom_tick = 0;
static uint8_t  s_showroom_rand_pages[SHOWROOM_DATA_PAGES];  // randomly chosen at startup
static lv_obj_t *s_showroom_video_scr = NULL;
// Slave: enter showroom or follow the slot after receiving the master's signal.
// Only the LVGL task reads/writes these (espnow recv goes through the app_event queue and is handled inside my_timerMain), so volatile is not needed.
static int  s_showroom_pending_slot = -1;  // -1=none, 0-7=slot to follow
static bool s_showroom_pending_enter = false;

/* ================================================================
 *  Boot / video / intro state
 * ================================================================ */
static volatile int s_intro_step = 0;
static int64_t s_boot_start_us = 0;
static int64_t s_intro_start_us = 0;
static bool    s_intro_shown = false;
static volatile bool s_boot_video_active = false;
static volatile bool s_boot_video_done = false;
static volatile bool s_boot_video_ready = false;
static int64_t s_boot_video_start_us = 0;
static lv_obj_t *s_boot_video_screen = NULL;
static lv_timer_t *s_boot_video_timer = NULL;
static bool s_boot_video_screen_shown = false;
static bool    s_boot_done = false;

// Video sync signals (reuses intro_step; values >5 never trigger RACE/AS/ONE rendering)
#define VIDEO_SYNC_READY  250
#define VIDEO_SYNC_PLAY   251

// "NO SIGNAL" overlay state
static lv_obj_t *s_no_signal_lbl = NULL;

// Forward declarations
static void boot_enter_default_page(void);

/* ================================================================
 *  Showroom helpers
 * ================================================================ */

// slot → page index
static uint8_t showroom_slot_to_page(uint8_t slot)
{
    switch (slot) {
        case 0: return 1;  // Logo
        case 1: {          // animation: depends on settings
            uint8_t ie = nvs_intro_enable_get();
            return (ie == 2) ? 0 : (ie == 1) ? 2 : 3;
        }
        case 3: return 8;  // Needle
        case 5: return 9;  // Chart
        default: {         // random data page
            uint8_t ri = (slot == 2) ? 0 : (slot == 4) ? 1 : (slot == 6) ? 2 : 3;
            return s_showroom_rand_pages[ri];
        }
    }
}

static void showroom_load_slot(uint8_t slot)
{
    uint8_t page = showroom_slot_to_page(slot);
    if (page == 0) {
        // Video page
        if (s_boot_video_ready || s_boot_video_active) return;  // already loaded
        // Boot animation is independent of the selected theme.
        if (s_showroom_video_scr) { lv_obj_del(s_showroom_video_scr); s_showroom_video_scr = NULL; }
        s_showroom_video_scr = lv_obj_create(NULL);
        if (!s_showroom_video_scr) return;
        lv_obj_set_style_bg_color(s_showroom_video_scr, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_showroom_video_scr, 255, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_showroom_video_scr, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(s_showroom_video_scr, 360, LV_PART_MAIN);
        lv_obj_clear_flag(s_showroom_video_scr, LV_OBJ_FLAG_SCROLLABLE);
        if (boot_video_create(s_showroom_video_scr, NULL, NULL)) {
            s_boot_video_ready = true;
        } else {
            lv_obj_del(s_showroom_video_scr);
            s_showroom_video_scr = NULL;
        }
    } else if (page == 2) {
        // RACE/AS/ONE: start the animation directly
        s_intro_step = 1;
        s_intro_shown = false;
        s_intro_start_us = esp_timer_get_time();
        if (ui_ScreenPageIntro == NULL) ui_ScreenPageIntro_screen_init();
        lv_scr_load_anim(ui_ScreenPageIntro, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, false);
    } else {
        // Regular pages
        static const struct { lv_obj_t **scr; void (*init)(void); } pages[] = {
            { NULL, NULL },  // 0 unused
            { &ui_ScreenPageLogo,       ui_ScreenPageLogo_screen_init },
            { NULL, NULL },  // 2 handled above
            { &ui_ScreenPageGear,       ui_ScreenPageGear_screen_init },
            { &ui_ScreenPageRpm,        ui_ScreenPageRpm_screen_init },
            { &ui_ScreenPageSpeed,      ui_ScreenPageSpeed_screen_init },
            { &ui_ScreenPageTemp,       ui_ScreenPageTemp_screen_init },
            { &ui_ScreenPageInfo,       ui_ScreenPageInfo_screen_init },
            { &ui_ScreenPageNeedle,     ui_ScreenPageNeedle_screen_init },
            { &ui_ScreenPageOilPressure,ui_ScreenPageOilPressure_screen_init },
        };
        if (page < 10 && pages[page].init) {
            if (*pages[page].scr == NULL) pages[page].init();
            lv_scr_load_anim(*pages[page].scr, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, false);
        }
    }
}

// Fake data generation (random walk)
static void showroom_fake_data(void) {
    static float fake_rpm = 2500, fake_spd = 60, fake_clt = 90, fake_oil = 95;
    static float fake_iat = 35, fake_load = 65, fake_tps = 40, fake_boost = 5;
    static float fake_afr = 14.7;
    fake_rpm  += ((float)(esp_random() % 200) - 100) * 2;  if (fake_rpm < 800) fake_rpm = 800; if (fake_rpm > 7500) fake_rpm = 7500;
    fake_spd  += ((float)(esp_random() % 20) - 10) * 0.5f; if (fake_spd < 0) fake_spd = 0; if (fake_spd > 200) fake_spd = 200;
    fake_clt  += ((float)(esp_random() % 4) - 2) * 0.1f;   if (fake_clt < 80) fake_clt = 80; if (fake_clt > 105) fake_clt = 105;
    fake_oil  += ((float)(esp_random() % 6) - 3) * 0.2f;   if (fake_oil < 80) fake_oil = 80; if (fake_oil > 130) fake_oil = 130;
    fake_iat  += ((float)(esp_random() % 4) - 2) * 0.1f;   if (fake_iat < 20) fake_iat = 20; if (fake_iat > 50) fake_iat = 50;
    fake_load += ((float)(esp_random() % 20) - 10);         if (fake_load < 10) fake_load = 10; if (fake_load > 100) fake_load = 100;
    fake_tps  += ((float)(esp_random() % 16) - 8);          if (fake_tps < 0) fake_tps = 0; if (fake_tps > 100) fake_tps = 100;
    fake_boost+= ((float)(esp_random() % 10) - 5) * 0.1f;  if (fake_boost < -5) fake_boost = -5; if (fake_boost > 20) fake_boost = 20;
    fake_afr  += ((float)(esp_random() % 10) - 5) * 0.04f; if (fake_afr < 10.0) fake_afr = 10.0; if (fake_afr > 18.0) fake_afr = 18.0;
    // obd.* only reads valid while sys.obd_link == DATA; showroom runs without an adapter.
    signal_store_set(SIG_SYS_OBD_LINK, SIG_LINK_DATA);
    obd_data_set_rpm((uint16_t)fake_rpm);
    obd_data_set_speed((uint8_t)fake_spd);
    obd_data_set_coolant_temp((int16_t)fake_clt);
    obd_data_set_oil_temp((int16_t)fake_oil);
    obd_data_set_intake_temp((int16_t)fake_iat);
    obd_data_set_load_pct((int16_t)fake_load);
    obd_data_set_tps((int16_t)fake_tps);
    obd_data_set_boost_x10((int16_t)(fake_boost * 10));
    obd_data_set_oil_pressure_x10((int16_t)(30 + (esp_random() % 40)));
    obd_data_set_afr_x100((int16_t)(fake_afr * 100));
}

/* ================================================================
 *  Boot / video helpers
 * ================================================================ */

// The worker owns its source snapshot and unpublished image cache. Completion
// transfers ownership to the LVGL task; no theme mutex is held while converting.
static theme_page_prepared_t *s_boot_theme_job;
static atomic_bool s_boot_theme_ready;
static bool s_boot_theme_attempted;

static void boot_video_finish(boot_video_state_t state);

static void boot_theme_worker(void *arg)
{
    int64_t start = esp_timer_get_time();
    theme_page_prepare_run(arg);
    ESP_LOGI(TAG, "Boot theme image preparation finished in %lld ms",
             (long long)((esp_timer_get_time() - start) / 1000));
    atomic_store_explicit(&s_boot_theme_ready, true, memory_order_release);
    lvgl_runtime_wake();
    vTaskDelete(NULL);
}

static void boot_theme_poll(lv_timer_t *timer)
{
    if (!atomic_load_explicit(&s_boot_theme_ready, memory_order_acquire)) return;
    // Avoid pausing the direct-to-panel decoder for widget creation.
    bool finish_video = s_boot_video_active;
    if (finish_video && boot_video_get_state() != BOOT_VIDEO_COMPLETED) return;
    int64_t start = esp_timer_get_time();
    lv_obj_t *screen = theme_page_prepare_finish(s_boot_theme_job);
    ESP_LOGI(TAG, "Boot theme page creation finished in %lld ms",
             (long long)((esp_timer_get_time() - start) / 1000));
    s_boot_theme_job = NULL;
    ui_ScreenPageThemeGauge_attach(screen);
    if (!screen) ESP_LOGW(TAG, "boot theme preparation failed; using built-in page");
    // Creating theme video widgets may replace the fullscreen player and
    // resume refresh. Switch now, before LVGL can redraw the empty video screen.
    if (finish_video) boot_video_finish(BOOT_VIDEO_COMPLETED);
    else if (!s_showroom_active && s_boot_video_done) boot_enter_default_page();
    lv_timer_del(timer);
}

static void boot_video_finish(boot_video_state_t state)
{
    if (state == BOOT_VIDEO_FAILED) ESP_LOGW(TAG, "Boot media failed; returning to Logo");
    else if (state == BOOT_VIDEO_COMPLETED && s_boot_theme_job)
        ESP_LOGI(TAG, "Boot video ended before theme was ready; returning to Logo");
    else ESP_LOGI(TAG, "Boot video finished");
    s_boot_video_active = false;
    s_boot_video_ready = false;
    boot_video_destroy();
    if (s_boot_video_timer) { lv_timer_del(s_boot_video_timer); s_boot_video_timer = NULL; }
    if (!s_showroom_active) {
        s_boot_video_done = true;
        if (s_boot_theme_job) {
            // On failure restore a real LVGL image while the worker finishes.
            // The direct-to-panel screen may contain no usable video frame.
            if (!ui_ScreenPageLogo) ui_ScreenPageLogo_screen_init();
            lv_scr_load(ui_ScreenPageLogo);
            if (s_boot_video_screen) lv_obj_del(s_boot_video_screen);
            s_boot_video_screen = NULL;
            return;
        }
        boot_enter_default_page();
        // The transition owns and deletes the previous screen.
        s_boot_video_screen = NULL;
    }
}

// Dedicated high-rate timer for video (33ms ≈ 30fps).
static void boot_video_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_boot_video_active) return;
    uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - s_boot_video_start_us) / 1000);
    boot_video_update(elapsed_ms);
    if (!s_showroom_active && s_boot_video_screen &&
        !s_boot_video_screen_shown && boot_video_first_frame_presented()) {
        // Fullscreen OBV pauses LVGL refresh while it decodes. Keep the already
        // painted Logo active until a complete video frame has reached the LCD.
        lv_scr_load(s_boot_video_screen);
        s_boot_video_screen_shown = true;
        ESP_LOGI(TAG, "Boot video first frame displayed; leaving Logo");
    }
    boot_video_state_t state = boot_video_get_state();
    if (state != BOOT_VIDEO_RUNNING) boot_video_finish(state);
}

// Pairing is needed only for unbound slaves or units without a saved OBD adapter.
static bool boot_needs_ble_scan(const nvs_user_cfg_t *cfg)
{
    if (cfg->device_role == ESPNOW_ROLE_SLAVE) {
        uint8_t mac[6];
        espnow_link_get_bound_master_mac(mac);
        return (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) == 0;
    }
    return cfg->ble_device_name[0] == '\0';
}

void ui_ext_boot_prepare(void)
{
    nvs_user_cfg_t cfg = nvs_cfg_read();
    if (theme_page_list_count() > 0 && !s_boot_theme_attempted) {
        s_boot_theme_attempted = true;
        ui_theme_gauge_page_index = theme_page_default_index();
        s_boot_theme_job = theme_page_prepare_begin(theme_page_list_at(ui_theme_gauge_page_index));
        if (s_boot_theme_job) {
            atomic_store_explicit(&s_boot_theme_ready, false, memory_order_relaxed);
            lv_timer_t *poll = lv_timer_create(boot_theme_poll, 20, NULL);
            if (!poll || xTaskCreate(boot_theme_worker, "theme_boot", 6144, s_boot_theme_job, 3, NULL) != pdPASS) {
                // Cancel without running: do not perform the conversion under
                // the LVGL mutex on allocation failure.
                if (poll) lv_timer_del(poll);
                theme_page_prepare_finish(s_boot_theme_job);
                s_boot_theme_job = NULL;
                ESP_LOGW(TAG, "could not start boot theme worker; using built-in page");
            }
        }
    }
    if (theme_page_list_count() == 0 && boot_needs_ble_scan(&cfg) &&
        !ui_ScreenPageBLEScan) {
        /* Build before playback; the screen starts scanning only when shown. */
        ui_ScreenPageBLEScan_screen_init();
    }
}

static void boot_load_destination(lv_obj_t *screen)
{
    // Fullscreen OBV writes directly to the panel; its LVGL screen has no
    // image of the last frame. A fade would redraw that empty black screen.
    bool from_video = s_boot_video_screen && lv_scr_act() == s_boot_video_screen;
    lv_scr_load_anim(screen, from_video ? LV_SCR_LOAD_ANIM_NONE : LV_SCR_LOAD_ANIM_FADE_ON,
                     from_video ? 0 : 300, 0, true);
}

static void boot_goto_ble_scan(void)
{
    if (ui_ScreenPageBLEScan == NULL) ui_ScreenPageBLEScan_screen_init();
    boot_load_destination(ui_ScreenPageBLEScan);
    ui_ScreenPageLogo = NULL;
    s_boot_done = true;
}

// Enter the default page when boot finishes (called on Logo timeout or after the boot animation completes)
static void boot_enter_default_page(void)
{
    if (s_boot_theme_job) return; // keep Logo / intro / video visible
    nvs_user_cfg_t pg_cfg = nvs_cfg_read();

    // If a custom theme with pages is loaded, boot directly into its default page ("main_gauge" or the first)
    uint8_t page_count = theme_page_list_count();
    if (page_count > 0) {
        ui_theme_gauge_page_index = theme_page_default_index();
        // The worker prepared this page during Logo / animation playback.
        // A failed preparation falls back; never retry expensive work here.
        if (!s_boot_theme_attempted && !ui_ScreenPageThemeGauge) ui_ScreenPageThemeGauge_screen_init();
        if (ui_ScreenPageThemeGauge) {   // build failed → fall back to the built-in default page below
            boot_load_destination(ui_ScreenPageThemeGauge);
            ESP_LOGI(TAG, "Boot entered theme page");
            ui_ScreenPageLogo = NULL;
            s_boot_done = true;
            if(s_sweep_pending) { s_sweep_pending = false; sweep_start(); }
            return;
        }
    }

    if (boot_needs_ble_scan(&pg_cfg)) {
        boot_goto_ble_scan();
        return;
    }

    lv_obj_t **target_scr = NULL;
    void (*target_init)(void) = NULL;
    // Default page (NVS indices, append only): 0=Temp,1=Info,2=Chart,3=Needle,4=Gear,5=Rpm,6=Speed,7=TPMS
    switch(pg_cfg.default_page) {
        case 0: target_scr = &ui_ScreenPageTemp;  target_init = ui_ScreenPageTemp_screen_init;  break;
        case 1: target_scr = &ui_ScreenPageInfo;  target_init = ui_ScreenPageInfo_screen_init;  break;
        case 2: target_scr = &ui_ScreenPageOilPressure; target_init = ui_ScreenPageOilPressure_screen_init;  break;
        case 3: target_scr = &ui_ScreenPageNeedle; target_init = ui_ScreenPageNeedle_screen_init;  break;
        case 4: target_scr = &ui_ScreenPageGear;  target_init = ui_ScreenPageGear_screen_init;  break;
        case 5: target_scr = &ui_ScreenPageRpm;   target_init = ui_ScreenPageRpm_screen_init;   break;
        case 6: target_scr = &ui_ScreenPageSpeed; target_init = ui_ScreenPageSpeed_screen_init; break;
        case 7: target_scr = &ui_ScreenPageTPMS; target_init = ui_ScreenPageTPMS_screen_init; break;
        default: target_scr = &ui_ScreenPageTemp; target_init = ui_ScreenPageTemp_screen_init;  break;
    }
    if(*target_scr == NULL) target_init();
    boot_load_destination(*target_scr);
    ui_ScreenPageLogo = NULL;
    s_boot_done = true;
    if(s_sweep_pending) { s_sweep_pending = false; sweep_start(); }  // a sweep deferred while the boot animation played fires now, as the default page loads
}

/* ================================================================
 *  Public state accessors (declared in ui.h, used by espnow/elm327/my_timerMain)
 * ================================================================ */

int  ui_sweep_get_step(void) { return s_sweep_step; }
bool ui_showroom_is_active(void) { return s_showroom_active; }
int  ui_intro_get_step(void) { return s_intro_step; }

void ui_showroom_set_active(bool en) {
    s_showroom_active = en;
    s_showroom_tap_cnt = 0;
    if (en) {
        ESP_LOGD("showroom", "ENTER role=%d", nvs_cfg_read().device_role);
        sweep_stop();
        // pick 4 random data pages; the position offset guarantees the three gauges differ
        static const uint8_t pool[] = { 3, 4, 5, 6, 7 };
        uint8_t pos = nvs_device_position_get();
        if (pos < 1 || pos > 3) pos = 1;
        for (int i = 0; i < SHOWROOM_DATA_PAGES; i++)
            s_showroom_rand_pages[i] = pool[(i * 3 + pos - 1) % 5];
        // start from slot 0
        s_showroom_slot = 0;
        s_showroom_tick = 0;
        showroom_load_slot(0);
    } else {
        ESP_LOGD("showroom", "EXIT");
    }
}

// Master's broadcast values arrive via the app_event queue; slaves follow them here.
void ui_showroom_set_page_from_sync(int sweep_step) {
    if (sweep_step >= 200 && sweep_step < 210) {
        // follow the master's slot
        s_showroom_pending_slot = sweep_step - 200;
        if (!s_showroom_active) {
            // Joining showroom mode mid-way: enter via slot 0 (Logo) first; the next real slot
            // broadcast by the master corrects it shortly (~100ms). That one switch is an expected brief transition, not a bug.
            s_showroom_pending_slot = -1;
            s_showroom_pending_enter = true;
            sweep_stop();
        }
    } else if (sweep_step >= 100 && sweep_step < 200 && !s_showroom_active) {
        s_showroom_pending_enter = true;
        sweep_stop();
    } else if (!s_showroom_active) {
        // Preserve coarse sync slots for flash/end; the visual ramp fills
        // the gaps between master packets without changing received state.
        ui_sweep_clock_sync(&s_sweep_clock, sweep_step, esp_timer_get_time() / 1000);
        s_sweep_step = s_sweep_clock.synced_step;
    }
}

// Triple-gauge boot animation sync: the master drives s_intro_step along the timeline and broadcasts it; slaves follow via espnow recv writes.
//   0=not started/still on logo, 1=TC, 2=+-, 3=+OFF, 4=all shown (hold), 255=done→enter page
void ui_intro_set_step(int step) {
    // In showroom mode each gauge self-drives its intro animation and ignores master overrides
    if (s_showroom_active) return;
    s_intro_step = step;
}

/* ================================================================
 *  Showroom API (called from my_timerMain / ui_event_easter_egg_background)
 * ================================================================ */

bool ui_ext_showroom_is_active(void) { return s_showroom_active; }

// Tap counting (shared by the version page / any page)
void ui_ext_showroom_handle_tap(void) {
    if (s_showroom_active) return;  // already inside, ignore taps
    uint32_t now = lv_tick_get();
    if (now - s_showroom_last_tap_ms > SHOWROOM_TAP_TIMEOUT_MS) s_showroom_tap_cnt = 0;
    s_showroom_last_tap_ms = now;
    s_showroom_tap_cnt++;
    if (s_showroom_tap_cnt >= SHOWROOM_TAP_NEED) {
        s_showroom_tap_cnt = 0;
        ui_showroom_set_active(true);  // enter only, never exit; leave via power cycle
    }
}

// The whole showroom state machine (moved out of my_timerMain):
// master drives slots, slaves follow; fake data + page switching + video slot playback + slot broadcast.
void ui_ext_showroom_tick(bool is_slave)
{
    // Slave: enter showroom
    if (!s_showroom_active && s_showroom_pending_enter) {
        s_showroom_pending_enter = false;
        s_boot_done = true;
        ui_showroom_set_active(true);
    }
    // Slave: follow the master's slot
    if (s_showroom_active && is_slave && s_showroom_pending_slot >= 0) {
        uint8_t new_slot = (uint8_t)s_showroom_pending_slot;
        s_showroom_pending_slot = -1;
        if (new_slot != s_showroom_slot) {
            // clean up the current slot
            uint8_t old_page = showroom_slot_to_page(s_showroom_slot);
            if (old_page == 0 && s_boot_video_active) {
                s_boot_video_active = false; s_boot_video_ready = false;
                boot_video_destroy();
                if (s_boot_video_timer) { lv_timer_del(s_boot_video_timer); s_boot_video_timer = NULL; }
            }
            if (old_page == 2) { s_intro_step = 0; s_intro_shown = false; }
            // jump to the master's slot
            s_showroom_slot = new_slot;
            s_showroom_tick = 0;
            showroom_load_slot(s_showroom_slot);
        }
    }
    if (s_showroom_active && !ui_ext_sweep_active()) {
        if (!is_slave) showroom_fake_data();

        // animation slot: start playing automatically once the video is ready (runs on both master and slave)
        uint8_t cur_page = showroom_slot_to_page(s_showroom_slot);
        if (s_showroom_slot == 1 && cur_page == 0 && s_boot_video_ready && !s_boot_video_active) {
            lv_scr_load(s_showroom_video_scr);
            s_boot_video_active = true;
            s_boot_video_start_us = esp_timer_get_time();
            if (!s_boot_video_timer)
                s_boot_video_timer = lv_timer_create(boot_video_timer_cb, 33, NULL);
        }

        // Local tick page switching:
        //   master: runs for all slots
        //   slave: only Logo(0) and animation(1) run local ticks (keeps animation switching in sync);
        //          other slots purely follow the master's broadcast (avoids flash-through)
        bool should_tick = !is_slave || s_showroom_slot <= 1;
        if (should_tick) {
            bool anim_playing = (cur_page == 0 && s_boot_video_active) ||
                                (cur_page == 2 && s_intro_step > 0 && s_intro_step < 255);
            if (!anim_playing) {
                s_showroom_tick++;
            }
            uint8_t max_t = s_showroom_slot_ticks[s_showroom_slot];
            if (max_t == 0) {
                max_t = anim_playing ? 255 : 1;
            }
            if (s_showroom_tick >= max_t) {
                // clean up when leaving the current slot
                if (cur_page == 0 && s_boot_video_active) {
                    s_boot_video_active = false;
                    s_boot_video_ready = false;
                    boot_video_destroy();
                    if (s_boot_video_timer) { lv_timer_del(s_boot_video_timer); s_boot_video_timer = NULL; }
                }
                if (cur_page == 2) { s_intro_step = 0; s_intro_shown = false; }
                s_showroom_slot = (s_showroom_slot + 1) % SHOWROOM_SLOT_COUNT;
                s_showroom_tick = 0;
                showroom_load_slot(s_showroom_slot);
            }
        }
        // master broadcasts the current slot (slaves use it to correct drift)
        if (!is_slave) s_sweep_step = 200 + s_showroom_slot;
    }
}

/* ================================================================
 *  Sweep API (called from my_timerMain)
 * ================================================================ */

bool ui_ext_sweep_active(void)
{
    const int step = s_sweep_step;
    return step > 0 && step <= SWEEP_TOTAL;
}

int ui_ext_sweep_get_step(void)
{
    return s_sweep_step;
}

void ui_ext_sweep_trigger(bool ble_now, bool is_slave)
{
    if (is_slave) return;
    if (ble_now && !s_prev_ble_connected) {
        if (s_boot_done) {
            sweep_start();          // boot animations (Logo/SKY GAUGE/RACE AS ONE) all finished, sweep immediately
        } else {
            s_sweep_pending = true; // boot animation still playing; defer and fire when the default page loads
        }
    }
    s_prev_ble_connected = ble_now;
}

float ui_ext_sweep_tick(bool is_slave, uint8_t configured_brightness)
{
    ui_sweep_frame_t frame = { .ratio = -1.0f };
    if (ui_ext_sweep_active()) {
        frame = ui_sweep_clock_sample(&s_sweep_clock, is_slave, esp_timer_get_time() / 1000);
        if (!is_slave) s_sweep_step = frame.step;
    }
    if (!frame.step) {
        // Restore brightness on the same frame that returns to live data.
        if (s_sweep_bl_last >= 0) {
            uint8_t d = configured_brightness;
            if (d < 10) d = 100;
            Set_Backlight(d);
            s_sweep_bl_last = -1;
        }
        return -1.0f;
    }
    if (frame.backlight != s_sweep_bl_last) {
        Set_Backlight(frame.backlight);
        s_sweep_bl_last = frame.backlight;
    }
    return frame.ratio;
}

/* ================================================================
 *  Boot animation API (called from my_timerMain)
 * ================================================================ */

// Video boot mode (INTRO == 2): prepare/play the boot video (media/boot_video.h), syncing master/slaves.
// Returns true when my_timerMain should return early (still showing the Logo or the video is playing).
bool ui_ext_boot_video_tick(void)
{
    uint8_t intro_val = nvs_intro_enable_get();
    bool want_video = (intro_val == 2);
    if (s_boot_done || s_showroom_active || s_boot_video_done || !want_video) return false;

    // show the Logo for 1 second first, then enter the video
    static int64_t s_video_logo_start_us = 0;
    if (s_video_logo_start_us == 0) s_video_logo_start_us = esp_timer_get_time();
    int64_t logo_el_ms = (esp_timer_get_time() - s_video_logo_start_us) / 1000;
    if (logo_el_ms < 1000) return true;  // Logo still showing, skip

    // Phase 1: prepare boot media from the shared SD boot directory.
    if (!s_boot_video_ready && !s_boot_video_active && s_boot_video_screen == NULL) {
        boot_video_src_t src = BOOT_VIDEO_SRC_NONE;
        s_boot_video_screen = lv_obj_create(NULL);
        if (!s_boot_video_screen) {
            ESP_LOGW(TAG, "could not allocate boot video screen");
            s_boot_video_done = true;
            return false;
        }
        lv_obj_set_style_bg_color(s_boot_video_screen, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_boot_video_screen, 255, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_boot_video_screen, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(s_boot_video_screen, 360, LV_PART_MAIN);
        lv_obj_clear_flag(s_boot_video_screen, LV_OBJ_FLAG_SCROLLABLE);
        if (boot_video_create(s_boot_video_screen, NULL, &src)) {
            ESP_LOGI(TAG, "boot video source %d, creating player", (int)src);
            // no lv_scr_load yet: keep the Logo screen, switch only when playback starts
            s_boot_video_ready = true;
            ESP_LOGI(TAG, "Boot video ready (logo kept)");
        } else {
            ESP_LOGW(TAG, "no usable SD boot media - boot animation skipped");
            lv_obj_del(s_boot_video_screen);
            s_boot_video_screen = NULL;
            s_boot_video_done = true;
        }
    }
    // Phase 2: wait for the sync signal before playing
    if (s_boot_video_ready && !s_boot_video_active) {
        bool should_start = false;
        uint8_t role = nvs_cfg_read().device_role;
        if (role == ESPNOW_ROLE_STANDALONE) {
            // standalone: play directly
            should_start = true;
        } else if (role == ESPNOW_ROLE_MASTER) {
            // master: broadcast READY → wait for slaves to come online → wait 800ms prep → broadcast PLAY + play itself
            // with no slave after a 5s timeout, play anyway
            static int64_t s_master_ready_us = 0;
            static bool s_master_slaves_seen = false;
            if (s_master_ready_us == 0) {
                s_master_ready_us = esp_timer_get_time();
                s_intro_step = VIDEO_SYNC_READY;
            }
            if (!s_master_slaves_seen && espnow_master_online_slaves() > 0) {
                s_master_slaves_seen = true;
                s_master_ready_us = esp_timer_get_time();  // restart timing, wait 800ms from now
            }
            int64_t wait_ms = (esp_timer_get_time() - s_master_ready_us) / 1000;
            if ((s_master_slaves_seen && wait_ms >= 800) || wait_ms > 5000) {
                s_intro_step = VIDEO_SYNC_PLAY;
                should_start = true;
                s_master_ready_us = 0;
                s_master_slaves_seen = false;
            }
        } else {
            // slave: wait for the PLAY signal, 5s timeout as fallback
            static int64_t s_slave_wait_us = 0;
            if (s_slave_wait_us == 0) s_slave_wait_us = esp_timer_get_time();
            int64_t wait_ms = (esp_timer_get_time() - s_slave_wait_us) / 1000;
            int intro = s_intro_step;
            if (intro >= VIDEO_SYNC_PLAY || wait_ms > 5000) {
                should_start = true;
                s_slave_wait_us = 0;
            }
        }
        if (should_start) {
            // Start fullscreen playback while retaining the Logo as LVGL's
            // active screen. The video timer switches screens only after the
            // first complete frame has been written to the panel.
            boot_video_update(0);
            s_boot_video_screen_shown = false;
            s_boot_video_active = true;
            s_boot_video_start_us = esp_timer_get_time();
            s_boot_video_timer = lv_timer_create(boot_video_timer_cb, 33, NULL);
            ESP_LOGD(TAG, "Boot video started");
        }
    }
    if (s_boot_video_active || s_boot_video_ready) return true;
    return false;
}

// Boot flow / Showroom Intro playback (RACE/AS/ONE).
void ui_ext_intro_tick(bool is_slave)
{
    bool run_intro = (!s_boot_done && !s_showroom_active) ||
                     (s_showroom_active && showroom_slot_to_page(s_showroom_slot) == 2);
    if (run_intro)
    {
        int64_t now_us = esp_timer_get_time();
        if (s_boot_start_us == 0) s_boot_start_us = now_us;
        int64_t boot_el = now_us - s_boot_start_us;
        bool intro_en = nvs_intro_enable_get();
        bool in_showroom_intro = s_showroom_active && showroom_slot_to_page(s_showroom_slot) == 2;

        if (!is_slave || in_showroom_intro) {
            if (s_intro_step == 0) {
                if (in_showroom_intro || (intro_en == 1 && espnow_master_online_slaves() > 0)) {
                    s_intro_start_us = now_us;
                    s_intro_step = 1;
                } else if ((intro_en != 1 && boot_el > 1000000) ||
                           (intro_en == 1 && boot_el > 3000000)) {
                    s_intro_step = 255;
                }
            } else if (s_intro_step != 255) {
                int64_t el = now_us - s_intro_start_us;
                s_intro_step = (el < 500000) ? 1 : (el < 1000000) ? 2 : (el < 1500000) ? 3
                             : (el < 2000000) ? 4 : (el < 3000000) ? 5 : 255;
            }
        } else {
            // slave: wait for the master's sync signal; only jump to the default page after a 5s timeout
            if (s_intro_step == 0) {
                if (boot_el > 5000000) s_intro_step = 255;
            }
        }

        // Rendering + screen switch
        if (s_intro_step >= 1 && s_intro_step <= 5) {
            static int8_t s_last_intro_word = -1;

            if (!s_intro_shown) {
                s_last_intro_word = -1;
                if (ui_ScreenPageIntro == NULL) ui_ScreenPageIntro_screen_init();
                if (!in_showroom_intro) {
                    lv_scr_load_anim(ui_ScreenPageIntro, LV_SCR_LOAD_ANIM_FADE_ON, 150, 0, false);
                    ui_ScreenPageLogo = NULL;
                }
                s_intro_shown = true;
            }
            if (ui_LabelIntroWord) {
                static const char *words[] = {"", "RACE", "AS", "ONE"};
                uint8_t pos = nvs_device_position_get();
                if (pos < 1 || pos > 3) pos = 1;
                int8_t word_idx = (s_intro_step >= pos + 1) ? (int8_t)pos : 0;
                if (word_idx != s_last_intro_word) {
                    s_last_intro_word = word_idx;
                    lv_label_set_text(ui_LabelIntroWord, words[word_idx]);
                }
            }
        } else if (s_intro_step == 255 && !in_showroom_intro) {
            boot_enter_default_page();   // enter the default page only at boot; showroom doesn't jump away
        }
        // s_intro_step==0: still waiting on the Logo
    }
}

/* ================================================================
 *  "NO SIGNAL" overlay: shown on top of gauge pages when the master's OBD BLE
 *  disconnects / a slave receives no master data.
 *  Disconnection is reflected instantly (ble_now already means "connected"/"data within
 *  the last ~2s"), no extra timing needed.
 * ================================================================ */
void ui_ext_no_signal_update(bool signal_ok)
{
    static bool s_no_signal_visible = false;
    lv_obj_t *act = lv_scr_act();
    // only warn on the pages that actually display gauge data; settings/scan/boot-animation pages don't need it
    bool on_gauge_page = (act == ui_ScreenPageTemp || act == ui_ScreenPageInfo ||
                           act == ui_ScreenPageOilPressure || act == ui_ScreenPageNeedle ||
                           act == ui_ScreenPageGear || act == ui_ScreenPageRpm ||
                           act == ui_ScreenPageSpeed);
    bool show = s_boot_done && on_gauge_page && !signal_ok;

    if (show) {
        if (!s_no_signal_lbl) {
            s_no_signal_lbl = lv_label_create(lv_layer_top());
            lv_obj_clear_flag(s_no_signal_lbl, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_text_font(s_no_signal_lbl, &ui_font_FontTypoderSize16, LV_PART_MAIN);
            lv_obj_set_style_text_color(s_no_signal_lbl, lv_color_hex(0xFF4D4D), LV_PART_MAIN);
            lv_obj_set_style_bg_color(s_no_signal_lbl, lv_color_hex(0x000000), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(s_no_signal_lbl, 160, LV_PART_MAIN);
            lv_obj_set_style_pad_hor(s_no_signal_lbl, 10, LV_PART_MAIN);
            lv_obj_set_style_pad_ver(s_no_signal_lbl, 4, LV_PART_MAIN);
            lv_obj_set_style_radius(s_no_signal_lbl, 6, LV_PART_MAIN);
            lv_label_set_text(s_no_signal_lbl, "NO SIGNAL");
            lv_obj_align(s_no_signal_lbl, LV_ALIGN_TOP_MID, 0, 34);
        }
        if (!s_no_signal_visible) {
            lv_obj_clear_flag(s_no_signal_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_no_signal_lbl);
            s_no_signal_visible = true;
        }
    } else if (s_no_signal_lbl && s_no_signal_visible) {
        lv_obj_add_flag(s_no_signal_lbl, LV_OBJ_FLAG_HIDDEN);
        s_no_signal_visible = false;
    }
}

/* ================================================================
 *  Lifecycle
 * ================================================================ */

/* ================================================================
 *  SD theme selection: USB preparation and legacy interrupted selection
 * ================================================================ */

#define THEME_LIB_POLL_MS 500

esp_err_t ui_ext_theme_prepare_install(void)
{
    if (s_boot_theme_job || ui_theme_navigation_busy() || theme_library_get_state(NULL) != THEME_LIB_IDLE) return ESP_ERR_INVALID_STATE;
    if (ui_ScreenPageThemeGauge) {
        if (ui_ScreenPageThemeGauge == lv_scr_act()) {
            if (!ui_ScreenPageSettings) ui_ScreenPageSettings_screen_init();
            if (!ui_ScreenPageSettings) return ESP_ERR_NO_MEM;
            lv_scr_load(ui_ScreenPageSettings);
        }
        lv_obj_del(ui_ScreenPageThemeGauge);
        ui_ScreenPageThemeGauge = NULL;
    }
    theme_unload();
    theme_info_t info;
    return theme_get_info(&info) == ESP_OK ? ESP_ERR_INVALID_STATE : ESP_OK;
}

static void theme_lib_timer_cb(lv_timer_t *t)
{
    (void)t;
    theme_lib_state_t st = theme_library_get_state(NULL);
    if (st != THEME_LIB_DONE && st != THEME_LIB_ERROR) return;
    if (s_boot_theme_job || ui_theme_navigation_busy()) return; // finish SD readers before reloading their source
    if (st == THEME_LIB_ERROR) {
        ESP_LOGW("ui_ext", "Theme selection failed: %s", theme_library_last_error());
    }
    // A legacy interrupted selection may finish after boot or card insertion.
    esp_err_t err = theme_load(0);
    ESP_LOGI("ui_ext", "SD theme selection reload: %s", esp_err_to_name(err));
    theme_library_ack();
}

void ui_ext_init(void)
{
    lv_timer_create(theme_lib_timer_cb, THEME_LIB_POLL_MS, NULL);
}

/* ================================================================
 *  RPM over-limit flash warning  (migrated from ui.c my_timerMain)
 *
 *  Strobes red/black when RPM >= warn threshold, plus the triple-gauge
 *  linked ramp: the band [threshold-1000, threshold] is split into thirds
 *  and gauges 1/2/3 (per their position) ramp black→red in turn, all three
 *  strobing together at the threshold. Each gauge computes locally from its
 *  own position + the same ESP-NOW-synced RPM, so no extra inter-gauge
 *  communication is needed and they stay in sync naturally.
 * ================================================================ */

// RPM warning test mode
volatile int s_rpm_flash_test_ticks = 0;
void ui_rpm_flash_test_start(void) { s_rpm_flash_test_ticks = 60; }  // ~2s test flash
// Note: the RPM ramp for the multi-gauge linked test is driven centrally by the master (espnow_link.c writes the RPM override layer and broadcasts it);
//     this unit only renders per its own position, no local simulation, keeping all three gauges in sync.

static bool s_rpm_flash_red = false;  // flash state (red/black toggle, drives the background color)
static bool s_rpm_flashing = false;   // whether strobing right now (stable flag, drives the timer's fast flash; does not toggle with red/black)
static bool s_rpm_link_ramp = false;  // multi-gauge linked flash: this unit is inside its ramp segment
static bool s_rpm_link_bg = false;    // multi-gauge linked flash: background is taken by the linked red (must restore theme bg when leaving)

bool ui_ext_rpm_is_flashing(void) { return s_rpm_flashing; }
bool ui_ext_rpm_link_ramp_active(void) { return s_rpm_link_ramp; }

// Alarm over: built-in pages get the palette bg + dial face back; a theme page
// puts its own layout background back in theme_tick() once rules resume.
static void rpm_restore_bg(lv_obj_t *scr)
{
    if (scr != ui_ScreenPageThemeGauge) ui_helpers_style_screen_bg(scr);
    lv_obj_set_style_bg_opa(scr, 255, LV_PART_MAIN);
}

// Whether any RPM flash mode could be active (used by my_timerMain's OBD snapshot gate)
bool ui_ext_rpm_warn_possible(const nvs_user_cfg_t *cfg, bool linked_flash_on)
{
    if (!cfg) return false;
    return cfg->rpm_warn_anim_en ||
           (cfg->device_role != ESPNOW_ROLE_STANDALONE &&
            linked_flash_on);
}

void ui_ext_rpm_flash_tick(uint16_t usRpm, bool in_sweep,
                           const nvs_user_cfg_t *cfg, bool linked_flash_on)
{
    if (!cfg) return;
    uint16_t warn_thresh = cfg->rpm_warn_threshold; // already clamped to [1000,...]/default 6000 in nvs_storage_init()
    // Linked flash and FLASH ANIM are mutually exclusive (the settings page ensures at most one is on); with linked on, the at-threshold strobe does not depend on anim_en.
    // While a link test is running (linktest_active), this unit renders even if LINKED FLASH is off locally, so the all-gauge sync test works.
    // espnow_link_linked_en(): master = local NVS, slave = mirrored from master's broadcast (bit2 in flags).
    // This ensures slaves render the gradient without needing manual NVS config on each unit.
    bool linked_on = linked_flash_on && (cfg->device_role != ESPNOW_ROLE_STANDALONE);

    // The linked-test RPM ramp is written by the master into the RPM override layer and broadcast via ESP-NOW; usRpm is the synced value, identical on all three gauges
    bool over = (!in_sweep && (cfg->rpm_warn_anim_en || linked_on) && usRpm >= warn_thresh);
    // Test mode: force trigger
    if (s_rpm_flash_test_ticks > 0) { over = true; s_rpm_flash_test_ticks--; }

    // Stable flag: stays true while strobing, driving the timer to flash at RPM_FLASH_PERIOD_MS (independent of the red/black toggle)
    bool was_flashing = s_rpm_flashing;
    s_rpm_flashing = over;

    // Red/black strobe. Custom flash artwork is a theme rule now (obd.rpm → set.asset), see docs/THEMES.md.
    if (over) {
        s_rpm_flash_red = !s_rpm_flash_red;
        lv_obj_t *scr = lv_scr_act();
        // Drop the dial-face artwork while strobing — a background image
        // covers bg_color, so the flash would otherwise be invisible.
        lv_obj_set_style_bg_img_src(scr, NULL, LV_PART_MAIN);
        lv_obj_set_style_bg_color(scr, s_rpm_flash_red ? lv_color_hex(UI_SEM_FLASH) : lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(scr, 255, LV_PART_MAIN);
    } else {
        if (was_flashing || s_rpm_flash_red) {   // also when the strobe ended on its black phase
            rpm_restore_bg(lv_scr_act());
            s_rpm_flash_red = false;
        }
    }

    // ---- Multi-gauge linked strobe (triple-gauge mode) ----
    {
        bool linked_drawn = false;
        if (linked_on && !over && !in_sweep && warn_thresh > 1000 && usRpm < warn_thresh) {
            uint32_t base = (uint32_t)warn_thresh - 1000;   // start RPM = threshold minus 1000 rpm
            uint8_t pos = nvs_device_position_get();        // this unit's position 1/2/3
            if (pos >= 1 && pos <= 3) {
                uint32_t seg_start = base + 1000u * (pos - 1) / 3;
                uint32_t seg_end   = base + 1000u * pos / 3;
                lv_obj_t *scr = lv_scr_act();
                if (usRpm >= seg_end) {
                    // this unit's segment completed: solid red
                    lv_obj_set_style_bg_img_src(scr, NULL, LV_PART_MAIN);
                    lv_obj_set_style_bg_color(scr, lv_color_hex(0xFF0000), LV_PART_MAIN);
                    lv_obj_set_style_bg_opa(scr, 255, LV_PART_MAIN);
                    s_rpm_link_ramp = false;
                    linked_drawn = true;
                } else if (usRpm > seg_start && seg_end > seg_start) {
                    // inside this unit's segment: full-screen black→red ramp, alpha rising linearly with RPM
                    uint8_t alpha = (uint8_t)(((uint32_t)usRpm - seg_start) * 255 / (seg_end - seg_start));
                    lv_obj_set_style_bg_img_src(scr, NULL, LV_PART_MAIN);
                    lv_obj_set_style_bg_color(scr,
                        lv_color_mix(lv_color_hex(0xFF0000), lv_color_hex(0x000000), alpha), LV_PART_MAIN);
                    lv_obj_set_style_bg_opa(scr, 255, LV_PART_MAIN);
                    s_rpm_link_ramp = true;
                    linked_drawn = true;
                }
            }
        }
        if (linked_drawn) {
            s_rpm_link_bg = true;
        } else if (!over) {
            // during over (strobe) the flash logic owns the background, don't touch it here; otherwise, leaving the linked red must restore black
            s_rpm_link_ramp = false;
            if (s_rpm_link_bg) {
                rpm_restore_bg(lv_scr_act());
                s_rpm_link_bg = false;
            }
        }
    }

    // System alarms own the background: theme rules pause while they draw.
    theme_set_rules_suppressed(over || s_rpm_link_bg);
}
