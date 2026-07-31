// boot_block_player.c — 从 hokori_vehicle_gauge 移植
// delta_varint_rgb565_black_v1 格式解码 + LVGL canvas 逐帧渲染
#include "app_obd_dsp/boot_block_player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

typedef struct {
    uint16_t canvas_width;
    uint16_t canvas_height;
    uint16_t grid_width;
    uint16_t grid_height;
    uint16_t fps;
    uint16_t frame_count;
    uint32_t duration_ms;
    char stream_format[32];
    char data_file[32];
} boot_block_manifest_t;

typedef struct {
    bool active;
    bool finished;
    uint16_t next_frame_index;
    uint32_t next_frame_due_ms;
    uint32_t frame_interval_ms;
    uint8_t *stream_data;
    size_t stream_size;
    size_t stream_offset;
    lv_obj_t *canvas_obj;
    lv_color_t *canvas_buf;
    uint16_t *x_edges;
    uint16_t *y_edges;
    boot_block_manifest_t manifest;
} boot_block_player_state_t;

static const char *TAG = "boot_block";

#define BOOT_BLOCK_DEFAULT_DATA_FILE "boot_block.bin"
#define BOOT_BLOCK_DEFAULT_STREAM_FORMAT "delta_varint_rgb565_black_v1"
#define BOOT_BLOCK_MAX_DIMENSION     512u

// 路径由外部设置 (简化版, 不依赖完整 registry)
static char s_manifest_path[96] = "/bootmedia/boot_block.txt";
static char s_data_path[96] = "/bootmedia/boot_block.bin";

void boot_block_player_set_paths(const char *manifest, const char *data)
{
    if (manifest) { strncpy(s_manifest_path, manifest, sizeof(s_manifest_path) - 1); s_manifest_path[sizeof(s_manifest_path) - 1] = '\0'; }
    if (data)     { strncpy(s_data_path, data, sizeof(s_data_path) - 1); s_data_path[sizeof(s_data_path) - 1] = '\0'; }
}

static boot_block_player_state_t s_state;

static void reset_state(void) { memset(&s_state, 0, sizeof(s_state)); }

static void release_arrays(void) {
    free(s_state.x_edges); s_state.x_edges = NULL;
    free(s_state.y_edges); s_state.y_edges = NULL;
}

static void release_canvas(void) {
    if (s_state.canvas_obj) { lv_obj_del(s_state.canvas_obj); s_state.canvas_obj = NULL; }
    free(s_state.canvas_buf); s_state.canvas_buf = NULL;
}

static void close_stream(void) {
    free(s_state.stream_data); s_state.stream_data = NULL;
    s_state.stream_size = 0; s_state.stream_offset = 0;
}

static bool parse_u32(const char *value, uint32_t *out) {
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (end == value) return false;
    *out = (uint32_t)parsed;
    return true;
}

static bool manifest_load(boot_block_manifest_t *out) {
    FILE *fp = fopen(s_manifest_path, "rb");
    if (!fp) return false;

    boot_block_manifest_t m = {0};
    strncpy(m.stream_format, BOOT_BLOCK_DEFAULT_STREAM_FORMAT, sizeof(m.stream_format) - 1);
    strncpy(m.data_file, BOOT_BLOCK_DEFAULT_DATA_FILE, sizeof(m.data_file) - 1);

    char line[128];
    while (fgets(line, sizeof(line), fp)) {
        char *value = strchr(line, '=');
        if (!value) continue;
        *value++ = '\0';
        char *key = line;
        key[strcspn(key, "\r\n\t ")] = '\0';
        value[strcspn(value, "\r\n")] = '\0';

        uint32_t parsed = 0;
        if (strcmp(key, "canvas_width") == 0 && parse_u32(value, &parsed)) m.canvas_width = (uint16_t)parsed;
        else if (strcmp(key, "canvas_height") == 0 && parse_u32(value, &parsed)) m.canvas_height = (uint16_t)parsed;
        else if (strcmp(key, "grid_width") == 0 && parse_u32(value, &parsed)) m.grid_width = (uint16_t)parsed;
        else if (strcmp(key, "grid_height") == 0 && parse_u32(value, &parsed)) m.grid_height = (uint16_t)parsed;
        else if (strcmp(key, "fps") == 0 && parse_u32(value, &parsed)) m.fps = (uint16_t)parsed;
        else if (strcmp(key, "frame_count") == 0 && parse_u32(value, &parsed)) m.frame_count = (uint16_t)parsed;
        else if (strcmp(key, "duration_ms") == 0 && parse_u32(value, &parsed)) m.duration_ms = parsed;
        else if (strcmp(key, "data_file") == 0) strncpy(m.data_file, value, sizeof(m.data_file) - 1);
        else if (strcmp(key, "stream_format") == 0) strncpy(m.stream_format, value, sizeof(m.stream_format) - 1);
    }
    fclose(fp);

    if (!m.canvas_width || !m.canvas_height || !m.grid_width || !m.grid_height || !m.fps || !m.frame_count)
        return false;
    if (m.canvas_width > BOOT_BLOCK_MAX_DIMENSION || m.canvas_height > BOOT_BLOCK_MAX_DIMENSION)
        return false;
    if (!m.duration_ms) m.duration_ms = ((uint32_t)m.frame_count * 1000u) / m.fps;
    if (strcmp(m.stream_format, BOOT_BLOCK_DEFAULT_STREAM_FORMAT) != 0) return false;

    *out = m;
    return true;
}

