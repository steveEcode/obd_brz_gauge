#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_obd_dsp/signal_store.h"
#include "app_obd_dsp/obd_data_cache.h"
#include "freertos/task.h"
#include "export_path/ui_data_plan.h"

static int64_t s_now_us;
static unsigned s_timer_calls;

void *heap_caps_calloc(size_t n, size_t size, unsigned caps)
{
    (void)caps;
    return calloc(n, size);
}
int64_t esp_timer_get_time(void) { s_timer_calls++; return s_now_us; }
TickType_t xTaskGetTickCount(void) { return (TickType_t)(s_now_us / 1000); }

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        return 1; \
    } \
} while (0)

static int test_ui_plan(void)
{
    static const int expected[DISP_ITEM_COUNT] = {
        SIG_OBD_COOLANT_TEMP, SIG_OBD_INTAKE_TEMP, SIG_OBD_OIL_TEMP,
        SIG_OBD_LOAD, SIG_OBD_THROTTLE, SIG_OBD_RPM, SIG_OBD_SPEED,
        SIG_OBD_BATTERY_VOLTAGE, SIG_OBD_OIL_PRESSURE, -1,
        SIG_OBD_BOOST, SIG_OBD_AFR
    };
    for (unsigned i = 0; i <= UINT8_MAX; i++) {
        const uint16_t wanted = i < DISP_ITEM_COUNT && expected[i] >= 0 ?
                                (uint16_t)(1u << expected[i]) : 0;
        CHECK(ui_data_mask_for_item((uint8_t)i) == wanted);
        uint8_t item = (uint8_t)i;
        CHECK(ui_data_mask_for_map(&item, 1) == ui_data_mask_for_item(i % DISP_ITEM_COUNT));
    }
    const uint8_t repeated[] = {DISP_ITEM_OILP, DISP_ITEM_OILP, DISP_ITEM_RESERVED_9};
    CHECK(ui_data_mask_for_map(repeated, sizeof(repeated)) == (1u << SIG_OBD_OIL_PRESSURE));
    const uint8_t temps[] = {DISP_ITEM_CLT, DISP_ITEM_IAT, DISP_ITEM_OIL};
    CHECK(ui_data_mask_for_map(temps, sizeof(temps)) ==
          ((1u << SIG_OBD_COOLANT_TEMP) | (1u << SIG_OBD_INTAKE_TEMP) | (1u << SIG_OBD_OIL_TEMP)));
    CHECK(ui_data_mask_for_map(NULL, 0) == 0);
    return 0;
}

static int seed(void)
{
    static const int32_t values[SIG_OBD_COUNT] = {
        [SIG_OBD_RPM] = 3500, [SIG_OBD_SPEED] = 321, [SIG_OBD_BOOST] = -115,
        [SIG_OBD_COOLANT_TEMP] = -10, [SIG_OBD_OIL_PRESSURE] = 155,
        [SIG_OBD_GEAR] = -1, [SIG_OBD_BATTERY_VOLTAGE] = 12345,
        [SIG_OBD_OIL_TEMP] = 97, [SIG_OBD_AFR] = 1470,
        [SIG_OBD_THROTTLE] = 31, [SIG_OBD_INTAKE_TEMP] = 24, [SIG_OBD_LOAD] = 61
    };
    s_now_us = 1000000;
    CHECK(signal_store_set(SIG_SYS_OBD_LINK, SIG_LINK_DATA));
    for (int i = 0; i < SIG_OBD_COUNT; i++) CHECK(signal_store_set(i, values[i]));
    obd_data_rpm_override_set(false, 0);
    return 0;
}

static int check_masks(void)
{
    const obd_data_snapshot_t empty = {
        .coolant_temp = -40, .oil_temp = -100, .intake_temp = -40,
        .load_pct = -1, .tps = -1, .bat_mv = -1, .oil_pressure_x10 = OBD_OIL_PRESSURE_INVALID,
        .boost_x10 = -32768, .gear = OBD_GEAR_UNKNOWN, .afr_x100 = -1,
        .oil_pressure_x100 = OBD_OIL_PRESSURE_INVALID
    };
    obd_data_snapshot_t full;
    obd_data_get_snapshot(&full);
    for (unsigned mask = 0; mask < (1u << SIG_OBD_COUNT); mask++) {
        obd_data_snapshot_t actual;
        memset(&actual, 0xA5, sizeof(actual));
        unsigned before = s_timer_calls;
        // Ignored sys/reserved bits must not change the OBD selection.
        obd_data_get_snapshot_mask(&actual, (uint16_t)(mask | 0xF000u));
        CHECK(s_timer_calls - before == (mask ? 1u : 0u));
#define FIELD(slot, member) CHECK(actual.member == ((mask & (1u << slot)) ? full.member : empty.member))
        FIELD(SIG_OBD_RPM, rpm);
        FIELD(SIG_OBD_SPEED, speed);
        FIELD(SIG_OBD_COOLANT_TEMP, coolant_temp);
        FIELD(SIG_OBD_OIL_TEMP, oil_temp);
        FIELD(SIG_OBD_INTAKE_TEMP, intake_temp);
        FIELD(SIG_OBD_LOAD, load_pct);
        FIELD(SIG_OBD_THROTTLE, tps);
        FIELD(SIG_OBD_BATTERY_VOLTAGE, bat_mv);
        FIELD(SIG_OBD_OIL_PRESSURE, oil_pressure_x10);
        FIELD(SIG_OBD_OIL_PRESSURE, oil_pressure_x100);
        FIELD(SIG_OBD_BOOST, boost_x10);
        FIELD(SIG_OBD_GEAR, gear);
        FIELD(SIG_OBD_AFR, afr_x100);
#undef FIELD
    }
    unsigned before = s_timer_calls;
    obd_data_get_snapshot_mask(NULL, UINT16_MAX);
    CHECK(s_timer_calls == before);
    return 0;
}

