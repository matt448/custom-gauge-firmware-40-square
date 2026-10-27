#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "display_config.h"
#include "gauge_renderer.h"

#define GAUGE_RENDERER_DEGREES_TO_RADIANS (3.14159265358979323846f / 180.0f)
#define GAUGE_RENDERER_BACKGROUND_SIZE     \
    (NEWHAVEN_DISPLAY_H_RES * NEWHAVEN_DISPLAY_V_RES * 2)
#define GAUGE_RENDERER_NEEDLE_SPRITE_SIZE \
    (GAUGE_RENDERER_NEEDLE_SPRITE_WIDTH * GAUGE_RENDERER_NEEDLE_SPRITE_HEIGHT * 4)

typedef struct {
    void *framebuffer;
    float angle_deg;
    bool has_needle;
} gauge_frame_history_t;

static gauge_state_t s_gauge_state;
static gauge_frame_history_t s_frame_history[NEWHAVEN_DISPLAY_FB_COUNT];
static const uint8_t *s_background_rgb565;
static const uint8_t *s_needle_rgba8888;
static bool s_initialized;

static float gauge_clamp_value(float value)
{
    if (value < s_gauge_state.min_value) {
        return s_gauge_state.min_value;
    }
    if (value > s_gauge_state.max_value) {
        return s_gauge_state.max_value;
    }
    return value;
}

static void gauge_map_coordinate(int x, int y, int *mapped_x, int *mapped_y)
{
    switch (NEWHAVEN_DISPLAY_ROTATION) {
    case NEWHAVEN_DISPLAY_ROTATION_0:
        *mapped_x = x;
        *mapped_y = y;
        break;
    case NEWHAVEN_DISPLAY_ROTATION_90:
        *mapped_x = NEWHAVEN_DISPLAY_H_RES - 1 - y;
        *mapped_y = x;
        break;
    case NEWHAVEN_DISPLAY_ROTATION_180:
        *mapped_x = NEWHAVEN_DISPLAY_H_RES - 1 - x;
        *mapped_y = NEWHAVEN_DISPLAY_V_RES - 1 - y;
        break;
    case NEWHAVEN_DISPLAY_ROTATION_270:
        *mapped_x = y;
        *mapped_y = NEWHAVEN_DISPLAY_V_RES - 1 - x;
        break;
    default:
        *mapped_x = x;
        *mapped_y = y;
        break;
    }
}

static void gauge_put_pixel(void *framebuffer, int x, int y, uint32_t rgb)
{
    if (x < 0 || x >= NEWHAVEN_DISPLAY_H_RES || y < 0 || y >= NEWHAVEN_DISPLAY_V_RES) {
        return;
    }

    int mapped_x;
    int mapped_y;
    gauge_map_coordinate(x, y, &mapped_x, &mapped_y);

#if NEWHAVEN_DISPLAY_PIXEL_FORMAT == NEWHAVEN_DISPLAY_PIXEL_FORMAT_RGB888
    uint8_t *pixel = (uint8_t *)framebuffer +
                     ((mapped_y * NEWHAVEN_DISPLAY_H_RES + mapped_x) * 3);
    const uint8_t red = (rgb >> 16) & 0xff;
    const uint8_t green = (rgb >> 8) & 0xff;
    const uint8_t blue = rgb & 0xff;
#if NEWHAVEN_DISPLAY_COLOR_ORDER == NEWHAVEN_DISPLAY_COLOR_ORDER_RGB
    pixel[0] = blue;
    pixel[1] = green;
    pixel[2] = red;
#else
    pixel[0] = red;
    pixel[1] = green;
    pixel[2] = blue;
#endif
#else
    uint16_t red = (rgb >> 19) & 0x1f;
    uint16_t green = (rgb >> 10) & 0x3f;
    uint16_t blue = (rgb >> 3) & 0x1f;
#if NEWHAVEN_DISPLAY_COLOR_ORDER == NEWHAVEN_DISPLAY_COLOR_ORDER_BGR
    const uint16_t temporary = red;
    red = blue;
    blue = temporary;
#endif
    ((uint16_t *)framebuffer)[mapped_y * NEWHAVEN_DISPLAY_H_RES + mapped_x] =
        (red << 11) | (green << 5) | blue;
#endif
}

static uint32_t gauge_read_background_pixel(int x, int y)
{
    const size_t offset = ((size_t)y * NEWHAVEN_DISPLAY_H_RES + x) * 2;
    const uint16_t pixel = ((uint16_t)s_background_rgb565[offset] << 8) |
                           s_background_rgb565[offset + 1];
    const uint8_t red = (pixel >> 11) & 0x1f;
    const uint8_t green = (pixel >> 5) & 0x3f;
    const uint8_t blue = pixel & 0x1f;
    return ((uint32_t)((red << 3) | (red >> 2)) << 16) |
           ((uint32_t)((green << 2) | (green >> 4)) << 8) |
           ((uint32_t)((blue << 3) | (blue >> 2)));
}

