#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* --------------------------------------------------------------------------
 * nvs_app — Application-private NVS partition (label "nvs_app", 0x11000, 60KB)
 *
 * The web flasher's complete-mode write path never touches this partition
 * (see docs/FLASH.md §3.2). It is the only storage area that survives a
 * web-flasher full reflash unchanged.
 *
 * Namespaces:
 *   sys   magic ('NAP2'), layout_crc, mig_ver, mig_veh (pending legacy vehicle)
 *   imp   legacy media CRC ledger, old write markers th_wr / th_mc for
 *         migration recovery, selected SD library theme th_cur
 *   veh   current vehicle profile id, prev id, installation state
 *   hub   Sky Hub bound WiFi STA MAC (device-local, not config-mirrored)
 *
 * Call nvs_app_init() once at boot, before nvs_storage_init() and before
 * any first-boot migration logic that may read mig_ver.
 * -------------------------------------------------------------------------- */

/** CRC32 of all 9 partitions.csv entries (layout v2). */
#define NVS_APP_LAYOUT_CRC   0x94752aa3u

/** mig_ver value written after the first-boot partition-table migration. */
#define NVS_APP_MIG_VER_DONE 1u

/** Maximum vehicle profile id string length (including NUL terminator). */
#define NVS_APP_VEH_ID_LEN   64u

/**
 * @brief  Initialise the nvs_app partition.
 *
 * Calls nvs_flash_init_partition().  If the partition is corrupt it is erased
 * and re-initialised.  If the stored magic does not match 'NAP2' the partition
 * is erased and a fresh magic is written; layout_crc is written only by the
 * first-boot migration (0 until then).
 *
 * Idempotent: safe to call again after a crash during migration.
 */
esp_err_t nvs_app_init(void);

/* ---- sys namespace ---- */

/** Current migration version (0 = not yet migrated). */
uint8_t   nvs_app_mig_ver_get(void);
esp_err_t nvs_app_mig_ver_set(uint8_t ver);

/* ---- imp namespace: inbox CRC ledger ---- */

/**
 * CRC32 of the theme_0 payload last successfully imported into SD / config.
 * 0 = no previous import.
 */
uint32_t  nvs_app_imp_theme_crc_get(void);
esp_err_t nvs_app_imp_theme_crc_set(uint32_t crc);

/**
 * CRC32 of the bootmedia payload last successfully imported.
 * 0 = no previous import.
 */
uint32_t  nvs_app_imp_bootmedia_crc_get(void);
esp_err_t nvs_app_imp_bootmedia_crc_set(uint32_t crc);

/* ---- veh namespace: current vehicle profile ---- */

/**
 * Vehicle profile installation state.
 * Stored as a uint8_t in key "state" of the veh namespace.
 * INSTALLED → PENDING → ACTIVE; failure path leads to FAILED.
 */
typedef enum {
    VEH_STATE_NONE      = 0, /* no profile beyond obd2.generic */
    VEH_STATE_INSTALLED = 1, /* profile stored in config partition */
    VEH_STATE_PENDING   = 2, /* installed, awaiting activation/validation */
    VEH_STATE_ACTIVE    = 3, /* running and validated */
    VEH_STATE_FAILED    = 4, /* validation failed; rolled back */
} veh_state_t;

/**
 * Get / set the currently active vehicle profile id string.
 * On set, the previous id is automatically copied to the "prev" key so
 * roll-back is possible.  Empty string means obd2.generic (built-in).
 */
esp_err_t nvs_app_veh_id_get(char *out, size_t out_len);
esp_err_t nvs_app_veh_id_set(const char *id);

/** Previously active profile id (the one to roll back to on FAILED). */
esp_err_t nvs_app_veh_prev_id_get(char *out, size_t out_len);

veh_state_t nvs_app_veh_state_get(void);
esp_err_t   nvs_app_veh_state_set(veh_state_t state);

/** Pending profile id (activated on the next boot); "" = none. */
esp_err_t nvs_app_veh_pending_id_get(char *out, size_t out_len);
esp_err_t nvs_app_veh_pending_id_set(const char *id);

/**
 * Compiled-or-source copy of the active profile (key "blob", <= 8KB) so the
 * vehicle survives a config partition wipe. *len is in/out (buffer size in,
 * stored size out). ESP_ERR_NVS_NOT_FOUND when absent.
 */
#define NVS_APP_VEH_BLOB_MAX (8u * 1024u)
esp_err_t nvs_app_veh_blob_get(void *buf, size_t *len);
esp_err_t nvs_app_veh_blob_set(const void *buf, size_t len);
esp_err_t nvs_app_veh_blob_erase(void);

/** Boots in a row where a PENDING profile failed verification. */
uint8_t   nvs_app_veh_boot_fail_get(void);
esp_err_t nvs_app_veh_boot_fail_set(uint8_t n);

/* ---- sys namespace: health ---- */

/** Partition layout CRC recorded by the migration (0 = never written). */
uint32_t  nvs_app_layout_crc_get(void);
esp_err_t nvs_app_layout_crc_set(uint32_t crc);

/** Consecutive abnormal resets (panic/WDT/brownout); drives safe mode. */
uint8_t   nvs_app_crash_count_get(void);
esp_err_t nvs_app_crash_count_set(uint8_t n);

/* ---- generic access (namespaces sys/imp/veh/stat/diag) ---- */

uint32_t  nvs_app_get_u32(const char *ns, const char *key, uint32_t def);
esp_err_t nvs_app_set_u32(const char *ns, const char *key, uint32_t val);
esp_err_t nvs_app_get_str(const char *ns, const char *key, char *out, size_t out_len);
esp_err_t nvs_app_set_str(const char *ns, const char *key, const char *val);
esp_err_t nvs_app_get_blob(const char *ns, const char *key, void *buf, size_t *len);
esp_err_t nvs_app_set_blob(const char *ns, const char *key, const void *buf, size_t len);
esp_err_t nvs_app_erase_key(const char *ns, const char *key);
