#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define GAUGE_RENDERER_CENTER_X             240
#define GAUGE_RENDERER_CENTER_Y             240
#define GAUGE_RENDERER_NEEDLE_SPRITE_WIDTH  11
#define GAUGE_RENDERER_NEEDLE_SPRITE_HEIGHT 250
#define GAUGE_RENDERER_NEEDLE_PIVOT_X       5
#define GAUGE_RENDERER_NEEDLE_PIVOT_Y       200
#define GAUGE_RENDERER_SMOOTHING_COEFFICIENT 0.18f
#define GAUGE_RENDERER_MIN_PSI              0.0f
#define GAUGE_RENDERER_MAX_PSI              150.0f
#define GAUGE_RENDERER_START_ANGLE_DEG      225.0f
#define GAUGE_RENDERER_END_ANGLE_DEG        -45.0f

typedef struct {
    float target_value;
    float displayed_value;
    float min_value;
    float max_value;
    float start_angle_deg;
    float end_angle_deg;
} gauge_state_t;

esp_err_t gauge_renderer_init(const uint8_t *background_rgb565,
                              size_t background_size,
                              const uint8_t *needle_rgba8888,
                              size_t needle_size);
void gauge_renderer_set_value(float value);
void gauge_renderer_set_value_immediately(float value);
float gauge_value_to_angle(float value);
const gauge_state_t *gauge_renderer_get_state(void);

/*
 * The active DSI framebuffer is RGB888, so this intentionally accepts its
 * native pointer type instead of an incompatible uint16_t RGB565 pointer.
 */
esp_err_t gauge_renderer_render(void *framebuffer);
