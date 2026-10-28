#include <inttypes.h>
#include <stdio.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "can_config.h"
#include "can_receiver.h"

static const char *TAG = "can_rx";

_Static_assert(CAN_RX_QUEUE_DEPTH > 0 && CAN_STATE_QUEUE_DEPTH > 0, "CAN queues must be bounded and nonempty");
_Static_assert(CAN_RAW_SAMPLES_PER_INTERVAL > 0, "CAN sample capacity must be positive");
_Static_assert(CAN_DIAGNOSTIC_INTERVAL_MS >= 1000, "CAN diagnostics must be rate limited");
_Static_assert(CAN_STATUS_POLL_MS > 0, "CAN status polling must have a positive interval");
_Static_assert(CAN_MS_REALTIME_BASE_ID <= 0x7FF, "Realtime broadcast base must be an 11-bit ID");
_Static_assert(__atomic_always_lock_free(sizeof(uint32_t), 0), "CAN ISR counters must be lock free");

typedef struct {
    twai_error_state_t old_state;
    twai_error_state_t new_state;
} can_state_event_t;

typedef struct {
    uint32_t received;
    uint32_t drops;
    uint32_t read_errors;
    uint32_t unsupported;
    uint32_t bus_errors;
    uint32_t state_drops;
} can_counters_t;

static DRAM_ATTR struct {
    twai_node_handle_t node;
    QueueHandle_t rx_queue;
    QueueHandle_t state_queue;
    StaticQueue_t rx_queue_control;
    StaticQueue_t state_queue_control;
    uint8_t rx_storage[CAN_RX_QUEUE_DEPTH * sizeof(can_rx_frame_t)];
    uint8_t state_storage[CAN_STATE_QUEUE_DEPTH * sizeof(can_state_event_t)];
    can_counters_t counters;
    uint32_t error_flags;
    uint32_t last_rx_tick;
    uint32_t state_epoch;
} receiver;

static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
static can_receiver_status_t published_status;
static bool start_attempted;
static can_frame_handler_t frame_handler;
static void *frame_handler_context;

static const char *state_name(can_receiver_state_t state)
{
    switch (state) {
    case CAN_RECEIVER_STOPPED: return "stopped";
    case CAN_RECEIVER_ACTIVE: return "error_active";
    case CAN_RECEIVER_WARNING: return "error_warning";
    case CAN_RECEIVER_PASSIVE: return "error_passive";
    case CAN_RECEIVER_BUS_OFF: return "bus_off";
    case CAN_RECEIVER_RECOVERING: return "recovering";
    case CAN_RECEIVER_FAILED: return "failed";
    default: return "unknown";
    }
}

static can_receiver_state_t controller_state(twai_error_state_t state)
{
    switch (state) {
    case TWAI_ERROR_ACTIVE: return CAN_RECEIVER_ACTIVE;
    case TWAI_ERROR_WARNING: return CAN_RECEIVER_WARNING;
    case TWAI_ERROR_PASSIVE: return CAN_RECEIVER_PASSIVE;
    case TWAI_ERROR_BUS_OFF: return CAN_RECEIVER_BUS_OFF;
    default: return CAN_RECEIVER_FAILED;
    }
}

static void publish_status(const can_receiver_status_t *status)
{
    portENTER_CRITICAL(&status_lock);
    published_status = *status;
    portEXIT_CRITICAL(&status_lock);
}

esp_err_t can_receiver_get_status(can_receiver_status_t *status)
{
    if (status == NULL) {
        ESP_LOGE(TAG, "Cannot copy CAN status to a null pointer");
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&status_lock);
    *status = published_status;
    portEXIT_CRITICAL(&status_lock);
    return ESP_OK;
}

