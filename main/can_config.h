#pragma once

#include "driver/gpio.h"

#define CAN_TX_GPIO                       GPIO_NUM_5
#define CAN_RX_GPIO                       GPIO_NUM_6
#define CAN_BITRATE                       500000
#define CAN_LISTEN_ONLY                   0
#define CAN_DIAGNOSTICS_ENABLED           1
#define CAN_DIAGNOSTIC_INTERVAL_MS        5000
#define CAN_RAW_SAMPLES_PER_INTERVAL      3
#define CAN_STATE_LOGS_PER_INTERVAL       4
#define CAN_RX_QUEUE_DEPTH                128
#define CAN_STATE_QUEUE_DEPTH             16
#define CAN_STATUS_POLL_MS                100
#define CAN_RECOVERY_COOLDOWN_MS          10000
#define CAN_TASK_STACK_BYTES              4096
#define CAN_TASK_PRIORITY                 1

/* Reference for a future decoder; raw reception is not filtered to this base. */
#define CAN_MS_REALTIME_BASE_ID           1520
