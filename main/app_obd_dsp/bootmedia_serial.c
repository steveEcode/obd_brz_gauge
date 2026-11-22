#include "bootmedia_serial.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app_obd_dsp/storage_util.h"
#include "app_obd_dsp/upload_sink.h"
#include "app_obd_dsp/theme_bundle.h"
#include "app_obd_dsp/ota_wifi_server.h"
#include "bsp_obd_dsp/sd_card.h"
#include "app_obd_dsp/device_serial_rpc.h"
#include "vehicle/vp_validate.h"
#include "media/boot_obv_validate.h"

static const char *TAG = "bootmedia_usb";

#define FRAME_HEADER_SIZE 16u
#define FRAME_DATA_MAX    4100u
#define SESSION_TIMEOUT   pdMS_TO_TICKS(30000)
#define SD_RESERVE_BYTES  (1024u * 1024u)

enum {
    CMD_BEGIN = 1,
    CMD_DATA = 2,
    CMD_END = 3,
    CMD_ABORT = 4,
    CMD_BEGIN_THEME = 5,
    CMD_BEGIN_RPC = 6,
    CMD_BEGIN_OBV = 7,
};

#define RPC_MAX_BYTES (VP_MAX_JSON_BYTES + 64u)

static upload_sink_t s_sink;
static bool s_theme_upload;
static bool s_rpc_session;
static bool s_obv_upload;
static bool s_storage_locked;
static TickType_t s_activity;
static char *s_rpc_reply;
static bool s_rpc_restart;
static const char *s_upload_error;

/* Hold the job lock for the whole transfer: the import scanner removes .part
 * files and must never unlink the receiver's open file (FATFS FF_FS_LOCK=0). */
static void release_storage(void)
{
    if (s_storage_locked) {
        s_storage_locked = false;
        storage_job_unlock();
    }
}

static void abort_session(void)
{
    upload_sink_abort(&s_sink);
    if (s_storage_locked && s_theme_upload) sd_card_remove(THEME_BUNDLE_PATH);
    s_theme_upload = false;
    s_rpc_session = false;
    s_obv_upload = false;
    release_storage();
}

static uint16_t get_u16le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_u16le(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void put_u32le(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static bool read_exact(uint8_t *dst, size_t len, TickType_t timeout, size_t *received)
{
    size_t got = 0;
    TickType_t started = xTaskGetTickCount();
    while (got < len) {
        TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= timeout) {
            if (received) *received = got;
            return false;
        }
        int n = usb_serial_jtag_read_bytes(dst + got, (uint32_t)(len - got), timeout - elapsed);
        if (n > 0) got += (size_t)n;
    }
    if (received) *received = got;
    return true;
}

static bool find_frame_header(uint8_t header[FRAME_HEADER_SIZE])
{
    static const uint8_t magic[4] = {'O', 'B', 'D', 'S'};
    uint8_t window[4] = {0};
    size_t filled = 0;
    for (;;) {
        uint8_t byte;
        int n = usb_serial_jtag_read_bytes(&byte, 1, pdMS_TO_TICKS(1000));
        if (n != 1) {
            if (upload_sink_is_open(&s_sink) && xTaskGetTickCount() - s_activity >= SESSION_TIMEOUT) {
                abort_session();
                return false;
            }
            continue;
        }
        if (filled < sizeof(window)) {
            window[filled++] = byte;
        } else {
            memmove(window, window + 1, sizeof(window) - 1);
            window[sizeof(window) - 1] = byte;
        }
        if (filled == sizeof(window) && memcmp(window, magic, sizeof(magic)) == 0) {
            memcpy(header, magic, sizeof(magic));
            size_t received = 0;
            if (!read_exact(header + 4, FRAME_HEADER_SIZE - 4, pdMS_TO_TICKS(3000), &received)) {
                ESP_LOGW(TAG, "partial frame header timeout: received=%u/%u", (unsigned)received,
                         FRAME_HEADER_SIZE - 4);
                return false;
            }
            return true;
        }
    }
}

static bool send_reply(uint8_t command, uint16_t sequence, uint8_t status, uint32_t received)
{
    uint8_t base[FRAME_HEADER_SIZE + 5] = {
        'O', 'B', 'D', 'S', 1, (uint8_t)(command | 0x80), 0, 0,
        5, 0, 0, 0, 0, 0, 0, 0, status, 0, 0, 0, 0
    };
    const char *body = command == CMD_END && status == 10 ? s_upload_error :
                       command == CMD_END && status == 0 ? s_rpc_reply : NULL;
    size_t extra = body ? strlen(body) : 0;
    size_t total = sizeof(base) + extra;
    uint8_t *reply = extra ? malloc(total) : base;
    if (!reply) return false;  /* Cache remains available for the client's retry. */
    if (extra) {
        memcpy(reply, base, sizeof(base));
        memcpy(reply + sizeof(base), body, extra);
    }
    put_u16le(reply + 6, sequence);
    put_u32le(reply + 8, (uint32_t)(5 + extra));
    put_u32le(reply + FRAME_HEADER_SIZE + 1, received);
    uint32_t crc = esp_rom_crc32_le(0, reply + FRAME_HEADER_SIZE, 5 + extra);
    put_u32le(reply + 12, crc);
    /* Keep header, status and JSON in one driver write so console logging cannot
     * be inserted between the prefix and its JSON body. */
    int written = usb_serial_jtag_write_bytes(reply, total, pdMS_TO_TICKS(3000));
    if (extra) free(reply);
    esp_err_t done = usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(1000));
    return written == (int)total && done == ESP_OK;
}