static bool IRAM_ATTR receive_frame(twai_node_handle_t node,
                                    const twai_rx_done_event_data_t *event, void *context)
{
    (void)event;
    (void)context;
    can_rx_frame_t received = {0};
    twai_frame_t frame = {
        .buffer = received.data,
        .buffer_len = sizeof(received.data),
    };
    if (twai_node_receive_from_isr(node, &frame) != ESP_OK) {
        __atomic_fetch_add(&receiver.counters.read_errors, 1, __ATOMIC_RELAXED);
        return false;
    }
    __atomic_fetch_add(&receiver.counters.received, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&receiver.last_rx_tick, xTaskGetTickCountFromISR(), __ATOMIC_RELAXED);
    if (frame.header.fdf || frame.header.brs) {
        __atomic_fetch_add(&receiver.counters.unsupported, 1, __ATOMIC_RELAXED);
        return false;
    }
    received.id = frame.header.id;
    received.timestamp_us = frame.header.timestamp;
    received.dlc = frame.header.dlc;
    received.extended = frame.header.ide;
    received.rtr = frame.header.rtr;
    received.data_length = received.rtr ? 0 : (received.dlc > 8 ? 8 : received.dlc);

    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(receiver.rx_queue, &received, &woken) != pdTRUE) {
        __atomic_fetch_add(&receiver.counters.drops, 1, __ATOMIC_RELAXED);
    }
    return woken == pdTRUE;
}

static bool IRAM_ATTR state_changed(twai_node_handle_t node,
                                    const twai_state_change_event_data_t *event, void *context)
{
    (void)node;
    (void)context;
    /* Publish state and episode together so recovery cannot race the driver state update. */
    uint32_t epoch = __atomic_load_n(&receiver.state_epoch, __ATOMIC_RELAXED);
    if (event->new_sta == TWAI_ERROR_BUS_OFF) {
        epoch += 4;
    }
    __atomic_store_n(&receiver.state_epoch, (epoch & ~3U) | event->new_sta, __ATOMIC_RELAXED);
    const can_state_event_t change = {
        .old_state = event->old_sta,
        .new_state = event->new_sta,
    };
    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(receiver.state_queue, &change, &woken) != pdTRUE) {
        __atomic_fetch_add(&receiver.counters.state_drops, 1, __ATOMIC_RELAXED);
    }
    return woken == pdTRUE;
}

static bool IRAM_ATTR bus_error(twai_node_handle_t node,
                                const twai_error_event_data_t *event, void *context)
{
    (void)node;
    (void)context;
    __atomic_fetch_add(&receiver.counters.bus_errors, 1, __ATOMIC_RELAXED);
    __atomic_fetch_or(&receiver.error_flags, event->err_flags.val, __ATOMIC_RELAXED);
    return false;
}

static void collect_counters(can_receiver_status_t *status, can_counters_t *previous)
{
#define COLLECT(counter, total) do { \
    const uint32_t now = __atomic_load_n(&receiver.counters.counter, __ATOMIC_RELAXED); \
    status->total += (uint32_t)(now - previous->counter); \
    previous->counter = now; \
} while (0)
    COLLECT(received, received_frames);
    COLLECT(drops, queue_drops);
    COLLECT(read_errors, receive_errors);
    COLLECT(unsupported, unsupported_frames);
    COLLECT(bus_errors, bus_errors);
    COLLECT(state_drops, state_event_drops);
#undef COLLECT
    status->error_flags = __atomic_load_n(&receiver.error_flags, __ATOMIC_RELAXED);
    status->traffic_seen = status->received_frames != 0;
    if (status->traffic_seen) {
        const TickType_t last_rx = __atomic_load_n(&receiver.last_rx_tick, __ATOMIC_RELAXED);
        status->last_rx_age_ms = (xTaskGetTickCount() - last_rx) * portTICK_PERIOD_MS;
    }
}

