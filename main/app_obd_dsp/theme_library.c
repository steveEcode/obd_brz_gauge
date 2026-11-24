// theme_library.c — SD theme library (/OBD/THEMES/<key>/theme.bin) and the
// persistent selection of a validated SD package.
// See theme_library.h for the write sequence and the UI contract.
#include "app_obd_dsp/theme_library.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp_obd_dsp/nvs_app.h"
#include "bsp_obd_dsp/sd_card.h"
#include "app_obd_dsp/ota_wifi_server.h"
#include "nvs.h"


static const char *TAG = "theme_lib";

#define NS_IMP        "imp"
#define KEY_WRITING   "th_wr"     /* legacy interrupted Flash copy */
#define KEY_MAN_CRC   "th_mc"     /* legacy manifest CRC */
#define KEY_CURRENT   "th_cur"    /* str: selected SD theme key */
#define TASK_STACK    4096
#define TASK_PRIO     (tskIDLE_PRIORITY + 2)
#define PATH_LEN      128

static portMUX_TYPE               s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile theme_lib_state_t s_state = THEME_LIB_IDLE;
static volatile uint8_t           s_pct;
static char                       s_err[64];
static char                       s_job_key[STORAGE_THEME_KEY_LEN];

/* ------------------------------------------------------------------ */

static void set_state(theme_lib_state_t st, uint8_t pct)
{
    taskENTER_CRITICAL(&s_mux);
    s_state = st;
    s_pct = pct;
    taskEXIT_CRITICAL(&s_mux);
}

theme_lib_state_t theme_library_get_state(uint8_t *pct)
{
    taskENTER_CRITICAL(&s_mux);
    theme_lib_state_t st = s_state;
    uint8_t p = s_pct;
    taskEXIT_CRITICAL(&s_mux);
    if (pct) *pct = p;
    return st;
}

const char *theme_library_last_error(void)
{
    return s_err;
}

void theme_library_ack(void)
{
    taskENTER_CRITICAL(&s_mux);
    if (s_state == THEME_LIB_DONE || s_state == THEME_LIB_ERROR) {
        s_state = THEME_LIB_IDLE;
        s_pct = 0;
    }
    taskEXIT_CRITICAL(&s_mux);
}

void theme_library_current_key(char *out, size_t out_len)
{
    if (!out || !out_len) return;
    if (nvs_app_get_str(NS_IMP, KEY_CURRENT, out, out_len) != ESP_OK) out[0] = '\0';
}

bool theme_library_resume_pending(void)
{
    char key[STORAGE_THEME_KEY_LEN];
    return nvs_app_get_str(NS_IMP, KEY_WRITING, key, sizeof(key)) == ESP_OK && key[0];
}

static bool key_ok(const char *key)
{
    if (!key || !key[0] || strlen(key) >= STORAGE_THEME_KEY_LEN) return false;
    if (strchr(key, '/') || strchr(key, '\\')) return false;
    return strcmp(key, ".") != 0 && strcmp(key, "..") != 0;
}

static void theme_path(const char *key, char *out, size_t len)
{
    snprintf(out, len, SD_DIR_THEMES "/%s/" STORAGE_THEME_FILE, key);
}

static bool remove_tree(const char *path)
{
    /* One path buffer and one DIR regardless of SD directory depth. Reopen
     * the parent after removing a child instead of retaining recursive frames
     * and FAT handles on the HTTP/USB caller's small internal task stack. */
    char current[384];
    int n = snprintf(current, sizeof(current), "%s", path);
    if (n < 0 || (size_t)n >= sizeof(current)) return false;
    const size_t root_len = (size_t)n;
    for (;;) {
        DIR *dir = opendir(current);
        if (!dir) return strlen(current) == root_len && errno == ENOENT;
        size_t parent_len = strlen(current);
        bool descend = false, ok = true;
        for (;;) {
            errno = 0;
            struct dirent *entry = readdir(dir);
            if (!entry) { ok = errno == 0; break; }
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            n = snprintf(current + parent_len, sizeof(current) - parent_len,
                         "/%s", entry->d_name);
            if (n < 0 || (size_t)n >= sizeof(current) - parent_len) { ok = false; break; }
            struct stat st;
            if (stat(current, &st) != 0) { ok = false; break; }
            if (S_ISDIR(st.st_mode)) { descend = true; break; }
            if (unlink(current) != 0) { ok = false; break; }
            current[parent_len] = '\0';
        }
        if (closedir(dir) != 0) ok = false;
        if (!ok) return false;
        if (descend) continue;
        if (rmdir(current) != 0) return false;
        if (parent_len == root_len) return true;
        char *slash = strrchr(current, '/');
        if (!slash || (size_t)(slash - current) < root_len) return false;
        *slash = '\0';
    }
}

