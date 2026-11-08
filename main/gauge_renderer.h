#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

#define GAUGE_RENDERER_CENTER_X              240
#define GAUGE_RENDERER_CENTER_Y              240
#define GAUGE_RENDERER_NEEDLE_SPRITE_WIDTH    11
#define GAUGE_RENDERER_NEEDLE_SPRITE_HEIGHT   250
#define GAUGE_RENDERER_NEEDLE_PIVOT_X         5
#define GAUGE_RENDERER_NEEDLE_PIVOT_Y         200
#define GAUGE_RENDERER_SMOOTHING_COEFFICIENT  0.18f
#define GAUGE_RENDERER_MIN_PSI                0.0f
#define GAUGE_RENDERER_MAX_PSI                150.0f
#define GAUGE_RENDERER_START_ANGLE_DEG        225.0f
#define GAUGE_RENDERER_END_ANGLE_DEG          -45.0f

typedef struct {
    float target_value;
    float displayed_value;
    float min_value;
    float max_value;
    float start_angle_deg;
    float end_angle_deg;
} gauge_state_t;

typedef struct gauge_text gauge_text_t;
typedef struct gauge_warning_indicator gauge_warning_indicator_t;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t width;
    lv_align_t align;
    lv_text_align_t text_align;
    lv_color_t color;
    const lv_font_t *font;
    const char *units;
    bool visible;
} gauge_text_config_t;

typedef enum {
    GAUGE_WARNING_CLEAR,
    GAUGE_WARNING_ACTIVE,
    GAUGE_WARNING_CRITICAL,
} gauge_warning_state_t;

typedef struct {
    int32_t x;
    int32_t y;
    lv_align_t align;
    uint16_t size;
    uint32_t blink_interval_ms;
    lv_color_t warning_color;
    lv_color_t critical_color;
    const void *image_src;
    bool recolor_image;
    bool visible;
    bool blink;
} gauge_warning_config_t;

esp_err_t gauge_renderer_init(esp_lcd_panel_handle_t dpi_panel,
                              const uint8_t *background_rgb565,
                              size_t background_size,
                              const uint8_t *needle_rgba8888,
                              size_t needle_size);
void gauge_renderer_set_value(float value);
void gauge_renderer_set_value_immediately(float value);
esp_err_t gauge_renderer_set_smoothing_coefficient(float coefficient);
float gauge_value_to_angle(float value);
esp_err_t gauge_renderer_get_state(gauge_state_t *state);

esp_err_t gauge_text_create(const gauge_text_config_t *config, gauge_text_t **text);
esp_err_t gauge_text_measure_width(const lv_font_t *font,
                                   const char *value,
                                   int32_t *width);
esp_err_t gauge_text_set_text(gauge_text_t *text, const char *value);
esp_err_t gauge_text_set_integer(gauge_text_t *text, int32_t value);
esp_err_t gauge_text_set_float(gauge_text_t *text, float value, uint8_t decimals);
esp_err_t gauge_text_set_visible(gauge_text_t *text, bool visible);

esp_err_t gauge_warning_indicator_create(const gauge_warning_config_t *config,
                                         gauge_warning_indicator_t **indicator);
esp_err_t gauge_warning_indicator_set_state(gauge_warning_indicator_t *indicator,
                                            gauge_warning_state_t state);
esp_err_t gauge_warning_indicator_set_visible(gauge_warning_indicator_t *indicator,
                                              bool visible);
esp_err_t gauge_warning_indicator_set_blink(gauge_warning_indicator_t *indicator,
                                            bool blink,
                                            uint32_t interval_ms);
esp_err_t gauge_warning_indicator_set_colors(gauge_warning_indicator_t *indicator,
                                             lv_color_t warning_color,
                                             lv_color_t critical_color);