static void gauge_sprite_coordinate(int center_x,
                                   int center_y,
                                   float cosine,
                                   float sine,
                                   int source_x,
                                   int source_y,
                                   int *destination_x,
                                   int *destination_y)
{
    const int perpendicular_offset = source_x - GAUGE_RENDERER_NEEDLE_PIVOT_X;
    const int forward_offset = GAUGE_RENDERER_NEEDLE_PIVOT_Y - source_y;

    *destination_x = center_x + (int)lroundf(cosine * forward_offset +
                                             sine * perpendicular_offset);
    *destination_y = center_y + (int)lroundf(-sine * forward_offset +
                                             cosine * perpendicular_offset);
}

static void gauge_get_sprite_bounds(int center_x,
                                    int center_y,
                                    float cosine,
                                    float sine,
                                    int *min_x,
                                    int *max_x,
                                    int *min_y,
                                    int *max_y)
{
    const int source_x[] = {0, GAUGE_RENDERER_NEEDLE_SPRITE_WIDTH - 1};
    const int source_y[] = {0, GAUGE_RENDERER_NEEDLE_SPRITE_HEIGHT - 1};

    *min_x = NEWHAVEN_DISPLAY_H_RES - 1;
    *max_x = 0;
    *min_y = NEWHAVEN_DISPLAY_V_RES - 1;
    *max_y = 0;

    for (size_t y_index = 0; y_index < sizeof(source_y) / sizeof(source_y[0]); y_index++) {
        for (size_t x_index = 0; x_index < sizeof(source_x) / sizeof(source_x[0]); x_index++) {
            int x;
            int y;

            gauge_sprite_coordinate(center_x, center_y, cosine, sine,
                                    source_x[x_index], source_y[y_index], &x, &y);
            if (x < *min_x) {
                *min_x = x;
            }
            if (x > *max_x) {
                *max_x = x;
            }
            if (y < *min_y) {
                *min_y = y;
            }
            if (y > *max_y) {
                *max_y = y;
            }
        }
    }
}

static void gauge_restore_needle_area(void *framebuffer,
                                     int center_x,
                                     int center_y,
                                     float angle_deg)
{
    const float angle_rad = angle_deg * GAUGE_RENDERER_DEGREES_TO_RADIANS;
    const float cosine = cosf(angle_rad);
    const float sine = sinf(angle_rad);
    int min_x;
    int max_x;
    int min_y;
    int max_y;

    gauge_get_sprite_bounds(center_x, center_y, cosine, sine,
                            &min_x, &max_x, &min_y, &max_y);

    for (int y = min_y - 1; y <= max_y + 1; y++) {
        for (int x = min_x - 1; x <= max_x + 1; x++) {
            if (x >= 0 && x < NEWHAVEN_DISPLAY_H_RES &&
                y >= 0 && y < NEWHAVEN_DISPLAY_V_RES) {
                gauge_put_pixel(framebuffer, x, y, gauge_read_background_pixel(x, y));
            }
        }
    }
}

static void gauge_draw_needle_sprite(void *framebuffer,
                                    int center_x,
                                    int center_y,
                                    float angle_deg)
{
    const float angle_rad = angle_deg * GAUGE_RENDERER_DEGREES_TO_RADIANS;
    const float cosine = cosf(angle_rad);
    const float sine = sinf(angle_rad);
    int min_x;
    int max_x;
    int min_y;
    int max_y;

    gauge_get_sprite_bounds(center_x, center_y, cosine, sine,
                            &min_x, &max_x, &min_y, &max_y);

    for (int destination_y = min_y - 1; destination_y <= max_y + 1; destination_y++) {
        for (int destination_x = min_x - 1; destination_x <= max_x + 1; destination_x++) {
            const int horizontal_offset = destination_x - center_x;
            const int vertical_offset = destination_y - center_y;
            const int forward_offset =
                (int)lroundf(cosine * horizontal_offset - sine * vertical_offset);
            const int perpendicular_offset =
                (int)lroundf(sine * horizontal_offset + cosine * vertical_offset);
            const int source_x = perpendicular_offset + GAUGE_RENDERER_NEEDLE_PIVOT_X;
            const int source_y = GAUGE_RENDERER_NEEDLE_PIVOT_Y - forward_offset;

            if (destination_x < 0 || destination_x >= NEWHAVEN_DISPLAY_H_RES ||
                destination_y < 0 || destination_y >= NEWHAVEN_DISPLAY_V_RES ||
                source_x < 0 || source_x >= GAUGE_RENDERER_NEEDLE_SPRITE_WIDTH ||
                source_y < 0 || source_y >= GAUGE_RENDERER_NEEDLE_SPRITE_HEIGHT) {
                continue;
            }

            const size_t offset =
                ((size_t)source_y * GAUGE_RENDERER_NEEDLE_SPRITE_WIDTH + source_x) * 4;
            const uint8_t source_alpha = s_needle_rgba8888[offset + 3];
            if (source_alpha == 0) {
                continue;
            }

            const uint32_t background = gauge_read_background_pixel(destination_x, destination_y);
            const uint32_t inverse_alpha = 255 - source_alpha;
            const uint32_t red = (s_needle_rgba8888[offset] * source_alpha +
                                 ((background >> 16) & 0xff) * inverse_alpha) / 255;
            const uint32_t green = (s_needle_rgba8888[offset + 1] * source_alpha +
                                   ((background >> 8) & 0xff) * inverse_alpha) / 255;
            const uint32_t blue = (s_needle_rgba8888[offset + 2] * source_alpha +
                                  (background & 0xff) * inverse_alpha) / 255;
            gauge_put_pixel(framebuffer, destination_x, destination_y,
                           (red << 16) | (green << 8) | blue);
        }
    }
}

