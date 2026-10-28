#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint32_t id;
    uint64_t timestamp_us;
    uint8_t dlc;
    uint8_t data_length;
    bool extended;
    bool rtr;
    uint8_t data[8];
} can_rx_frame_t;

typedef enum {
    CAN_RECEIVER_STOPPED,
    CAN_RECEIVER_ACTIVE,
    CAN_RECEIVER_WARNING,
    CAN_RECEIVER_PASSIVE,
    CAN_RECEIVER_BUS_OFF,
    CAN_RECEIVER_RECOVERING,
    CAN_RECEIVER_FAILED,
} can_receiver_state_t;

typedef struct {
    can_receiver_state_t state;
    esp_err_t last_error;
    bool listen_only;
    bool traffic_seen;
    uint64_t received_frames;
    uint64_t queue_drops;
    uint64_t receive_errors;
    uint64_t unsupported_frames;
    uint64_t bus_errors;
    uint64_t state_event_drops;
    uint32_t error_flags;
    uint32_t last_rx_age_ms;
    float frames_per_second;
    uint16_t tx_error_count;
    uint16_t rx_error_count;
    uint32_t recovery_requests;
} can_receiver_status_t;

/* Called in the CAN task, never an ISR. Copy the frame if retaining it; do not block. */
typedef void (*can_frame_handler_t)(const can_rx_frame_t *frame, void *context);

esp_err_t can_receiver_start(can_frame_handler_t handler, void *context);
/* Task-context snapshot, refreshed approximately every CAN_STATUS_POLL_MS. */
esp_err_t can_receiver_get_status(can_receiver_status_t *status);
