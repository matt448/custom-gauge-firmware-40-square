#include <math.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"

#include "display_config.h"
#include "esp_lvgl_port.h"
#include "gauge_renderer.h"

#define GAUGE_RENDERER_BACKGROUND_SIZE \
    (NEWHAVEN_DISPLAY_H_RES * NEWHAVEN_DISPLAY_V_RES * 2U)
#define GAUGE_RENDERER_NEEDLE_SIZE \
    (GAUGE_RENDERER_NEEDLE_SPRITE_WIDTH * GAUGE_RENDERER_NEEDLE_SPRITE_HEIGHT * 4U)
#define GAUGE_TEXT_CAPACITY 64
#define GAUGE_TEXT_UNITS_CAPACITY 12
#define GAUGE_WARNING_DEFAULT_BLINK_INTERVAL_MS 500
#define GAUGE_RENDERER_TIMER_PERIOD_MS 16

#if NEWHAVEN_DISPLAY_PIXEL_FORMAT == NEWHAVEN_DISPLAY_PIXEL_FORMAT_RGB888
#define GAUGE_RENDERER_DISPLAY_COLOR_FORMAT LV_COLOR_FORMAT_RGB888
#define GAUGE_RENDERER_DISPLAY_BYTES_PER_PIXEL 3U
#define GAUGE_RENDERER_DISPLAY_FORMAT_NAME "RGB888"
#elif NEWHAVEN_DISPLAY_PIXEL_FORMAT == NEWHAVEN_DISPLAY_PIXEL_FORMAT_RGB565
#define GAUGE_RENDERER_DISPLAY_COLOR_FORMAT LV_COLOR_FORMAT_RGB565
#define GAUGE_RENDERER_DISPLAY_BYTES_PER_PIXEL 2U
#define GAUGE_RENDERER_DISPLAY_FORMAT_NAME "RGB565"
#else
#error "NEWHAVEN_DISPLAY_PIXEL_FORMAT must be RGB888 or RGB565."
#endif

static const char *TAG = "gauge_renderer";

struct gauge_text {
    struct gauge_text *next;
    lv_obj_t *label;
    const lv_font_t *font;
    lv_align_t align;
    lv_text_align_t text_align;
    lv_color_t color;
    char units[GAUGE_TEXT_UNITS_CAPACITY];
    char pending_value[GAUGE_TEXT_CAPACITY];
    char rendered_value[GAUGE_TEXT_CAPACITY];
    int32_t x;
    int32_t y;
    int32_t width;
    bool visible;
    bool dirty;
};

struct gauge_warning_indicator {
    struct gauge_warning_indicator *next;
    lv_obj_t *object;
    const void *image_src;
    lv_align_t align;
    lv_color_t warning_color;
    lv_color_t critical_color;
    gauge_warning_state_t state;
    uint32_t blink_interval_ms;
    int32_t x;
    int32_t y;
    uint16_t size;
    bool recolor_image;
    bool visible;
    bool blink;
    bool dirty;
    bool blink_phase_valid;
    bool blink_phase;
    bool hidden;
};

static gauge_state_t s_gauge_state;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_state_generation;
static float s_smoothing_coefficient = GAUGE_RENDERER_SMOOTHING_COEFFICIENT;
static uint8_t *s_needle_pixels;
static lv_image_dsc_t s_background_image;
static lv_image_dsc_t s_needle_image;
static lv_display_t *s_lvgl_display;
static lv_obj_t *s_background_object;
static lv_obj_t *s_needle_object;
static lv_timer_t *s_renderer_timer;
static SemaphoreHandle_t s_widget_lock;
static gauge_text_t *s_text_widgets;
static gauge_warning_indicator_t *s_warning_widgets;
static int32_t s_last_needle_rotation = -1;
static int64_t s_last_update_us;
static bool s_scene_created;
static bool s_initialized;
#if GAUGE_PERFORMANCE_DIAGNOSTICS
static int64_t s_render_start_us;
static int64_t s_render_total_us;
static int64_t s_performance_window_start_us;
static uint32_t s_render_count;
static uint32_t s_refresh_count;
#endif