static int test_masks(void)
{
    CHECK(seed() == 0);
    obd_data_snapshot_t initial;
    obd_data_get_snapshot(&initial);
    CHECK(initial.rpm == 3500 && initial.speed == UINT8_MAX && initial.gear == -1);
    CHECK(initial.coolant_temp == -10 && initial.oil_temp == 97 && initial.intake_temp == 24);
    CHECK(initial.load_pct == 61 && initial.tps == 31 && initial.bat_mv == 12345);
    CHECK(initial.oil_pressure_x100 == 155);
    CHECK(initial.oil_pressure_x10 == 16 && initial.boost_x10 == -12 && initial.afr_x100 == 1470);
    CHECK(check_masks() == 0);
    obd_data_rpm_override_set(true, 7200);
    CHECK(check_masks() == 0);
    signal_store_invalidate(SIG_OBD_RPM);
    signal_store_invalidate(SIG_OBD_GEAR);
    signal_store_invalidate(SIG_OBD_COOLANT_TEMP);
    CHECK(check_masks() == 0);
    obd_data_rpm_override_set(false, 0);
    CHECK(check_masks() == 0);
    CHECK(seed() == 0);
    CHECK(signal_store_set(SIG_SYS_OBD_LINK, SIG_LINK_NO_DATA));
    CHECK(check_masks() == 0);
    // Local sensor exemption must match full snapshots when the link is down.
    signal_store_set_local(SIG_OBD_OIL_PRESSURE, true);
    CHECK(check_masks() == 0);
    signal_store_set_local(SIG_OBD_OIL_PRESSURE, false);
    CHECK(seed() == 0);
    s_now_us += 5000000; // exact stale boundary: motion expired, temperatures fresh
    CHECK(check_masks() == 0);
    s_now_us += 10000000; // exact temperature stale boundary
    CHECK(check_masks() == 0);

    // Both signs around the pressure half-up rounding boundary.
    for (int32_t v = -120; v <= 120; v++) {
        s_now_us = 1000000;
        CHECK(signal_store_set(SIG_SYS_OBD_LINK, SIG_LINK_DATA));
        CHECK(signal_store_set(SIG_OBD_BOOST, v));
        CHECK(signal_store_set(SIG_OBD_OIL_PRESSURE, v < 0 ? -v : v));
        obd_data_snapshot_t full, masked;
        obd_data_get_snapshot(&full);
        obd_data_get_snapshot_mask(&masked,
            (1u << SIG_OBD_BOOST) | (1u << SIG_OBD_OIL_PRESSURE));
        CHECK(masked.boost_x10 == full.boost_x10);
        CHECK(masked.oil_pressure_x10 == full.oil_pressure_x10);
    }
    const int32_t hub_pressure[] = {-100, -99, -96, -15, -10, -5, -1, 0, 1, 5, 96, 155, 1000, 1001, 2000};
    for (unsigned i=0; i<sizeof(hub_pressure)/sizeof(hub_pressure[0]); ++i) {
        CHECK(signal_store_set(SIG_SYS_OBD_LINK, SIG_LINK_NONE));
        int32_t sample[3]={0,0,hub_pressure[i]};
        signal_store_hub_apply(4,sample);
        obd_data_snapshot_t full, masked;
        obd_data_get_snapshot(&full);
        obd_data_get_snapshot_mask(&masked,1u<<SIG_OBD_OIL_PRESSURE);
        int32_t p=hub_pressure[i];
        int16_t expected=(int16_t)(p>=0 ? (p+5)/10 : -((-p+5)/10));
        CHECK(full.oil_pressure_x100==p && masked.oil_pressure_x100==p);
        CHECK(full.oil_pressure_x10==expected && masked.oil_pressure_x10==expected);
        CHECK(masked.oil_pressure_x10!=OBD_OIL_PRESSURE_INVALID);
        CHECK(check_masks()==0);
        obd_data_set_oil_pressure_x10(-1); /* offline local ADC cannot invalidate Hub */
        obd_data_get_snapshot(&full);
        CHECK(full.oil_pressure_x10==expected && full.oil_pressure_x100==p);
    }
    signal_store_hub_apply(0,NULL);
    obd_data_snapshot_t missing;
    obd_data_get_snapshot(&missing);
    CHECK(missing.oil_pressure_x10==OBD_OIL_PRESSURE_INVALID);
    CHECK(missing.oil_pressure_x100==OBD_OIL_PRESSURE_INVALID);
    CHECK(check_masks()==0);
    puts("snapshot masks: all 4096 masks x 8 validity/override scenarios OK");
    return 0;
}

