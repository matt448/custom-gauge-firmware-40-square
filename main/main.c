#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "backlight.h"
#include "display_config.h"
#include "gauge_renderer.h"
#include "newhaven_display.h"
#include "newhaven_display_diagnostics.h"

static const char *TAG = "main";

extern const uint8_t oil_pressure_gauge_rgb565_start[] asm("_binary_oil_pressure_gauge_rgb565_start");
extern const uint8_t oil_pressure_gauge_rgb565_end[] asm("_binary_oil_pressure_gauge_rgb565_end");
extern const uint8_t needle_rgba8888_start[] asm("_binary_needle_rgba8888_start");
extern const uint8_t needle_rgba8888_end[] asm("_binary_needle_rgba8888_end");

#define GAUGE_FRAME_RATE_HZ 30
#define GAUGE_SWEEP_DURATION_MS 1500
#define GAUGE_CYCLE_DURATION_MS (GAUGE_SWEEP_DURATION_MS * 2)
#define GAUGE_LOG_INTERVAL_MS 1000

#if NEWHAVEN_DISPLAY_DIAGNOSTICS && \
    ((NEWHAVEN_DISPLAY_HARDWARE_PATTERN_MS > 1000) || (NEWHAVEN_DISPLAY_STARTUP_COLOR_TEST_MS > 0))
static void log_refresh_progress(const char *stage, uint32_t before, TickType_t start)
{
    const uint32_t after = newhaven_display_refresh_count();
    const unsigned int elapsed_ms =
        (unsigned int)((xTaskGetTickCount() - start) * portTICK_PERIOD_MS);
    ESP_LOGI(TAG, "Refresh callbacks during %s: %u -> %u, delta=%u over %u ms "
             "(host/DMA progress only; not proof of LCD reception)",
             stage, (unsigned int)before, (unsigned int)after,
             (unsigned int)(after - before), elapsed_ms);
}
#endif

#if NEWHAVEN_DISPLAY_STARTUP_COLOR_TEST_MS > 0
#if NEWHAVEN_DISPLAY_STARTUP_COLOR_TEST_MS <= 1000
#error "Startup color tests require more than 1000 ms per color for refresh sampling."
#endif

static void run_startup_color_tests(newhaven_display_t *display)
{
    static const struct {
        const char *name;
        uint32_t rgb;
    } colors[] = {
        {"black", 0x000000U},
        {"red",   0xFF0000U},
        {"green", 0x00FF00U},
        {"blue",  0x0000FFU},
        {"white", 0xFFFFFFU},
    };

    for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); i++) {
        ESP_LOGI(TAG, "Showing %s for %d ms",
                 colors[i].name, NEWHAVEN_DISPLAY_STARTUP_COLOR_TEST_MS);
        for (int index = 0; index < NEWHAVEN_DISPLAY_FB_COUNT; index++) {
            void *framebuffer;
            ESP_ERROR_CHECK(newhaven_display_begin_frame(display, &framebuffer));
            ESP_ERROR_CHECK(newhaven_display_fill(display, colors[i].rgb));
            ESP_ERROR_CHECK(newhaven_display_flush_framebuffer(display));
        }

        const TickType_t color_start = xTaskGetTickCount();
#if NEWHAVEN_DISPLAY_DIAGNOSTICS
        const uint32_t refresh_before = newhaven_display_refresh_count();
#endif
        TickType_t sample_time = color_start;
        vTaskDelayUntil(&sample_time, pdMS_TO_TICKS(200));
        vTaskDelayUntil(&sample_time, pdMS_TO_TICKS(800));
#if NEWHAVEN_DISPLAY_DIAGNOSTICS
        if (colors[i].rgb == 0xFF0000U) {
            log_refresh_progress("red framebuffer", refresh_before, color_start);
        }
#endif
#if NEWHAVEN_DISPLAY_DIAGNOSTICS
        if (colors[i].rgb == 0xFF0000U) {
            newhaven_display_log_host_snapshot("during red framebuffer test");
        }
#endif
        vTaskDelayUntil(&sample_time, pdMS_TO_TICKS(NEWHAVEN_DISPLAY_STARTUP_COLOR_TEST_MS - 1000));
    }
    ESP_LOGI(TAG, "Startup color tests complete; resuming gauge display");
}
#endif