static bool read_bytes(void *dst, size_t size) {
    if (!dst || !size || !s_state.stream_data) return false;
    if (s_state.stream_offset + size > s_state.stream_size) return false;
    memcpy(dst, s_state.stream_data + s_state.stream_offset, size);
    s_state.stream_offset += size;
    return true;
}

static bool read_varint_u32(uint32_t *out) {
    uint32_t value = 0, shift = 0;
    while (shift <= 21) {
        uint8_t byte;
        if (!read_bytes(&byte, 1)) return false;
        value |= (uint32_t)(byte & 0x7F) << shift;
        if (!(byte & 0x80)) { *out = value; return true; }
        shift += 7;
    }
    return false;
}

static uint32_t rgb565_to_rgb24(uint16_t rgb565) {
    uint32_t r5 = (rgb565 >> 11) & 0x1F;
    uint32_t g6 = (rgb565 >> 5) & 0x3F;
    uint32_t b5 = rgb565 & 0x1F;
    return (((r5 << 3) | (r5 >> 2)) << 16) | (((g6 << 2) | (g6 >> 4)) << 8) | ((b5 << 3) | (b5 >> 2));
}

static bool prepare_edges(uint16_t **out, uint16_t canvas_size, uint16_t grid_size) {
    uint16_t *edges = malloc(sizeof(uint16_t) * (grid_size + 1));
    if (!edges) return false;
    for (uint32_t i = 0; i <= grid_size; i++)
        edges[i] = (uint16_t)((uint32_t)i * canvas_size / grid_size);
    *out = edges;
    return true;
}

static bool prepare_canvas(lv_obj_t *parent, const boot_block_manifest_t *m) {
    size_t buf_size = LV_CANVAS_BUF_SIZE_TRUE_COLOR(m->canvas_width, m->canvas_height);
    s_state.canvas_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_state.canvas_buf) s_state.canvas_buf = heap_caps_malloc(buf_size, MALLOC_CAP_8BIT);
    if (!s_state.canvas_buf) { ESP_LOGW(TAG, "canvas alloc failed: %u bytes", (unsigned)buf_size); return false; }

    memset(s_state.canvas_buf, 0, buf_size);
    s_state.canvas_obj = lv_canvas_create(parent);
    if (!s_state.canvas_obj) return false;

    lv_canvas_set_buffer(s_state.canvas_obj, s_state.canvas_buf,
                         m->canvas_width, m->canvas_height, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(s_state.canvas_obj, lv_color_black(), LV_OPA_COVER);
    lv_obj_set_style_border_width(s_state.canvas_obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_state.canvas_obj, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(s_state.canvas_obj, LV_OBJ_FLAG_SCROLLABLE);
    return true;
}

static bool load_stream(void) {
    FILE *fp = fopen(s_data_path, "rb");
    if (!fp) { ESP_LOGW(TAG, "open block data failed: %s", s_data_path); return false; }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (file_size <= 0) { fclose(fp); return false; }

    uint8_t *data = heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) data = heap_caps_malloc(file_size, MALLOC_CAP_8BIT);
    if (!data) { fclose(fp); return false; }

    size_t read = fread(data, 1, file_size, fp);
    fclose(fp);
    if ((long)read != file_size) { free(data); return false; }

    s_state.stream_data = data;
    s_state.stream_size = file_size;
    s_state.stream_offset = 0;
    return true;
}

static bool apply_change(uint32_t idx, uint32_t packed) {
    if (idx >= (uint32_t)s_state.manifest.grid_width * s_state.manifest.grid_height) return false;

    uint16_t gx = (uint16_t)(idx % s_state.manifest.grid_width);
    uint16_t gy = (uint16_t)(idx / s_state.manifest.grid_width);
    uint16_t x0 = s_state.x_edges[gx], x1 = s_state.x_edges[gx + 1];
    uint16_t y0 = s_state.y_edges[gy], y1 = s_state.y_edges[gy + 1];
    lv_color_t color = (packed == 0) ? lv_color_black()
        : lv_color_make((packed >> 16) & 0xFF, (packed >> 8) & 0xFF, packed & 0xFF);

    for (uint16_t y = y0; y < y1; y++) {
        uint32_t row = (uint32_t)y * s_state.manifest.canvas_width;
        for (uint16_t x = x0; x < x1; x++)
            s_state.canvas_buf[row + x] = color;
    }
    return true;
}

