#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * obd_data_cache - legacy facade over signal_store.
 *
 * Setters take the historical units and sentinels, convert to the registry's
 * internal units (contracts/signals.json) and write signal_store; a sentinel
 * invalidates the signal. The snapshot reads signal_store and maps invalid
 * signals back to the legacy sentinels. New code should use signal_store
 * directly.
 * -------------------------------------------------------------------------- */

/* Snapshot-only sentinel: -1 is a legitimate signed pressure sample.
 * The legacy OBD/local-ADC setter still treats negative input as invalid. */
#define OBD_OIL_PRESSURE_INVALID INT16_MIN

#define OBD_GEAR_UNKNOWN  127   /* snapshot.gear when obd.gear is invalid */

typedef struct {
    uint16_t rpm;
    uint8_t speed;
    int8_t gear;              /* -1 R, 0 N, 1..8, OBD_GEAR_UNKNOWN = invalid */
} obd_motion_snapshot_t;

typedef struct {
    uint16_t rpm;
    uint8_t  speed;
    int16_t  coolant_temp;      /* °C, -40 = invalid */
    int16_t  oil_temp;          /* °C, -100 = invalid */
    int16_t  intake_temp;       /* °C, -40 = invalid */
    int16_t  load_pct;          /* %, -1 = invalid */
    int16_t  tps;               /* %, -1 = invalid */
    int32_t  bat_mv;            /* mV, -1 = invalid */
    int16_t  oil_pressure_x10;  /* signed 0.1 bar, OBD_OIL_PRESSURE_INVALID = invalid */
    int16_t  boost_x10;         /* 0.1 bar gauge, -32768 = invalid */
    int8_t   gear;              /* -1 R, 0 N, 1..8, OBD_GEAR_UNKNOWN = invalid */
    int16_t  afr_x100;          /* AFR x100, -1 = invalid */
    int16_t  oil_pressure_x100; /* signed 0.01 bar (kPa), no rounding; same invalid sentinel */
} obd_data_snapshot_t;

void obd_data_set_rpm(uint16_t rpm);
// Read only RPM (including the multi-gauge override) without building a full snapshot.
uint16_t obd_data_get_rpm(void);
// RPM override layer: for multi-gauge linkage tests. While enabled the snapshot
// disabling restores the real value.
void obd_data_rpm_override_set(bool en, uint16_t val);
void obd_data_set_speed(uint8_t kmh);
void obd_data_set_coolant_temp(int16_t temp);
void obd_data_set_oil_temp(int16_t temp);          // °C; values outside [-20, 255] are ignored (hold)
void obd_data_set_intake_temp(int16_t temp);
void obd_data_set_load_pct(int16_t pct);           // 0~100 %, -1 = invalid
void obd_data_set_tps(int16_t pct);                // 0~100 %, -1 = invalid
void obd_data_set_oil_pressure_x10(int16_t pressure_x10); // 0.1 bar, <0 = invalid
void obd_data_set_boost_x10(int16_t boost_x10);    // 0.1 bar gauge, -32768 = invalid
void obd_data_set_afr_x100(int16_t afr_x100);      // AFR x100, -1 = invalid
// Compact coherent read for the primary RPM/speed/gear pages.
void obd_data_get_motion_snapshot(obd_motion_snapshot_t *out);
void     obd_data_get_snapshot(obd_data_snapshot_t *out);
// One coherent read of the requested SIG_OBD_* slot bits (1u << slot).
// Unrequested fields use legacy invalid sentinels; RPM override is applied only
// when RPM is requested. Non-OBD bits are ignored; an empty mask reads nothing.
void obd_data_get_snapshot_mask(obd_data_snapshot_t *out, uint16_t requested);

#ifdef __cplusplus
}
#endif