static float clamp_value(float value)
{
    if (value < GAUGE_RENDERER_MIN_PSI) {
        return GAUGE_RENDERER_MIN_PSI;
    }
    if (value > GAUGE_RENDERER_MAX_PSI) {
        return GAUGE_RENDERER_MAX_PSI;
    }
    return value;
}

static void create_scene(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    s_background_object = lv_image_create(screen);
    lv_image_set_src(s_background_object, &s_background_image);
    lv_obj_set_pos(s_background_object, 0, 0);
    lv_obj_set_size(s_background_object, NEWHAVEN_DISPLAY_H_RES, NEWHAVEN_DISPLAY_V_RES);

    s_needle_object = lv_image_create(screen);
    lv_image_set_src(s_needle_object, &s_needle_image);
    lv_image_set_pivot(s_needle_object,
                       GAUGE_RENDERER_NEEDLE_PIVOT_X,
                       GAUGE_RENDERER_NEEDLE_PIVOT_Y);
    lv_image_set_antialias(s_needle_object, true);
    lv_obj_set_style_bg_opa(s_needle_object, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_pos(s_needle_object,
                   GAUGE_RENDERER_CENTER_X - GAUGE_RENDERER_NEEDLE_PIVOT_X,
                   GAUGE_RENDERER_CENTER_Y - GAUGE_RENDERER_NEEDLE_PIVOT_Y);
    s_scene_created = true;
}

static void create_text_object(gauge_text_t *text)
{
    text->label = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_color(text->label, text->color, LV_PART_MAIN);
    lv_obj_set_style_text_font(text->label, text->font, LV_PART_MAIN);
    lv_obj_set_style_text_align(text->label, text->text_align, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(text->label, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(text->label, 0, LV_PART_MAIN);
    if (text->width > 0) {
        lv_obj_set_width(text->label, text->width);
    }
    lv_label_set_long_mode(text->label, LV_LABEL_LONG_CLIP);
    lv_obj_align(text->label, text->align, text->x, text->y);
    memcpy(text->rendered_value, text->pending_value, sizeof(text->rendered_value));
    lv_label_set_text_static(text->label, text->rendered_value);
    if (!text->visible) {
        lv_obj_add_flag(text->label, LV_OBJ_FLAG_HIDDEN);
    }
    text->dirty = false;
}

static void create_warning_object(gauge_warning_indicator_t *indicator)
{
    if (indicator->image_src != NULL) {
        indicator->object = lv_image_create(lv_screen_active());
        lv_image_set_src(indicator->object, indicator->image_src);
        if (indicator->recolor_image) {
            lv_obj_set_style_image_recolor(indicator->object,
                                           indicator->warning_color,
                                           LV_PART_MAIN);
            lv_obj_set_style_image_recolor_opa(indicator->object,
                                               LV_OPA_COVER,
                                               LV_PART_MAIN);
        }
    } else {
        indicator->object = lv_obj_create(lv_screen_active());
        lv_obj_set_size(indicator->object, indicator->size, indicator->size);
        lv_obj_set_style_radius(indicator->object, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_border_width(indicator->object, 0, LV_PART_MAIN);
    }
    lv_obj_set_style_pad_all(indicator->object, 0, LV_PART_MAIN);
    lv_obj_align(indicator->object, indicator->align, indicator->x, indicator->y);
    lv_obj_add_flag(indicator->object, LV_OBJ_FLAG_HIDDEN);
    indicator->hidden = true;
    indicator->dirty = true;
}

static void update_needle(void)
{
    const int64_t now_us = esp_timer_get_time();
    if (s_last_update_us == 0) {
        s_last_update_us = now_us;
        return;
    }
    int64_t elapsed_us = now_us - s_last_update_us;
    s_last_update_us = now_us;
    if (elapsed_us <= 0) {
        return;
    }
    if (elapsed_us > 250000) {
        elapsed_us = 250000;
    }

    float target;
    float displayed;
    float coefficient;
    uint32_t generation;
    portENTER_CRITICAL(&s_state_lock);
    target = s_gauge_state.target_value;
    displayed = s_gauge_state.displayed_value;
    coefficient = s_smoothing_coefficient;
    generation = s_state_generation;
    portEXIT_CRITICAL(&s_state_lock);

    const float frame_fraction = (float)elapsed_us * 30.0f / 1000000.0f;
    const float alpha = 1.0f - powf(1.0f - coefficient, frame_fraction);
    float next_value = displayed + alpha * (target - displayed);
    if (fabsf(target - next_value) < 0.01f) {
        next_value = target;
    }

    portENTER_CRITICAL(&s_state_lock);
    if (generation == s_state_generation) {
        s_gauge_state.displayed_value = next_value;
    } else {
        next_value = s_gauge_state.displayed_value;
    }
    portEXIT_CRITICAL(&s_state_lock);

    const int32_t rotation =
        (int32_t)lroundf((90.0f - gauge_value_to_angle(next_value)) * 10.0f);
    if (rotation != s_last_needle_rotation) {
        lv_image_set_rotation(s_needle_object, rotation);
        s_last_needle_rotation = rotation;
    }
}

static void update_text_widgets(void)
{
    for (gauge_text_t *text = s_text_widgets; text != NULL; text = text->next) {
        if (text->label == NULL) {
            create_text_object(text);
        } else if (text->dirty) {
            memcpy(text->rendered_value, text->pending_value, sizeof(text->rendered_value));
            lv_label_set_text_static(text->label, text->rendered_value);
            text->dirty = false;
        }

        const bool hidden = !text->visible;
        const bool currently_hidden = lv_obj_has_flag(text->label, LV_OBJ_FLAG_HIDDEN);
        if (hidden != currently_hidden) {
            if (hidden) {
                lv_obj_add_flag(text->label, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_clear_flag(text->label, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

static void update_warning_widgets(int64_t now_us)
{
    for (gauge_warning_indicator_t *indicator = s_warning_widgets;
         indicator != NULL;
         indicator = indicator->next) {
        if (indicator->object == NULL) {
            create_warning_object(indicator);
        }

        const bool hidden = !indicator->visible ||
                            indicator->state == GAUGE_WARNING_CLEAR;
        if (hidden != indicator->hidden) {
            if (hidden) {
                lv_obj_add_flag(indicator->object, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_clear_flag(indicator->object, LV_OBJ_FLAG_HIDDEN);
            }
            indicator->hidden = hidden;
        }

        if (indicator->dirty) {
            const lv_color_t color = indicator->state == GAUGE_WARNING_CRITICAL ?
                                     indicator->critical_color : indicator->warning_color;
            if (indicator->image_src != NULL) {
                if (indicator->recolor_image) {
                    lv_obj_set_style_image_recolor(indicator->object, color, LV_PART_MAIN);
                }
            } else {
                lv_obj_set_style_bg_color(indicator->object, color, LV_PART_MAIN);
            }
            indicator->dirty = false;
            indicator->blink_phase_valid = false;
        }

        bool blink_on = true;
        if (indicator->blink && !hidden) {
            const uint32_t interval_ms = indicator->blink_interval_ms == 0 ?
                                         GAUGE_WARNING_DEFAULT_BLINK_INTERVAL_MS :
                                         indicator->blink_interval_ms;
            blink_on = ((now_us / ((int64_t)interval_ms * 1000)) & 1) == 0;
        }
        if (!indicator->blink_phase_valid || indicator->blink_phase != blink_on) {
            lv_obj_set_style_opa(indicator->object,
                                 blink_on ? LV_OPA_COVER : LV_OPA_TRANSP,
                                 LV_PART_MAIN);
            indicator->blink_phase = blink_on;
            indicator->blink_phase_valid = true;
        }
    }
}

static void renderer_timer_callback(lv_timer_t *timer)
{
    (void)timer;
    if (!s_scene_created) {
        create_scene();
        s_last_needle_rotation = -1;
    }

    update_needle();
    if (xSemaphoreTake(s_widget_lock, portMAX_DELAY) == pdTRUE) {
        update_text_widgets();
        update_warning_widgets(esp_timer_get_time());
        xSemaphoreGive(s_widget_lock);
    }
}

#if GAUGE_PERFORMANCE_DIAGNOSTICS
static void performance_event_callback(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    const int64_t now_us = esp_timer_get_time();
    if (code == LV_EVENT_RENDER_START) {
        s_render_start_us = now_us;
        return;
    }
    if (code == LV_EVENT_RENDER_READY && s_render_start_us != 0) {
        s_render_total_us += now_us - s_render_start_us;
        s_render_count++;
        s_render_start_us = 0;
        return;
    }
    if (code != LV_EVENT_REFR_READY) {
        return;
    }

    s_refresh_count++;
    if (s_performance_window_start_us == 0) {
        s_performance_window_start_us = now_us;
        return;
    }
    const int64_t elapsed_us = now_us - s_performance_window_start_us;
    if (elapsed_us < 5000000) {
        return;
    }

    const uint32_t refresh_hz = (uint32_t)((uint64_t)s_refresh_count * 1000000U /
                                           (uint64_t)elapsed_us);
    const uint32_t render_avg_us = s_render_count == 0 ? 0 :
                                   (uint32_t)(s_render_total_us / s_render_count);
    const size_t framebuffer_bytes =
        (size_t)NEWHAVEN_DISPLAY_H_RES * NEWHAVEN_DISPLAY_V_RES *
        GAUGE_RENDERER_DISPLAY_BYTES_PER_PIXEL *
        NEWHAVEN_DISPLAY_FB_COUNT;
    ESP_LOGI(TAG, "LVGL perf: refresh=%" PRIu32 " Hz render_avg=%" PRIu32
             " us internal_free=%u PSRAM_free=%u framebuffers=%u bytes; "
             "per-core CPU utilization unavailable with current SMP runtime-stat config",
             refresh_hz, render_avg_us,
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned int)framebuffer_bytes);
    s_performance_window_start_us = now_us;
    s_render_total_us = 0;
    s_render_count = 0;
    s_refresh_count = 0;
}
#endif

esp_err_t gauge_renderer_init(esp_lcd_panel_handle_t dpi_panel,
                              const uint8_t *background_rgb565,
                              size_t background_size,
                              const uint8_t *needle_rgba8888,
                              size_t needle_size)
{
    if (dpi_panel == NULL || background_rgb565 == NULL ||
        background_size != GAUGE_RENDERER_BACKGROUND_SIZE ||
        needle_rgba8888 == NULL || needle_size != GAUGE_RENDERER_NEEDLE_SIZE) {
        ESP_LOGE(TAG, "Display handle or embedded gauge asset has an invalid size");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_initialized) {
        ESP_LOGE(TAG, "Renderer is already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    s_needle_pixels = heap_caps_malloc(GAUGE_RENDERER_NEEDLE_SIZE,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_needle_pixels == NULL) {
        ESP_LOGE(TAG, "Unable to allocate the transformed needle image in PSRAM");
        return ESP_ERR_NO_MEM;
    }
    for (size_t pixel = 0; pixel < GAUGE_RENDERER_NEEDLE_SPRITE_WIDTH *
                                     GAUGE_RENDERER_NEEDLE_SPRITE_HEIGHT;
         pixel++) {
        const uint8_t *source = needle_rgba8888 + pixel * 4;
        uint8_t *destination = s_needle_pixels + pixel * 4;
        const uint16_t alpha = source[3];
        destination[0] = (uint8_t)((source[2] * alpha + 127U) / 255U);
        destination[1] = (uint8_t)((source[1] * alpha + 127U) / 255U);
        destination[2] = (uint8_t)((source[0] * alpha + 127U) / 255U);
        destination[3] = source[3];
    }

    s_background_image = (lv_image_dsc_t) {
        .header = {
            .magic = LV_IMAGE_HEADER_MAGIC,
            .cf = LV_COLOR_FORMAT_RGB565_SWAPPED,
            .w = NEWHAVEN_DISPLAY_H_RES,
            .h = NEWHAVEN_DISPLAY_V_RES,
            .stride = NEWHAVEN_DISPLAY_H_RES * 2,
        },
        .data_size = background_size,
        .data = background_rgb565,
    };
    s_needle_image = (lv_image_dsc_t) {
        .header = {
            .magic = LV_IMAGE_HEADER_MAGIC,
            .cf = LV_COLOR_FORMAT_ARGB8888_PREMULTIPLIED,
            .flags = LV_IMAGE_FLAGS_PREMULTIPLIED,
            .w = GAUGE_RENDERER_NEEDLE_SPRITE_WIDTH,
            .h = GAUGE_RENDERER_NEEDLE_SPRITE_HEIGHT,
            .stride = GAUGE_RENDERER_NEEDLE_SPRITE_WIDTH * 4,
        },
        .data_size = GAUGE_RENDERER_NEEDLE_SIZE,
        .data = s_needle_pixels,
    };

    s_widget_lock = xSemaphoreCreateMutex();
    if (s_widget_lock == NULL) {
        ESP_LOGE(TAG, "Unable to create the graphics model mutex");
        heap_caps_free(s_needle_pixels);
        s_needle_pixels = NULL;
        return ESP_ERR_NO_MEM;
    }

    lvgl_port_cfg_t lvgl_config = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_config.task_affinity = 1;
    lvgl_config.timer_period_ms = 5;
    const esp_err_t init_err = lvgl_port_init(&lvgl_config);
    if (init_err != ESP_OK) {
        ESP_LOGE(TAG, "LVGL port initialization failed: %s", esp_err_to_name(init_err));
        heap_caps_free(s_needle_pixels);
        s_needle_pixels = NULL;
        vSemaphoreDelete(s_widget_lock);
        s_widget_lock = NULL;
        return init_err;
    }

    const lvgl_port_display_cfg_t display_config = {
        .panel_handle = dpi_panel,
        .buffer_size = NEWHAVEN_DISPLAY_H_RES * NEWHAVEN_DISPLAY_V_RES,
        .double_buffer = true,
        .hres = NEWHAVEN_DISPLAY_H_RES,
        .vres = NEWHAVEN_DISPLAY_V_RES,
        .color_format = GAUGE_RENDERER_DISPLAY_COLOR_FORMAT,
        .flags = {
            .direct_mode = true,
        },
    };
    const lvgl_port_display_dsi_cfg_t dsi_config = {
        .flags = {
            .avoid_tearing = true,
        },
    };
    s_lvgl_display = lvgl_port_add_disp_dsi(&display_config, &dsi_config);
    if (s_lvgl_display == NULL) {
        ESP_LOGE(TAG, "Unable to connect LVGL to the MIPI DSI panel");
        (void)lvgl_port_deinit();
        heap_caps_free(s_needle_pixels);
        s_needle_pixels = NULL;
        vSemaphoreDelete(s_widget_lock);
        s_widget_lock = NULL;
        return ESP_FAIL;
    }
    portENTER_CRITICAL(&s_state_lock);
    s_gauge_state = (gauge_state_t) {
        .target_value = GAUGE_RENDERER_MIN_PSI,
        .displayed_value = GAUGE_RENDERER_MIN_PSI,
        .min_value = GAUGE_RENDERER_MIN_PSI,
        .max_value = GAUGE_RENDERER_MAX_PSI,
        .start_angle_deg = GAUGE_RENDERER_START_ANGLE_DEG,
        .end_angle_deg = GAUGE_RENDERER_END_ANGLE_DEG,
    };
    s_state_generation = 0;
    portEXIT_CRITICAL(&s_state_lock);

    if (!lvgl_port_lock(portMAX_DELAY)) {
        ESP_LOGE(TAG, "Unable to acquire the LVGL lock to create the renderer timer");
        (void)lvgl_port_remove_disp(s_lvgl_display);
        s_lvgl_display = NULL;
        (void)lvgl_port_deinit();
        heap_caps_free(s_needle_pixels);
        s_needle_pixels = NULL;
        vSemaphoreDelete(s_widget_lock);
        s_widget_lock = NULL;
        return ESP_ERR_TIMEOUT;
    }
#if GAUGE_PERFORMANCE_DIAGNOSTICS
    lv_display_add_event_cb(s_lvgl_display, performance_event_callback,
                            LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(s_lvgl_display, performance_event_callback,
                            LV_EVENT_RENDER_READY, NULL);
    lv_display_add_event_cb(s_lvgl_display, performance_event_callback,
                            LV_EVENT_REFR_READY, NULL);
#endif
    s_renderer_timer = lv_timer_create(renderer_timer_callback,
                                       GAUGE_RENDERER_TIMER_PERIOD_MS,
                                       NULL);
    s_initialized = s_renderer_timer != NULL;
    lvgl_port_unlock();
    if (s_renderer_timer == NULL) {
        ESP_LOGE(TAG, "Unable to create the LVGL gauge update timer");
        (void)lvgl_port_remove_disp(s_lvgl_display);
        s_lvgl_display = NULL;
        (void)lvgl_port_deinit();
        heap_caps_free(s_needle_pixels);
        s_needle_pixels = NULL;
        vSemaphoreDelete(s_widget_lock);
        s_widget_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "LVGL display configured for %dx%d %s direct-mode DSI framebuffers",
             NEWHAVEN_DISPLAY_H_RES, NEWHAVEN_DISPLAY_V_RES,
             GAUGE_RENDERER_DISPLAY_FORMAT_NAME);
    return ESP_OK;
}

void gauge_renderer_set_value(float value)
{
    if (!s_initialized || !isfinite(value)) {
        return;
    }
    portENTER_CRITICAL(&s_state_lock);
    s_gauge_state.target_value = clamp_value(value);
    s_state_generation++;
    portEXIT_CRITICAL(&s_state_lock);
}

void gauge_renderer_set_value_immediately(float value)
{
    if (!s_initialized || !isfinite(value)) {
        return;
    }
    const float clamped = clamp_value(value);
    portENTER_CRITICAL(&s_state_lock);
    s_gauge_state.target_value = clamped;
    s_gauge_state.displayed_value = clamped;
    s_state_generation++;
    portEXIT_CRITICAL(&s_state_lock);
}

esp_err_t gauge_renderer_set_smoothing_coefficient(float coefficient)
{
    if (!isfinite(coefficient) || coefficient <= 0.0f || coefficient > 1.0f) {
        ESP_LOGE(TAG, "Smoothing coefficient must be greater than zero and at most one");
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_state_lock);
    s_smoothing_coefficient = coefficient;
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

float gauge_value_to_angle(float value)
{
    const float clamped = clamp_value(value);
    const float fraction = (clamped - GAUGE_RENDERER_MIN_PSI) /
                           (GAUGE_RENDERER_MAX_PSI - GAUGE_RENDERER_MIN_PSI);
    return GAUGE_RENDERER_START_ANGLE_DEG +
           fraction * (GAUGE_RENDERER_END_ANGLE_DEG - GAUGE_RENDERER_START_ANGLE_DEG);
}

esp_err_t gauge_renderer_get_state(gauge_state_t *state)
{
    if (state == NULL) {
        ESP_LOGE(TAG, "Gauge state output pointer is null");
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_state_lock);
    *state = s_gauge_state;
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

static esp_err_t set_text_value(gauge_text_t *text, const char *value)
{
    if (text == NULL || value == NULL) {
        ESP_LOGE(TAG, "Text widget or value is null");
        return ESP_ERR_INVALID_ARG;
    }
    const size_t length = strlen(value);
    if (length >= sizeof(text->pending_value)) {
        ESP_LOGE(TAG, "Text exceeds the fixed %u-byte widget buffer",
                 (unsigned int)(sizeof(text->pending_value) - 1));
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_widget_lock == NULL ||
        xSemaphoreTake(s_widget_lock, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Unable to acquire the graphics model mutex");
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(text->pending_value, value, length + 1);
    text->dirty = true;
    xSemaphoreGive(s_widget_lock);
    return ESP_OK;
}

esp_err_t gauge_text_create(const gauge_text_config_t *config, gauge_text_t **text)
{
    if (config == NULL || text == NULL ||
        (config->units != NULL && strlen(config->units) >= GAUGE_TEXT_UNITS_CAPACITY)) {
        ESP_LOGE(TAG, "Text widget configuration is invalid");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_widget_lock == NULL ||
        xSemaphoreTake(s_widget_lock, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Graphics model is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    gauge_text_t *widget = calloc(1, sizeof(*widget));
    if (widget == NULL) {
        xSemaphoreGive(s_widget_lock);
        ESP_LOGE(TAG, "Unable to allocate a text widget model");
        return ESP_ERR_NO_MEM;
    }
    widget->font = config->font == NULL ? LV_FONT_DEFAULT : config->font;
    widget->align = config->align;
    widget->text_align = config->text_align;
    widget->color = config->color;
    widget->x = config->x;
    widget->y = config->y;
    widget->width = config->width;
    widget->visible = config->visible;
    widget->dirty = true;
    if (config->units != NULL) {
        (void)snprintf(widget->units, sizeof(widget->units), "%s", config->units);
    }
    widget->next = s_text_widgets;
    s_text_widgets = widget;
    *text = widget;
    xSemaphoreGive(s_widget_lock);
    return ESP_OK;
}

esp_err_t gauge_text_set_text(gauge_text_t *text, const char *value)
{
    return set_text_value(text, value);
}

esp_err_t gauge_text_set_integer(gauge_text_t *text, int32_t value)
{
    if (text == NULL) {
        ESP_LOGE(TAG, "Text widget is null");
        return ESP_ERR_INVALID_ARG;
    }
    char formatted[GAUGE_TEXT_CAPACITY];
    const int written = text->units[0] == '\0' ?
                        snprintf(formatted, sizeof(formatted), "%ld", (long)value) :
                        snprintf(formatted, sizeof(formatted), "%ld %s",
                                 (long)value, text->units);
    if (written < 0 || (size_t)written >= sizeof(formatted)) {
        ESP_LOGE(TAG, "Integer text formatting exceeded its fixed buffer");
        return ESP_ERR_INVALID_SIZE;
    }
    return set_text_value(text, formatted);
}

esp_err_t gauge_text_set_float(gauge_text_t *text, float value, uint8_t decimals)
{
    if (text == NULL || !isfinite(value) || decimals > 6) {
        ESP_LOGE(TAG, "Text widget, floating-point value, or precision is invalid");
        return ESP_ERR_INVALID_ARG;
    }
    char formatted[GAUGE_TEXT_CAPACITY];
    const int written = text->units[0] == '\0' ?
                        snprintf(formatted, sizeof(formatted), "%.*f", decimals, (double)value) :
                        snprintf(formatted, sizeof(formatted), "%.*f %s",
                                 decimals, (double)value, text->units);
    if (written < 0 || (size_t)written >= sizeof(formatted)) {
        ESP_LOGE(TAG, "Floating-point text formatting exceeded its fixed buffer");
        return ESP_ERR_INVALID_SIZE;
    }
    return set_text_value(text, formatted);
}

esp_err_t gauge_text_set_visible(gauge_text_t *text, bool visible)
{
    if (text == NULL || s_widget_lock == NULL) {
        ESP_LOGE(TAG, "Text widget or graphics model is unavailable");
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_widget_lock, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Unable to acquire the graphics model mutex");
        return ESP_ERR_INVALID_STATE;
    }
    text->visible = visible;
    xSemaphoreGive(s_widget_lock);
    return ESP_OK;
}

esp_err_t gauge_warning_indicator_create(const gauge_warning_config_t *config,
                                         gauge_warning_indicator_t **indicator)
{
    if (config == NULL || indicator == NULL) {
        ESP_LOGE(TAG, "Warning indicator configuration or output is null");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_widget_lock == NULL ||
        xSemaphoreTake(s_widget_lock, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Graphics model is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    gauge_warning_indicator_t *widget = calloc(1, sizeof(*widget));
    if (widget == NULL) {
        xSemaphoreGive(s_widget_lock);
        ESP_LOGE(TAG, "Unable to allocate a warning indicator model");
        return ESP_ERR_NO_MEM;
    }
    widget->image_src = config->image_src;
    widget->align = config->align;
    widget->warning_color = config->warning_color;
    widget->critical_color = config->critical_color;
    widget->blink_interval_ms = config->blink_interval_ms;
    widget->x = config->x;
    widget->y = config->y;
    widget->size = config->size == 0 ? 16 : config->size;
    widget->recolor_image = config->recolor_image;
    widget->visible = config->visible;
    widget->blink = config->blink;
    widget->state = GAUGE_WARNING_CLEAR;
    widget->dirty = true;
    widget->next = s_warning_widgets;
    s_warning_widgets = widget;
    *indicator = widget;
    xSemaphoreGive(s_widget_lock);
    return ESP_OK;
}

esp_err_t gauge_warning_indicator_set_state(gauge_warning_indicator_t *indicator,
                                            gauge_warning_state_t state)
{
    if (indicator == NULL || state < GAUGE_WARNING_CLEAR || state > GAUGE_WARNING_CRITICAL ||
        s_widget_lock == NULL) {
        ESP_LOGE(TAG, "Warning indicator or state is invalid");
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_widget_lock, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Unable to acquire the graphics model mutex");
        return ESP_ERR_INVALID_STATE;
    }
    indicator->state = state;
    indicator->dirty = true;
    xSemaphoreGive(s_widget_lock);
    return ESP_OK;
}

esp_err_t gauge_warning_indicator_set_visible(gauge_warning_indicator_t *indicator,
                                              bool visible)
{
    if (indicator == NULL || s_widget_lock == NULL) {
        ESP_LOGE(TAG, "Warning indicator or graphics model is unavailable");
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_widget_lock, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Unable to acquire the graphics model mutex");
        return ESP_ERR_INVALID_STATE;
    }
    indicator->visible = visible;
    xSemaphoreGive(s_widget_lock);
    return ESP_OK;
}

esp_err_t gauge_warning_indicator_set_blink(gauge_warning_indicator_t *indicator,
                                            bool blink,
                                            uint32_t interval_ms)
{
    if (indicator == NULL || (blink && interval_ms == 0) || s_widget_lock == NULL) {
        ESP_LOGE(TAG, "Warning blink configuration is invalid");
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_widget_lock, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Unable to acquire the graphics model mutex");
        return ESP_ERR_INVALID_STATE;
    }
    indicator->blink = blink;
    indicator->blink_interval_ms = interval_ms;
    indicator->blink_phase_valid = false;
    xSemaphoreGive(s_widget_lock);
    return ESP_OK;
}

esp_err_t gauge_warning_indicator_set_colors(gauge_warning_indicator_t *indicator,
                                             lv_color_t warning_color,
                                             lv_color_t critical_color)
{
    if (indicator == NULL || s_widget_lock == NULL) {
        ESP_LOGE(TAG, "Warning indicator or graphics model is unavailable");
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_widget_lock, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Unable to acquire the graphics model mutex");
        return ESP_ERR_INVALID_STATE;
    }
    indicator->warning_color = warning_color;
    indicator->critical_color = critical_color;
    indicator->dirty = true;
    xSemaphoreGive(s_widget_lock);
    return ESP_OK;
}