static uint64_t clock_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) abort();
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static int64_t checksum(const obd_data_snapshot_t *s, uint16_t mask)
{
    int64_t sum = 0;
#define ADD(slot, member) if (mask & (1u << slot)) sum += s->member
    ADD(SIG_OBD_RPM, rpm); ADD(SIG_OBD_SPEED, speed); ADD(SIG_OBD_GEAR, gear);
    ADD(SIG_OBD_COOLANT_TEMP, coolant_temp); ADD(SIG_OBD_OIL_TEMP, oil_temp);
    ADD(SIG_OBD_INTAKE_TEMP, intake_temp); ADD(SIG_OBD_LOAD, load_pct);
    ADD(SIG_OBD_THROTTLE, tps); ADD(SIG_OBD_BATTERY_VOLTAGE, bat_mv);
    ADD(SIG_OBD_OIL_PRESSURE, oil_pressure_x10); ADD(SIG_OBD_BOOST, boost_x10);
    ADD(SIG_OBD_AFR, afr_x100);
#undef ADD
    return sum;
}

static uint64_t bench_run(uint16_t mask, int sparse, size_t count, int64_t *sum)
{
    uint64_t start = clock_ns();
    int64_t total = 0;
    for (size_t i = 0; i < count; i++) {
        obd_data_snapshot_t out;
        if (sparse) obd_data_get_snapshot_mask(&out, mask);
        else obd_data_get_snapshot(&out);
        total += checksum(&out, mask);
    }
    *sum = total;
    return clock_ns() - start;
}
static int compare_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static int benchmark(void)
{
    static const struct { const char *name; uint16_t mask; } cases[] = {
        {"chart_oilp", 1u << SIG_OBD_OIL_PRESSURE},
        {"chart_oilp_rpm", (1u << SIG_OBD_OIL_PRESSURE) | (1u << SIG_OBD_RPM)},
        {"temp", (1u << SIG_OBD_COOLANT_TEMP) | (1u << SIG_OBD_INTAKE_TEMP) | (1u << SIG_OBD_OIL_TEMP)},
        {"temp_rpm", (1u << SIG_OBD_COOLANT_TEMP) | (1u << SIG_OBD_INTAKE_TEMP) | (1u << SIG_OBD_OIL_TEMP) | (1u << SIG_OBD_RPM)},
        {"info", (1u << SIG_OBD_COOLANT_TEMP) | (1u << SIG_OBD_INTAKE_TEMP) | (1u << SIG_OBD_OIL_TEMP) | (1u << SIG_OBD_BATTERY_VOLTAGE) | (1u << SIG_OBD_LOAD)},
        {"info_rpm", (1u << SIG_OBD_COOLANT_TEMP) | (1u << SIG_OBD_INTAKE_TEMP) | (1u << SIG_OBD_OIL_TEMP) | (1u << SIG_OBD_BATTERY_VOLTAGE) | (1u << SIG_OBD_LOAD) | (1u << SIG_OBD_RPM)},
        {"all", (1u << SIG_OBD_COUNT) - 1u}
    };
    CHECK(seed() == 0);
    puts("scenario,requested_slots,full_ns,masked_ns,ratio");
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        uint64_t full[7], masked[7];
        int64_t a, b;
        bench_run(cases[c].mask, 0, 20000, &a);
        bench_run(cases[c].mask, 1, 20000, &b);
        CHECK(a == b);
        for (unsigned run = 0; run < 7; run++) {
            if (run & 1u) {
                masked[run] = bench_run(cases[c].mask, 1, 500000, &b);
                full[run] = bench_run(cases[c].mask, 0, 500000, &a);
            } else {
                full[run] = bench_run(cases[c].mask, 0, 500000, &a);
                masked[run] = bench_run(cases[c].mask, 1, 500000, &b);
            }
            CHECK(a == b);
        }
        qsort(full, 7, sizeof(full[0]), compare_u64);
        qsort(masked, 7, sizeof(masked[0]), compare_u64);
        printf("%s,%u,%.2f,%.2f,%.3f\n", cases[c].name,
               (unsigned)__builtin_popcount(cases[c].mask),
               full[3] / 500000.0, masked[3] / 500000.0, (double)full[3] / masked[3]);
    }
    return 0;
}

int main(int argc, char **argv)
{
    signal_store_init();
    if (test_ui_plan()) return 1;
    if (test_masks()) return 1;
    return argc == 2 && !strcmp(argv[1], "--bench") ? benchmark() : 0;
}
