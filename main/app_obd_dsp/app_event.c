// ================================================================
//  app_event.c — cross-task event queue implementation
// ================================================================

#include "app_event.h"
#include "esp_log.h"

static const char *TAG = "app_event";
static QueueHandle_t s_queue = NULL;

void app_event_init(void)
{
    if (s_queue) return;
    s_queue = xQueueCreate(APP_EVENT_QUEUE_DEPTH, sizeof(app_event_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "Queue create failed!");
    } else {
        ESP_LOGI(TAG, "Event queue initialized (depth=%d)", APP_EVENT_QUEUE_DEPTH);
    }
}

bool app_event_send(app_event_type_t type, uint32_t data)
{
    if (!s_queue) return false;
    app_event_t evt = { .type = type, .data.u32 = data };
    // Can be called from an ISR or a normal task
    if (xPortInIsrContext()) {
        BaseType_t woken = pdFALSE;
        xQueueSendFromISR(s_queue, &evt, &woken);
        return woken == pdTRUE;
    }
    return xQueueSend(s_queue, &evt, 0) == pdTRUE;  // non-blocking
}

bool app_event_recv(app_event_t *out)
{
    if (!s_queue || !out) return false;
    return xQueueReceive(s_queue, out, 0) == pdTRUE;  // non-blocking
}

QueueHandle_t app_event_queue(void)
{
    return s_queue;
}