static void log_frame(const can_rx_frame_t *frame)
{
    char payload[3 * sizeof(frame->data) + 1] = {0};
    size_t used = 0;
    for (uint8_t i = 0; i < frame->data_length; i++) {
        used += snprintf(payload + used, sizeof(payload) - used, "%02X ", frame->data[i]);
    }
    ESP_LOGI(TAG, "sample t=%" PRIu64 " us ID=0x%08" PRIX32 " %s RTR=%u DLC=%u data=%s",
             frame->timestamp_us, frame->id, frame->extended ? "EXT" : "STD",
             frame->rtr, frame->dlc,
             frame->rtr ? "<RTR: no payload>" : (used ? payload : "<empty>"));
}

static void log_status(const can_receiver_status_t *status, uint32_t suppressed)
{
    char age[32];
    if (status->traffic_seen) {
        snprintf(age, sizeof(age), "%" PRIu32 " ms", status->last_rx_age_ms);
    } else {
        snprintf(age, sizeof(age), "never");
    }
    ESP_LOGI(TAG, "state=%s rx=%" PRIu64 " rate=%.1f fps last_rx=%s "
             "queue_drops=%" PRIu64 " read_errors=%" PRIu64 " unsupported=%" PRIu64,
             state_name(status->state), status->received_frames, status->frames_per_second,
             age, status->queue_drops, status->receive_errors, status->unsupported_frames);
    ESP_LOGI(TAG, "bus_errors=%" PRIu64 " flags=0x%02" PRIX32 " TEC=%u REC=%u "
             "recoveries=%" PRIu32 " state_queue_drops=%" PRIu64 " transitions_suppressed=%" PRIu32,
             status->bus_errors, status->error_flags, status->tx_error_count,
             status->rx_error_count, status->recovery_requests, status->state_event_drops, suppressed);
}