static uint8_t process_frame(uint8_t command, const uint8_t *payload, uint32_t length)
{
    if (command == CMD_BEGIN || command == CMD_BEGIN_THEME || command == CMD_BEGIN_RPC || command == CMD_BEGIN_OBV) {
        free(s_rpc_reply);
        s_rpc_reply = NULL;
        s_rpc_restart = false;
        s_upload_error = NULL;
    }
    switch (command) {
    case CMD_BEGIN:
    case CMD_BEGIN_THEME:
    case CMD_BEGIN_OBV: {
        if (length != 40 || upload_sink_is_open(&s_sink)) return 1;
        uint32_t total = get_u32le(payload);
        uint32_t manifest = get_u32le(payload + 4);
        bool theme = command == CMD_BEGIN_THEME;
        bool obv = command == CMD_BEGIN_OBV;
        if (ota_wifi_server_is_active()) return 9;
        if (!total || total > 256u * 1024u * 1024u ||
            (theme || obv ? manifest != 0 : (total <= manifest || manifest == 0 || manifest > 4096)) ||
            !sd_card_is_mounted() ||
            sd_card_get_free_bytes() < (uint64_t)total * (theme ? 2 : 1) + SD_RESERVE_BYTES) return 2;
        /* Allow short theme reads / boot storage jobs to finish before reporting BUSY. */
        if (!storage_job_lock(pdMS_TO_TICKS(1000))) return 9;
        s_storage_locked = true;
        s_theme_upload = theme;
        s_obv_upload = obv;
        esp_err_t err = upload_sink_open(&s_sink, UPLOAD_SINK_SD_FILE, total,
                                         payload + 8, theme ? THEME_BUNDLE_PATH : obv ? SD_BOOT_OBV : SD_BOOT_LEGACY, manifest);
        if (err != ESP_OK) {
            abort_session();
            return 3;
        }
        return 0;
    }
    case CMD_BEGIN_RPC: {
        if (length != 40 || upload_sink_is_open(&s_sink) || get_u32le(payload) == 0 ||
            get_u32le(payload) > RPC_MAX_BYTES || get_u32le(payload + 4) != 0) return 1;
        if (upload_sink_open(&s_sink, UPLOAD_SINK_MEMORY, get_u32le(payload), payload + 8, NULL, 0) != ESP_OK) {
            return 3;
        }
        s_rpc_session = true;
        return 0;
    }
    case CMD_DATA: {
        if (!upload_sink_is_open(&s_sink) || length < 5 || length > FRAME_DATA_MAX) return 1;
        uint32_t offset = get_u32le(payload);
        if (offset != s_sink.received || length - 4 > s_sink.expected - s_sink.received) return 4;
        if (!s_rpc_session && !s_storage_locked) return 9;
        if (upload_sink_write(&s_sink, payload + 4, length - 4) == ESP_OK) return 0;
        if (s_sink.failed) abort_session();
        return 5;
    }
    case CMD_END: {
        if (length != 0 || !upload_sink_is_open(&s_sink)) return 1;
        esp_err_t err = upload_sink_verify(&s_sink);
        if (err == ESP_OK && s_obv_upload) {
            char part[UPLOAD_PATH_MAX + 8];
            snprintf(part, sizeof(part), "%s.part", s_sink.path);
            /* FatFs publishes the directory size on sync; validation opens a second reader. */
            if (fflush(s_sink.fp) != 0 || fsync(fileno(s_sink.fp)) != 0) err = ESP_FAIL;
            else {
                const char *reason = NULL;
                if (!boot_obv_validate_file(part, &reason)) {
                    s_upload_error = reason ? reason : "invalid JPEG animation";
                    err = ESP_ERR_INVALID_ARG;
                }
            }
        }
        if (err == ESP_OK) err = upload_sink_commit(&s_sink);
        if (err != ESP_OK) {
            uint8_t status = s_upload_error ? 10 : 6;
            abort_session();
            return status;
        }
        /* The installer acquires LVGL then storage. Never enter it holding
         * storage: the UI may already hold LVGL while waiting for storage. */
        if (s_rpc_session) {
            s_rpc_reply = device_serial_rpc((const char *)s_sink.buf, &s_rpc_restart);
            return s_rpc_reply ? 0 : 6;
        }
        if (!s_theme_upload) {
            /* Keep only the newly installed boot format after its completed
             * file is durable. Keep all removals inside the job lock. */
            const char *old_video = s_obv_upload ? SD_BOOT_LEGACY : SD_BOOT_OBV;
            esp_err_t video_err = sd_card_remove(old_video);
            esp_err_t jpeg_err = sd_card_remove(SD_BOOT_JPEG);
            if (video_err != ESP_OK || jpeg_err != ESP_OK) {
                abort_session();
                return 6;
            }
        }
        release_storage();
        if (s_theme_upload && theme_bundle_install(THEME_BUNDLE_PATH) != ESP_OK) {
            abort_session();
            return 6;
        }
        return 0;
    }
    case CMD_ABORT:
        if (length != 0) return 1;
        abort_session();
        return 0;
    default:
        return 1;
    }
}