static bool apply_next_frame(void) {
    uint8_t header[2];
    if (!read_bytes(header, 2)) return false;

    uint16_t change_count = (uint16_t)header[0] | ((uint16_t)header[1] << 8);
    uint32_t prev_idx = UINT32_MAX;

    for (uint16_t i = 0; i < change_count; i++) {
        uint32_t delta_and_black;
        if (!read_varint_u32(&delta_and_black)) return false;

        bool is_black = (delta_and_black & 1) != 0;
        uint32_t delta_idx = delta_and_black >> 1;
        uint32_t idx = (prev_idx == UINT32_MAX) ? delta_idx : prev_idx + delta_idx;
        prev_idx = idx;

        uint32_t packed = 0;
        if (!is_black) {
            uint8_t color_bytes[2];
            if (!read_bytes(color_bytes, 2)) return false;
            uint16_t rgb565 = (uint16_t)color_bytes[0] | ((uint16_t)color_bytes[1] << 8);
            packed = rgb565_to_rgb24(rgb565);
        }
        if (!apply_change(idx, packed)) return false;
    }
    return true;
}

bool boot_block_player_create(lv_obj_t *parent, lv_obj_t **out_obj) {
    if (out_obj) *out_obj = NULL;
    boot_block_player_destroy();

    boot_block_manifest_t manifest;
    if (!manifest_load(&manifest)) {
        ESP_LOGW(TAG, "manifest load failed: %s", s_manifest_path);
        return false;
    }

    s_state.manifest = manifest;
    s_state.frame_interval_ms = 1000u / manifest.fps;
    if (!s_state.frame_interval_ms) s_state.frame_interval_ms = 1;

    if (!prepare_edges(&s_state.x_edges, manifest.canvas_width, manifest.grid_width) ||
        !prepare_edges(&s_state.y_edges, manifest.canvas_height, manifest.grid_height) ||
        !prepare_canvas(parent, &manifest) ||
        !load_stream()) {
        boot_block_player_destroy();
        return false;
    }
    lv_obj_align(s_state.canvas_obj, LV_ALIGN_CENTER, 0, 0);

    s_state.active = true;
    s_state.finished = false;
    s_state.next_frame_index = 0;
    s_state.next_frame_due_ms = 0;

    // 立刻解码首帧
    if (apply_next_frame()) {
        s_state.next_frame_index = 1;
        s_state.next_frame_due_ms = s_state.frame_interval_ms;
        if (s_state.canvas_obj) lv_obj_invalidate(s_state.canvas_obj);
    } else {
        boot_block_player_destroy();
        return false;
    }

    if (out_obj) *out_obj = s_state.canvas_obj;
    ESP_LOGI(TAG, "player ready: %ux%u grid=%ux%u fps=%u frames=%u dur=%ums",
             manifest.canvas_width, manifest.canvas_height,
             manifest.grid_width, manifest.grid_height,
             manifest.fps, manifest.frame_count, manifest.duration_ms);
    return true;
}

void boot_block_player_destroy(void) {
    close_stream();
    release_canvas();
    release_arrays();
    reset_state();
}

bool boot_block_player_is_finished(void) {
    return !s_state.active || s_state.finished;
}

bool boot_block_player_get_canvas_size(uint16_t *w, uint16_t *h) {
    if (!s_state.active || !s_state.manifest.canvas_width) return false;
    if (w) *w = s_state.manifest.canvas_width;
    if (h) *h = s_state.manifest.canvas_height;
    return true;
}

uint32_t boot_block_player_get_duration_ms(void) {
    return s_state.manifest.duration_ms;
}

void boot_block_player_update(uint32_t elapsed_ms) {
    if (!s_state.active || s_state.finished) return;

    bool changed = false;
    while (s_state.next_frame_index < s_state.manifest.frame_count &&
           elapsed_ms >= s_state.next_frame_due_ms) {
        if (!apply_next_frame()) {
            ESP_LOGW(TAG, "stream ended early at frame %u/%u (elapsed=%ums)",
                     s_state.next_frame_index, s_state.manifest.frame_count, elapsed_ms);
            s_state.finished = true;
            break;
        }
        s_state.next_frame_index++;
        s_state.next_frame_due_ms += s_state.frame_interval_ms;
        changed = true;
    }

    if (s_state.next_frame_index >= s_state.manifest.frame_count ||
        elapsed_ms >= s_state.manifest.duration_ms) {
        ESP_LOGI(TAG, "finished: frame=%u/%u elapsed=%ums dur=%ums",
                 s_state.next_frame_index, s_state.manifest.frame_count,
                 elapsed_ms, s_state.manifest.duration_ms);
        s_state.finished = true;
        close_stream();
    }

    if (changed && s_state.canvas_obj) {
        lv_obj_invalidate(s_state.canvas_obj);
    }
}