static void receive_task(void *argument)
{
    (void)argument;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    can_receiver_status_t status = {.state = CAN_RECEIVER_ACTIVE, .listen_only = CAN_LISTEN_ONLY};
    can_counters_t previous = {0};
    can_rx_frame_t samples[CAN_RAW_SAMPLES_PER_INTERVAL];
    size_t sample_count = 0;
    uint32_t state_logs = 0;
    uint32_t suppressed = 0;
    uint32_t recovery_generation = UINT32_MAX;
    bool recovering = false;
    bool recovery_attempted = false;
    TickType_t last_recovery = 0;
    TickType_t last_poll = xTaskGetTickCount();
    TickType_t last_report = last_poll;
    uint64_t last_report_frames = 0;
    bool info_error_reported = false;

    while (true) {
        can_rx_frame_t frame;
        if (xQueueReceive(receiver.rx_queue, &frame, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (CAN_DIAGNOSTICS_ENABLED && sample_count < CAN_RAW_SAMPLES_PER_INTERVAL) {
                samples[sample_count++] = frame;
            }
            if (frame_handler != NULL) {
                frame_handler(&frame, frame_handler_context);
            }
        }
        can_state_event_t event;
        for (int i = 0; i < CAN_STATE_QUEUE_DEPTH &&
             xQueueReceive(receiver.state_queue, &event, 0) == pdTRUE; i++) {
            if (CAN_DIAGNOSTICS_ENABLED && state_logs < CAN_STATE_LOGS_PER_INTERVAL) {
                ESP_LOGI(TAG, "controller transition: %s -> %s",
                         state_name(controller_state(event.old_state)),
                         state_name(controller_state(event.new_state)));
                state_logs++;
            } else {
                suppressed++;
            }
        }

        const TickType_t now = xTaskGetTickCount();
        if (now - last_poll >= pdMS_TO_TICKS(CAN_STATUS_POLL_MS)) {
            last_poll = now;
            collect_counters(&status, &previous);
            twai_node_status_t node_status;
            const esp_err_t err = twai_node_get_info(receiver.node, &node_status, NULL);
            status.last_error = err;
            if (err != ESP_OK) {
                if (!info_error_reported) {
                    ESP_LOGW(TAG, "Cannot read CAN controller status: %s", esp_err_to_name(err));
                    info_error_reported = true;
                }
            } else {
                status.tx_error_count = node_status.tx_error_count;
                status.rx_error_count = node_status.rx_error_count;
                status.state = controller_state(node_status.state);
                if (recovering && node_status.state != TWAI_ERROR_BUS_OFF) {
                    ESP_LOGI(TAG, "Bus-off recovery completed: %s", state_name(status.state));
                    recovering = false;
                }
                const uint32_t epoch = __atomic_load_n(&receiver.state_epoch, __ATOMIC_RELAXED);
                if (node_status.state == TWAI_ERROR_BUS_OFF && (epoch & 3U) == TWAI_ERROR_BUS_OFF &&
                    epoch != recovery_generation &&
                    (!recovery_attempted || now - last_recovery >= pdMS_TO_TICKS(CAN_RECOVERY_COOLDOWN_MS))) {
                    recovery_generation = epoch;
                    last_recovery = now;
                    recovery_attempted = true;
                    const esp_err_t recovery_err = twai_node_recover(receiver.node);
                    status.last_error = recovery_err;
                    if (recovery_err == ESP_OK) {
                        status.recovery_requests++;
                        recovering = true;
                        ESP_LOGW(TAG, "Bus-off: recovery requested once; waiting for controller recovery");
                    } else {
                        ESP_LOGW(TAG, "Bus-off recovery request failed: %s; no retry in this episode",
                                 esp_err_to_name(recovery_err));
                    }
                }
                if (recovering) {
                    status.state = CAN_RECEIVER_RECOVERING;
                }
            }
            publish_status(&status);
        }
        if (now - last_report >= pdMS_TO_TICKS(CAN_DIAGNOSTIC_INTERVAL_MS)) {
            collect_counters(&status, &previous);
            const uint32_t elapsed_ms = (now - last_report) * portTICK_PERIOD_MS;
            status.frames_per_second =
                (float)(status.received_frames - last_report_frames) * 1000.0f / elapsed_ms;
            publish_status(&status);
            if (CAN_DIAGNOSTICS_ENABLED) {
                log_status(&status, suppressed);
                for (size_t i = 0; i < sample_count; i++) {
                    log_frame(&samples[i]);
                }
            }
            sample_count = 0;
            state_logs = 0;
            suppressed = 0;
            info_error_reported = false;
            last_report = now;
            last_report_frames = status.received_frames;
        }
    }
}

esp_err_t can_receiver_start(can_frame_handler_t handler, void *context)
{
    if (start_attempted) {
        ESP_LOGE(TAG, "CAN startup already attempted; refusing repeated initialization");
        return ESP_ERR_INVALID_STATE;
    }
    start_attempted = true;
    frame_handler = handler;
    frame_handler_context = context;
    TaskHandle_t task = NULL;
    bool enable_attempted = false;
    const char *stage = "create receive queues";
    esp_err_t err = ESP_ERR_NO_MEM;
    receiver.rx_queue = xQueueCreateStatic(CAN_RX_QUEUE_DEPTH, sizeof(can_rx_frame_t),
                                           receiver.rx_storage, &receiver.rx_queue_control);
    receiver.state_queue = xQueueCreateStatic(CAN_STATE_QUEUE_DEPTH, sizeof(can_state_event_t),
                                              receiver.state_storage, &receiver.state_queue_control);
    if (receiver.rx_queue == NULL || receiver.state_queue == NULL) {
        goto failed;
    }

    const twai_onchip_node_config_t config = {
        .io_cfg = {
            .tx = CAN_TX_GPIO,
            .rx = CAN_RX_GPIO,
            .quanta_clk_out = GPIO_NUM_NC,
            .bus_off_indicator = GPIO_NUM_NC,
        },
        .bit_timing.bitrate = CAN_BITRATE,
        .timestamp_resolution_hz = 1000000,
        .tx_queue_depth = 1,
        .flags.enable_listen_only = CAN_LISTEN_ONLY,
    };
    stage = "create on-chip TWAI node";
    err = twai_new_node_onchip(&config, &receiver.node);
    if (err != ESP_OK) {
        goto failed;
    }
    /* IDF's P4 HAL treats id=0/mask=0 as full-open, including both IDE formats. */
    const twai_mask_filter_config_t filter = {.id = 0, .mask = 0, .no_fd = true};
    stage = "configure accept-all filter";
    err = twai_node_config_mask_filter(receiver.node, 0, &filter);
    if (err != ESP_OK) {
        goto failed;
    }
    const twai_event_callbacks_t callbacks = {
        .on_rx_done = receive_frame,
        .on_state_change = state_changed,
        .on_error = bus_error,
    };
    stage = "register TWAI callbacks";
    err = twai_node_register_event_callbacks(receiver.node, &callbacks, NULL);
    if (err != ESP_OK) {
        goto failed;
    }
    stage = "create CAN receive task";
    if (xTaskCreate(receive_task, "can_rx", CAN_TASK_STACK_BYTES, NULL,
                    CAN_TASK_PRIORITY, &task) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto failed;
    }
    stage = "enable TWAI node";
    enable_attempted = true;
    err = twai_node_enable(receiver.node);
    if (err != ESP_OK) {
        goto failed;
    }
    const can_receiver_status_t initial = {
        .state = CAN_RECEIVER_ACTIVE,
        .listen_only = CAN_LISTEN_ONLY,
    };
    publish_status(&initial);
    ESP_LOGI(TAG, "Classical CAN %" PRIu32 " bit/s TX=GPIO%d RX=GPIO%d mode=%s; no application TX",
             (uint32_t)CAN_BITRATE, CAN_TX_GPIO, CAN_RX_GPIO, CAN_LISTEN_ONLY ? "listen-only" : "normal (ACK enabled)");
    ESP_LOGI(TAG, "Accept STD/EXT and RTR; RX queue=%u; timestamps=1 MHz; hardware RX-overrun count unavailable",
             CAN_RX_QUEUE_DEPTH);
    ESP_LOGI(TAG, "Realtime broadcast reference base=%u (0x%03X); no ECU decoding or gauge updates",
             CAN_MS_REALTIME_BASE_ID, CAN_MS_REALTIME_BASE_ID);
    if (CAN_LISTEN_ONLY) {
        ESP_LOGW(TAG, "Listen-only sends no ACK: another active receiver must acknowledge ECU frames");
    }
    xTaskNotifyGive(task);
    return ESP_OK;

failed:
    ESP_LOGE(TAG, "CAN startup failed at %s: %s; display will continue", stage, esp_err_to_name(err));
    if (task != NULL) {
        vTaskDelete(task);
    }
    if (receiver.node != NULL) {
        if (enable_attempted) {
            const esp_err_t stop_err = twai_node_disable(receiver.node);
            if (stop_err != ESP_OK && stop_err != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "CAN node stop failed during cleanup: %s", esp_err_to_name(stop_err));
            }
        }
        const esp_err_t cleanup_err = twai_node_delete(receiver.node);
        if (cleanup_err != ESP_OK) {
            ESP_LOGW(TAG, "CAN node cleanup failed: %s; retaining ISR queues for safety",
                     esp_err_to_name(cleanup_err));
        } else {
            receiver.node = NULL;
        }
    }
    if (receiver.node == NULL && receiver.rx_queue != NULL) {
        vQueueDelete(receiver.rx_queue);
    }
    if (receiver.node == NULL && receiver.state_queue != NULL) {
        vQueueDelete(receiver.state_queue);
    }
    const can_receiver_status_t failed_status = {
        .state = CAN_RECEIVER_FAILED,
        .last_error = err,
        .listen_only = CAN_LISTEN_ONLY,
    };
    publish_status(&failed_status);
    return err;
}