void app_main(void)
{
    ESP_LOGI(TAG, "Starting Newhaven NHD-4.0-480480AF-MSXP bring-up");

    newhaven_display_t display;
    ESP_ERROR_CHECK(newhaven_display_init(&display));
    ESP_ERROR_CHECK(backlight_init());
    ESP_ERROR_CHECK(backlight_set_brightness(NEWHAVEN_BACKLIGHT_DEFAULT_PERCENT));

#if NEWHAVEN_DISPLAY_HARDWARE_PATTERN_MS > 0
    ESP_LOGI(TAG, "Showing DSI HOST hardware color bars for %d ms (bypasses framebuffers)",
             NEWHAVEN_DISPLAY_HARDWARE_PATTERN_MS);
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_set_pattern(display.dpi_panel, MIPI_DSI_PATTERN_BAR_VERTICAL));
#if NEWHAVEN_DISPLAY_DIAGNOSTICS && NEWHAVEN_DISPLAY_HARDWARE_PATTERN_MS > 1000
    const TickType_t pattern_start = xTaskGetTickCount();
    const uint32_t pattern_refresh_before = newhaven_display_refresh_count();
    TickType_t pattern_sample_time = pattern_start;
    vTaskDelayUntil(&pattern_sample_time, pdMS_TO_TICKS(1000));
    log_refresh_progress("host hardware color bars", pattern_refresh_before, pattern_start);
    newhaven_display_log_host_snapshot("during host hardware color bars");
    ESP_LOGI(TAG, "Host pattern bypasses DPI bridge; pre-v3 DMA refresh callbacks can stop or slow");
    vTaskDelayUntil(&pattern_sample_time, pdMS_TO_TICKS(NEWHAVEN_DISPLAY_HARDWARE_PATTERN_MS - 1000));
#else
    vTaskDelay(pdMS_TO_TICKS(NEWHAVEN_DISPLAY_HARDWARE_PATTERN_MS));
#endif
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_set_pattern(display.dpi_panel, MIPI_DSI_PATTERN_NONE));
    ESP_LOGI(TAG, "DSI HOST hardware color bars disabled; returning to framebuffer output");
#endif

#if NEWHAVEN_DISPLAY_STARTUP_COLOR_TEST_MS > 0
    run_startup_color_tests(&display);
#endif

    const size_t oil_pressure_gauge_size =
        oil_pressure_gauge_rgb565_end - oil_pressure_gauge_rgb565_start;
    const size_t needle_size = needle_rgba8888_end - needle_rgba8888_start;
    ESP_LOGI(TAG, "Preparing %d framebuffer(s) from the embedded oil-pressure gauge (%u bytes)",
             NEWHAVEN_DISPLAY_FB_COUNT,
             (unsigned int)oil_pressure_gauge_size);
    for (int index = 0; index < NEWHAVEN_DISPLAY_FB_COUNT; index++) {
        void *framebuffer;
        ESP_ERROR_CHECK(newhaven_display_begin_frame(&display, &framebuffer));
        ESP_ERROR_CHECK(newhaven_display_prepare_rgb565(&display,
                                                         oil_pressure_gauge_rgb565_start,
                                                         oil_pressure_gauge_size));
        ESP_ERROR_CHECK(newhaven_display_flush_framebuffer(&display));
    }
    ESP_ERROR_CHECK(gauge_renderer_init(oil_pressure_gauge_rgb565_start,
                                        oil_pressure_gauge_size,
                                        needle_rgba8888_start,
                                        needle_size));
    ESP_LOGI(TAG, "Starting double-buffered animated oil-pressure gauge");

    const TickType_t frame_period = pdMS_TO_TICKS(1000 / GAUGE_FRAME_RATE_HZ);
    TickType_t last_wake_time = xTaskGetTickCount();
    const TickType_t animation_start_time = last_wake_time;
    uint32_t last_log_time_ms = 0;
    bool startup_sweep_completed = false;

    while (true) {
        const uint32_t elapsed_time_ms =
            (uint32_t)((xTaskGetTickCount() - animation_start_time) * portTICK_PERIOD_MS);
        if (elapsed_time_ms < GAUGE_CYCLE_DURATION_MS) {
            const float cycle_fraction = (float)(elapsed_time_ms <= GAUGE_SWEEP_DURATION_MS ?
                                                     elapsed_time_ms :
                                                     GAUGE_CYCLE_DURATION_MS - elapsed_time_ms) /
                                         (float)GAUGE_SWEEP_DURATION_MS;
            gauge_renderer_set_value_immediately(cycle_fraction * GAUGE_RENDERER_MAX_PSI);
        } else if (!startup_sweep_completed) {
            gauge_renderer_set_value_immediately(GAUGE_RENDERER_MIN_PSI);
            startup_sweep_completed = true;
        }

        void *framebuffer;
        ESP_ERROR_CHECK(newhaven_display_begin_frame(&display, &framebuffer));
        ESP_ERROR_CHECK(gauge_renderer_render(framebuffer));
        ESP_ERROR_CHECK(newhaven_display_flush_framebuffer(&display));

        if (elapsed_time_ms - last_log_time_ms >= GAUGE_LOG_INTERVAL_MS) {
            const gauge_state_t *gauge_state = gauge_renderer_get_state();
            ESP_LOGI(TAG, "Gauge target %.2f, displayed %.2f, angle %.1f degrees",
                     gauge_state->target_value,
                     gauge_state->displayed_value,
                     gauge_value_to_angle(gauge_state->displayed_value));
            last_log_time_ms = elapsed_time_ms;
        }

        vTaskDelayUntil(&last_wake_time, frame_period);
    }
}