static void bootmedia_serial_task(void *arg)
{
    (void)arg;
    usb_serial_jtag_driver_config_t config = {
        .tx_buffer_size = 4096,
        .rx_buffer_size = 8192,
    };
    if (!usb_serial_jtag_is_driver_installed() &&
        usb_serial_jtag_driver_install(&config) != ESP_OK) {
        ESP_LOGE(TAG, "USB Serial/JTAG driver install failed");
        vTaskDelete(NULL);
        return;
    }

    uint8_t header[FRAME_HEADER_SIZE];
    uint8_t *payload = heap_caps_malloc(FRAME_DATA_MAX, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!payload) {
        ESP_LOGE(TAG, "frame buffer allocation failed");
        vTaskDelete(NULL);
        return;
    }

    uint16_t last_sequence = 0;
    uint8_t last_command = 0;
    uint32_t last_crc = 0;
    uint8_t last_status = 0;
    uint32_t last_received = 0;
    bool have_last = false;
    for (;;) {
        if (!find_frame_header(header)) {
            abort_session();
            have_last = false;
            continue;
        }
        uint8_t version = header[4];
        uint8_t command = header[5];
        uint16_t sequence = get_u16le(header + 6);
        uint32_t length = get_u32le(header + 8);
        uint32_t payload_crc = get_u32le(header + 12);
        size_t payload_received = 0;
        if (version != 1 || length > FRAME_DATA_MAX ||
            (length && !read_exact(payload, length, SESSION_TIMEOUT, &payload_received))) {
            ESP_LOGW(TAG, "frame payload timeout/invalid: cmd=%u seq=%u version=%u len=%u received=%u",
                     command, sequence, version, (unsigned)length, (unsigned)payload_received);
            send_reply(command, sequence, 7, upload_sink_is_open(&s_sink) ? s_sink.received : 0);
            abort_session();
            have_last = false;
            continue;
        }

        uint8_t status;
        uint32_t received;
        uint32_t actual_crc = esp_rom_crc32_le(0, payload, length);
        if (actual_crc != payload_crc) {
            status = 8;
            received = upload_sink_is_open(&s_sink) ? s_sink.received : 0;
        } else if (have_last && sequence == last_sequence && command == last_command &&
                   payload_crc == last_crc) {
            status = last_status;
            received = last_received;
        } else {
            int64_t process_started_us = esp_timer_get_time();
            status = process_frame(command, payload, length);
            int64_t process_elapsed_us = esp_timer_get_time() - process_started_us;
            if (command == CMD_DATA && process_elapsed_us >= 1000000) {
                uint32_t offset = length >= 4 ? get_u32le(payload) : 0;
                ESP_LOGW(TAG, "slow DATA processing: seq=%u offset=%u len=%u elapsed=%lldms status=%u",
                         sequence, (unsigned)offset, (unsigned)length,
                         (long long)(process_elapsed_us / 1000), status);
            }
            received = upload_sink_is_open(&s_sink) ? s_sink.received : 0;
            if (command == CMD_END && status == 0) received = s_sink.expected;
            last_sequence = sequence;
            last_command = command;
            last_crc = payload_crc;
            last_status = status;
            last_received = received;
            have_last = true;
        }
        s_activity = xTaskGetTickCount();
        bool acknowledged = send_reply(command, sequence, status, received);
        if (!acknowledged) {
            ESP_LOGW(TAG, "ACK transmit failed: cmd=%u seq=%u status=%u received=%u",
                     command, sequence, status, (unsigned)received);
        }
        if (command == CMD_END && status == 0) {
            bool was_rpc = s_rpc_reply != NULL;
            bool restart = !was_rpc || s_rpc_restart;
            bool was_theme = s_theme_upload;
            abort_session();
            if (!restart || !acknowledged) continue;
            ESP_LOGI(TAG, "%s", was_rpc ? "USB reboot requested" : was_theme ? "theme installed on SD" : "boot animation installed on SD");
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }
    }
}

esp_err_t bootmedia_serial_start(void)
{
    /* Keep USB transfers below OBD polling and LVGL (both priority 4). */
    BaseType_t ok = xTaskCreate(bootmedia_serial_task, "bootmedia_usb", 8192, NULL, 3, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
