#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_obd_dsp/signal_store.h"
#include "app_obd_dsp/obd_data_cache.h"
#include "freertos/task.h"

static int64_t s_now_us;
static unsigned s_timer_calls;

void *heap_caps_calloc(size_t n, size_t size, unsigned caps)
{
    (void)caps;
    return calloc(n, size);
}

int64_t esp_timer_get_time(void)
{
    s_timer_calls++;
    return s_now_us;
}

TickType_t xTaskGetTickCount(void)
{
    return (TickType_t)(s_now_us / 1000);
}

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        return 1; \
    } \
} while (0)

int main(void)
{
    signal_store_init();
    CHECK(signal_store_veh_register(NULL, 0, 1000, 5000) == SIG_SLOT_NONE);
    CHECK(signal_store_veh_register("obd.rpm", 0, 1000, 5000) == SIG_SLOT_NONE);
    CHECK(signal_store_veh_register("veh.Upper", 0, 1000, 5000) == SIG_SLOT_NONE);
    CHECK(signal_store_veh_register("veh.bad-key", 0, 1000, 5000) == SIG_SLOT_NONE);
    CHECK(signal_store_veh_register("veh.", 0, 1000, 5000) == SIG_SLOT_NONE);
    CHECK(signal_store_veh_register("veh.abcdefghijklmnopqrstuvwxyz", 0, 1000, 5000) == SIG_SLOT_NONE);
    CHECK(!signal_store_is_local(SIG_OBD_RPM));
    signal_store_set_local(SIG_OBD_RPM, true);
    CHECK(signal_store_is_local(SIG_OBD_RPM));
    signal_store_set_local(SIG_OBD_RPM, false);
    CHECK(!signal_store_is_local(SIG_OBD_RPM));

    int veh_slot = signal_store_veh_register("veh.test", 0, 1000, 5000);
    char veh_key[32];
    CHECK(veh_slot == SIG_VEH_BASE);
    CHECK(signal_store_veh_count() == 1);
    CHECK(signal_store_slot_by_id(SIG_VEH_ID_BASE) == veh_slot);
    CHECK(signal_store_num_id(veh_slot) == SIG_VEH_ID_BASE);
    CHECK(signal_store_copy_key(veh_slot, veh_key, sizeof(veh_key)));
    CHECK(strcmp(veh_key, "veh.test") == 0);
    signal_store_veh_clear();
    CHECK(signal_store_veh_count() == 0);
    CHECK(signal_store_slot_by_id(SIG_VEH_ID_BASE) == SIG_SLOT_NONE);
    CHECK(!signal_store_copy_key(veh_slot, veh_key, sizeof(veh_key)));
    CHECK(veh_key[0] == '\0');

    CHECK(signal_store_set(SIG_SYS_OBD_LINK, SIG_LINK_DATA));
    obd_data_set_rpm(3500);
    CHECK(obd_data_get_rpm() == 3500);
    obd_data_rpm_override_set(true, 7200);
    CHECK(obd_data_get_rpm() == 7200);
    obd_data_rpm_override_set(false, 0);
    CHECK(obd_data_get_rpm() == 3500);
    CHECK(signal_store_set(SIG_OBD_RPM, 3500));
    /* Rapid updates retain the actual samples, including a real stop. */
    int32_t sample;
    obd_data_set_speed(60); CHECK(signal_store_get(SIG_OBD_SPEED, &sample) && sample == 60);
    obd_data_set_speed(0); CHECK(signal_store_get(SIG_OBD_SPEED, &sample) && sample == 0);
    obd_data_set_oil_temp(255); CHECK(signal_store_get(SIG_OBD_OIL_TEMP, &sample) && sample == 255);
    obd_data_set_coolant_temp(255); CHECK(signal_store_get(SIG_OBD_COOLANT_TEMP, &sample) && sample == 255);
    obd_data_set_boost_x10(-7); CHECK(signal_store_get(SIG_OBD_BOOST, &sample) && sample == -70);
    CHECK(signal_store_set(SIG_OBD_SPEED, 123));
    CHECK(signal_store_set(SIG_OBD_GEAR, 5));
    CHECK(signal_store_set(SIG_OBD_COOLANT_TEMP, -10));
    CHECK(signal_store_set(SIG_OBD_OIL_PRESSURE, 157));
    CHECK(signal_store_set(SIG_OBD_BATTERY_VOLTAGE, 12345));
    obd_motion_snapshot_t motion;
    unsigned calls_before_motion = s_timer_calls;
    obd_data_get_motion_snapshot(&motion);
    CHECK(s_timer_calls - calls_before_motion == 1); /* one coherent timestamp */
    CHECK(motion.rpm == 3500 && motion.speed == 123 && motion.gear == 5);
    obd_data_rpm_override_set(true, 7200);
    obd_data_get_motion_snapshot(&motion);
    CHECK(motion.rpm == 7200 && motion.speed == 123 && motion.gear == 5);
    obd_data_rpm_override_set(false, 0);
    signal_store_invalidate(SIG_OBD_SPEED);
    signal_store_invalidate(SIG_OBD_GEAR);
    obd_data_get_motion_snapshot(&motion);
    CHECK(motion.rpm == 3500 && motion.speed == 0 && motion.gear == OBD_GEAR_UNKNOWN);
    CHECK(signal_store_set(SIG_OBD_SPEED, 123));
    CHECK(signal_store_set(SIG_OBD_GEAR, 5));
    signal_store_invalidate(SIG_OBD_RPM);
    CHECK(obd_data_get_rpm() == 0); /* invalid RPM maps to snapshot-compatible zero */
    CHECK(signal_store_set(SIG_OBD_RPM, 3500));

    const int slots[] = {
        SIG_OBD_RPM, SIG_OBD_SPEED, SIG_OBD_COOLANT_TEMP,
        SIG_OBD_OIL_PRESSURE, SIG_SYS_OBD_LINK, -1, SIG_SLOT_COUNT
    };
    int32_t values[sizeof(slots) / sizeof(slots[0])];
    unsigned calls_before = s_timer_calls;
    uint64_t valid = signal_store_get_many(slots, 7, values);

    CHECK(s_timer_calls - calls_before == 1); /* one shared freshness timestamp */
    CHECK(valid == 0x1f);
    CHECK(values[0] == 3500 && values[1] == 123 && values[2] == -10);
    CHECK(values[3] == 157 && values[4] == SIG_LINK_DATA);
    CHECK(values[5] == 0 && values[6] == 0);

    double display_values[SIG_SLOT_COUNT] = {0};
    uint64_t display_mask = (UINT64_C(1) << SIG_OBD_RPM) |
                            (UINT64_C(1) << SIG_OBD_OIL_PRESSURE) |
                            (UINT64_C(1) << SIG_OBD_BATTERY_VOLTAGE);
    calls_before = s_timer_calls;
    valid = signal_store_get_delivered_mask(display_mask, display_values);
    CHECK(s_timer_calls - calls_before == 1);
    CHECK(valid == display_mask);
    CHECK(display_values[SIG_OBD_RPM] == 3500.0);
    CHECK(display_values[SIG_OBD_OIL_PRESSURE] == 23.0); /* 157 kPa -> 22.77 psi, half-up */
    CHECK(display_values[SIG_OBD_BATTERY_VOLTAGE] == 123.0); /* mV -> 0.1 V, floor */

    /* OBD values expire together, but sys.obd_link has no staleness timeout. */
    s_now_us += 16000000;
    valid = signal_store_get_many(slots, 7, values);
    CHECK(valid == (1u << 4));
    CHECK(values[0] == 0 && values[4] == SIG_LINK_DATA);

    calls_before = s_timer_calls;
    valid = signal_store_get_delivered_mask(display_mask, display_values);
    CHECK(s_timer_calls - calls_before == 1);
    CHECK(valid == 0);
    CHECK(isnan(display_values[SIG_OBD_RPM]));
    CHECK(isnan(display_values[SIG_OBD_OIL_PRESSURE]));
    CHECK(isnan(display_values[SIG_OBD_BATTERY_VOLTAGE]));

    /* Invalid call parameters fail without reading the clock. */
    calls_before = s_timer_calls;
    CHECK(signal_store_get_many(slots, 65, values) == 0);
    CHECK(signal_store_get_many(NULL, 1, values) == 0);
    CHECK(s_timer_calls == calls_before);

    const int write_slots[] = { SIG_OBD_RPM, SIG_OBD_SPEED, SIG_OBD_OIL_PRESSURE };
    const int32_t write_values[] = { 4200, 999999, 200 };
    signal_store_set_local(SIG_OBD_OIL_PRESSURE, true);
    calls_before = s_timer_calls;
    signal_store_set_many(write_slots, write_values, 3);
    CHECK(s_timer_calls - calls_before == 1);
    signal_entry_t e;
    signal_store_entry(SIG_OBD_RPM, &e);
    CHECK(e.valid && e.value == 4200);
    signal_store_entry(SIG_OBD_SPEED, &e);
    CHECK(!e.valid && e.value == 999999); /* batch writes preserve range semantics */
    signal_store_entry(SIG_OBD_OIL_PRESSURE, &e);
    CHECK(e.valid && e.value == 157); /* local slots are left untouched */
    signal_store_set_local(SIG_OBD_OIL_PRESSURE, false);

    const int inval_slots[] = { SIG_OBD_RPM, SIG_OBD_SPEED, SIG_OBD_OIL_PRESSURE };
    signal_store_entry(SIG_OBD_RPM, &e);
    uint16_t rpm_seq = e.seq;
    signal_store_entry(SIG_OBD_SPEED, &e);
    uint16_t speed_seq = e.seq;
    signal_store_set_local(SIG_OBD_OIL_PRESSURE, true);
    signal_store_invalidate_many(inval_slots, 3);
    signal_store_entry(SIG_OBD_RPM, &e);
    CHECK(!e.valid && e.seq == (uint16_t)(rpm_seq + 1));
    signal_store_entry(SIG_OBD_SPEED, &e);
    CHECK(!e.valid && e.seq == speed_seq); /* already invalid: no sequence churn */
    signal_store_entry(SIG_OBD_OIL_PRESSURE, &e);
    CHECK(e.valid && e.value == 157); /* local values survive remote invalidation */
    signal_store_set_local(SIG_OBD_OIL_PRESSURE, false);

    /* Vehicle-wide invalidation only changes valid entries and preserves local values. */
    signal_store_set(SIG_OBD_RPM, 3500);
    signal_store_set(SIG_OBD_OIL_PRESSURE, 180);
    signal_store_set_local(SIG_OBD_OIL_PRESSURE, true);
    signal_store_entry(SIG_OBD_RPM, &e);
    rpm_seq = e.seq;
    signal_store_invalidate_vehicle();
    signal_store_entry(SIG_OBD_RPM, &e);
    CHECK(!e.valid && e.seq == (uint16_t)(rpm_seq + 1));
    signal_store_entry(SIG_OBD_OIL_PRESSURE, &e);
    CHECK(e.valid && e.value == 180);
    signal_store_entry(SIG_OBD_RPM, &e);
    rpm_seq = e.seq;
    signal_store_invalidate_vehicle();
    signal_store_entry(SIG_OBD_RPM, &e);
    CHECK(!e.valid && e.seq == rpm_seq); /* repeated invalidation is a no-op */
    signal_store_set_local(SIG_OBD_OIL_PRESSURE, false);

    /* Single-value invalidation is also idempotent, avoiding redundant work. */
    signal_store_set(SIG_OBD_RPM, 2500);
    signal_store_entry(SIG_OBD_RPM, &e);
    rpm_seq = e.seq;
    signal_store_invalidate(SIG_OBD_RPM);
    signal_store_entry(SIG_OBD_RPM, &e);
    CHECK(!e.valid && e.seq == (uint16_t)(rpm_seq + 1));
    rpm_seq = e.seq;
    signal_store_invalidate(SIG_OBD_RPM);
    signal_store_entry(SIG_OBD_RPM, &e);
    CHECK(!e.valid && e.seq == rpm_seq);

    /* Accessory fields do not depend on ELM and cannot be overwritten by it. */
    signal_store_set(SIG_SYS_OBD_LINK, SIG_LINK_NONE);
    const int32_t hub_values[3]={90,110,250};
    signal_store_hub_apply(4,hub_values);
    CHECK(signal_store_hub_obd_mask()==(1u<<SIG_OBD_OIL_PRESSURE));
    int32_t pressure;
    CHECK(signal_store_get(SIG_OBD_OIL_PRESSURE,&pressure) && pressure==250);
    CHECK(!signal_store_set(SIG_OBD_OIL_PRESSURE,999));
    signal_store_invalidate(SIG_OBD_OIL_PRESSURE);
    signal_store_invalidate_vehicle();
    const int hub_slots[3]={SIG_OBD_COOLANT_TEMP,SIG_OBD_OIL_TEMP,SIG_OBD_OIL_PRESSURE};
    const int32_t obd_values[3]={80,95,800};
    signal_store_set_many(hub_slots,obd_values,3);
    signal_store_invalidate_many(hub_slots,3);
    CHECK(signal_store_get(SIG_OBD_OIL_PRESSURE,&pressure) && pressure==250);
    CHECK(!signal_store_get(SIG_OBD_COOLANT_TEMP,&pressure));
    CHECK(signal_store_get(SIG_SYS_OBD_LINK,&pressure) && pressure==SIG_LINK_NONE);
    /* Unprovided fields release ownership immediately, no made-up zeros. */
    signal_store_hub_apply(1,hub_values);
    CHECK(!signal_store_get(SIG_OBD_OIL_PRESSURE,&pressure));
    CHECK(signal_store_get(SIG_OBD_COOLANT_TEMP,&pressure) && pressure==90);
    signal_store_hub_apply(0,NULL);
    CHECK(signal_store_hub_obd_mask()==0);
    signal_store_set(SIG_SYS_OBD_LINK,SIG_LINK_DATA);
    CHECK(signal_store_set(SIG_OBD_OIL_PRESSURE,800));
    CHECK(signal_store_get(SIG_OBD_OIL_PRESSURE,&pressure) && pressure==800);
    /* Hub vacuum is authoritative; legacy OBD cannot overwrite or invalidate it. */
    const int32_t signed_pressure[] = {-100, -99, -10, -5, -1, 0, 1000, 2000};
    for (unsigned i=0; i<sizeof(signed_pressure)/sizeof(signed_pressure[0]); ++i) {
        const int32_t sample[3]={0,0,signed_pressure[i]};
        signal_store_hub_apply(4,sample);
        CHECK(signal_store_get(SIG_OBD_OIL_PRESSURE,&pressure) && pressure==sample[2]);
        CHECK(signal_store_hub_obd_mask()==(1u<<SIG_OBD_OIL_PRESSURE));
        CHECK(!signal_store_set(SIG_OBD_OIL_PRESSURE,800));
        signal_store_invalidate(SIG_OBD_OIL_PRESSURE);
        int32_t relay[3];
        CHECK(signal_store_hub_snapshot(relay)==4 && relay[2]==sample[2]);
    }
    signal_store_hub_apply(0,NULL);
    CHECK(!signal_store_get(SIG_OBD_OIL_PRESSURE,&pressure));
    CHECK(!signal_store_set(SIG_OBD_OIL_PRESSURE,-1)); /* ordinary OBD floor unchanged */
    CHECK(signal_store_set(SIG_OBD_OIL_PRESSURE,800));
    /* Out-of-range packets remain invalid, never a bogus 0 bar reading. */
    const int32_t bad_values[3]={-20,300,-101};
    signal_store_hub_apply(7,bad_values);
    CHECK(signal_store_hub_obd_mask()==(1u<<SIG_OBD_COOLANT_TEMP));
    CHECK(signal_store_get(SIG_OBD_COOLANT_TEMP,&pressure) && pressure==-20);
    const int32_t too_high[3]={0,0,2001};
    signal_store_hub_apply(4,too_high);
    CHECK(!signal_store_hub_obd_mask());
    /* A board-local ADC remains local after the Hub releases the same slot. */
    signal_store_set_local(SIG_OBD_OIL_PRESSURE,true);
    signal_store_hub_apply(4,hub_values);
    signal_store_hub_apply(0,NULL);
    CHECK(signal_store_is_local(SIG_OBD_OIL_PRESSURE));
    signal_store_set_local(SIG_OBD_OIL_PRESSURE,false);
    puts("signal_store batch operations + Sky Hub arbitration: OK");
    return 0;
}