esp_err_t theme_library_delete_current(bool *deleted)
{
    if (deleted) *deleted = false;
    if (!sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    if (!storage_job_lock(pdMS_TO_TICKS(1000))) return ESP_ERR_TIMEOUT;

    char key[STORAGE_THEME_KEY_LEN] = {0};
    esp_err_t key_err = nvs_app_get_str(NS_IMP, KEY_CURRENT, key, sizeof(key));
    if (key_err != ESP_OK && key_err != ESP_ERR_NVS_NOT_FOUND) {
        storage_job_unlock();
        return key_err;
    }
    esp_err_t err = ESP_OK;
    if (key[0]) {
        char dir[PATH_LEN];
        int n = snprintf(dir, sizeof(dir), SD_DIR_THEMES "/%s", key);
        if (n < 0 || (size_t)n >= sizeof(dir) || !key_ok(key)) {
            err = ESP_ERR_INVALID_RESPONSE;
        } else {
            struct stat st;
            int stat_err = stat(dir, &st);
            bool present = stat_err == 0;
            if (stat_err != 0 && errno != ENOENT) {
                err = ESP_FAIL;
            } else if (present && !S_ISDIR(st.st_mode)) {
                err = ESP_FAIL;
            } else if (present && !remove_tree(dir)) {
                err = ESP_FAIL;
            } else if (deleted) {
                *deleted = present;
            }
        }
    }
    if (err == ESP_OK) err = nvs_app_erase_key(NS_IMP, KEY_CURRENT);
    storage_job_unlock();
    return err;
}

/* Validate the SD package and commit its key. NVS commits atomically; the
 * previous selection survives any failure before that commit. */
esp_err_t theme_library_select(const char *key)
{
    if (!key_ok(key)) return ESP_ERR_INVALID_ARG;
    if (!sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    if (!storage_job_lock(pdMS_TO_TICKS(1000))) return ESP_ERR_TIMEOUT;
    char path[PATH_LEN];
    theme_path(key, path, sizeof(path));
    storage_theme_meta_t meta;
    esp_err_t err = storage_theme_meta_from_file(path, &meta);
    if (err == ESP_OK) err = nvs_app_set_str(NS_IMP, KEY_CURRENT, key);
    storage_job_unlock();
    return err;
}

static void select_task(void *arg)
{
    (void)arg;
    esp_err_t err = theme_library_select(s_job_key);
    if (err == ESP_OK) {
        s_err[0] = 0;
        set_state(THEME_LIB_DONE, 100);
    } else {
        snprintf(s_err, sizeof(s_err), "select failed: %s", esp_err_to_name(err));
        set_state(THEME_LIB_ERROR, 0);
    }
    vTaskDelete(NULL);
}

esp_err_t theme_library_activate(const char *key)
{
    if (!key_ok(key)) return ESP_ERR_INVALID_ARG;
    if (ota_wifi_server_is_active() || !sd_card_is_mounted() || storage_job_busy())
        return ESP_ERR_INVALID_STATE;
    taskENTER_CRITICAL(&s_mux);
    bool busy = s_state != THEME_LIB_IDLE;
    if (!busy) { s_state = THEME_LIB_WRITING; s_pct = 0; }
    taskEXIT_CRITICAL(&s_mux);
    if (busy) return ESP_ERR_INVALID_STATE;
    strlcpy(s_job_key, key, sizeof(s_job_key));
    s_err[0] = 0;
    if (xTaskCreate(select_task, "theme_lib", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS) {
        set_state(THEME_LIB_IDLE, 0);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t theme_library_resume(void)
{
    char key[STORAGE_THEME_KEY_LEN];
    if (nvs_app_get_str(NS_IMP, KEY_WRITING, key, sizeof(key)) != ESP_OK || !key[0]) return ESP_OK;
    if (ota_wifi_server_is_active()) return ESP_ERR_INVALID_STATE;
    /* Convert an old interrupted Flash activation into SD selection. Keep
     * the marker while the card is absent so insertion can finish it. */
    if (!sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    esp_err_t err = theme_library_select(key);
    if (err == ESP_OK || err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_ARG ||
        err == ESP_ERR_INVALID_SIZE || err == ESP_ERR_INVALID_RESPONSE || err == ESP_ERR_NOT_SUPPORTED) {
        nvs_app_erase_key(NS_IMP, KEY_MAN_CRC);
        nvs_app_erase_key(NS_IMP, KEY_WRITING);
    }
    if (err == ESP_OK) set_state(THEME_LIB_DONE, 100);
    return err;
}

/* ------------------------------------------------------------------ */
/* Import a dropped theme.bin into the library                         */
/* ------------------------------------------------------------------ */

esp_err_t theme_library_import_file(const char *path, char *key_out, size_t key_len)
{
    if (!path) return ESP_ERR_INVALID_ARG;
    if (key_out && key_len) key_out[0] = '\0';
    if (!sd_card_is_mounted()) return ESP_ERR_INVALID_STATE;
    if (!storage_job_lock(pdMS_TO_TICKS(1000))) return ESP_ERR_TIMEOUT;
    storage_theme_meta_t meta;
    esp_err_t err = storage_theme_meta_from_file(path, &meta);
    if (err != ESP_OK) { storage_job_unlock(); return err; }

    char key[STORAGE_THEME_KEY_LEN];
    storage_theme_key(meta.id, key, sizeof(key));
    char dst[PATH_LEN];
    theme_path(key, dst, sizeof(dst));

    char current[STORAGE_THEME_KEY_LEN];
    theme_library_current_key(current, sizeof(current));
    if (!ota_wifi_server_is_active() && strcmp(current, key) == 0) {
        storage_job_unlock();
        return ESP_ERR_INVALID_STATE;  // choose another theme before replacing this package
    }
    if (theme_library_get_state(NULL) == THEME_LIB_WRITING) {
        storage_job_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    storage_sd_mkdir_parents(dst);
    err = sd_card_rename(path, dst);
    storage_job_unlock();
    if (err != ESP_OK) return err;
    if (key_out && key_len) strlcpy(key_out, key, key_len);
    ESP_LOGI(TAG, "imported %s -> %s", path, dst);
    return ESP_OK;
}