static gauge_frame_history_t *gauge_get_frame_history(void *framebuffer)
{
    for (size_t index = 0; index < NEWHAVEN_DISPLAY_FB_COUNT; index++) {
        if (s_frame_history[index].framebuffer == framebuffer) {
            return &s_frame_history[index];
        }
    }
    for (size_t index = 0; index < NEWHAVEN_DISPLAY_FB_COUNT; index++) {
        if (s_frame_history[index].framebuffer == NULL) {
            s_frame_history[index].framebuffer = framebuffer;
            return &s_frame_history[index];
        }
    }
    return NULL;
}

esp_err_t gauge_renderer_init(const uint8_t *background_rgb565,
                              size_t background_size,
                              const uint8_t *needle_rgba8888,
                              size_t needle_size)
{
    if (background_rgb565 == NULL || background_size != GAUGE_RENDERER_BACKGROUND_SIZE ||
        needle_rgba8888 == NULL || needle_size != GAUGE_RENDERER_NEEDLE_SPRITE_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }

    s_gauge_state = (gauge_state_t) {
        .target_value = 0.0f,
        .displayed_value = 0.0f,
        .min_value = GAUGE_RENDERER_MIN_PSI,
        .max_value = GAUGE_RENDERER_MAX_PSI,
        .start_angle_deg = GAUGE_RENDERER_START_ANGLE_DEG,
        .end_angle_deg = GAUGE_RENDERER_END_ANGLE_DEG,
    };
    for (size_t index = 0; index < NEWHAVEN_DISPLAY_FB_COUNT; index++) {
        s_frame_history[index] = (gauge_frame_history_t) {};
    }
    s_background_rgb565 = background_rgb565;
    s_needle_rgba8888 = needle_rgba8888;
    s_initialized = true;
    return ESP_OK;
}

void gauge_renderer_set_value(float value)
{
    if (!s_initialized) {
        return;
    }
    s_gauge_state.target_value = gauge_clamp_value(value);
}

void gauge_renderer_set_value_immediately(float value)
{
    if (!s_initialized) {
        return;
    }

    const float clamped_value = gauge_clamp_value(value);
    s_gauge_state.target_value = clamped_value;
    s_gauge_state.displayed_value = clamped_value;
}

float gauge_value_to_angle(float value)
{
    const float value_range = s_gauge_state.max_value - s_gauge_state.min_value;
    if (value_range <= 0.0f) {
        return s_gauge_state.start_angle_deg;
    }

    const float clamped_value = gauge_clamp_value(value);
    return s_gauge_state.start_angle_deg +
           ((clamped_value - s_gauge_state.min_value) / value_range) *
               (s_gauge_state.end_angle_deg - s_gauge_state.start_angle_deg);
}

const gauge_state_t *gauge_renderer_get_state(void)
{
    return &s_gauge_state;
}

esp_err_t gauge_renderer_render(void *framebuffer)
{
    if (!s_initialized || framebuffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    gauge_frame_history_t *frame_history = gauge_get_frame_history(framebuffer);
    if (frame_history == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (frame_history->has_needle) {
        gauge_restore_needle_area(framebuffer,
                                  GAUGE_RENDERER_CENTER_X,
                                  GAUGE_RENDERER_CENTER_Y,
                                  frame_history->angle_deg);
    }

    s_gauge_state.displayed_value += GAUGE_RENDERER_SMOOTHING_COEFFICIENT *
                                     (s_gauge_state.target_value - s_gauge_state.displayed_value);
    frame_history->angle_deg = gauge_value_to_angle(s_gauge_state.displayed_value);
    gauge_draw_needle_sprite(framebuffer,
                             GAUGE_RENDERER_CENTER_X,
                             GAUGE_RENDERER_CENTER_Y,
                             frame_history->angle_deg);
    frame_history->has_needle = true;
    return ESP_OK;
}
