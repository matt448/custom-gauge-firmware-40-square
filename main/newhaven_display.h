#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"

typedef struct {
    esp_lcd_dsi_bus_handle_t dsi_bus;
    esp_lcd_panel_io_handle_t dbi_io;
    esp_lcd_panel_handle_t dpi_panel;
    esp_ldo_channel_handle_t dsi_phy_ldo;
    void *framebuffer;
    void *backbuffer;
} newhaven_display_t;

esp_err_t newhaven_display_reset(void);
esp_err_t newhaven_st7701s_init(esp_lcd_panel_handle_t panel);
esp_err_t newhaven_display_init(newhaven_display_t *display);
esp_err_t newhaven_display_begin_frame(newhaven_display_t *display, void **framebuffer);
esp_err_t newhaven_display_prepare_rgb565(newhaven_display_t *display,
                                         const uint8_t *image,
                                         size_t image_size);
esp_err_t newhaven_display_fill(newhaven_display_t *display, uint32_t rgb);
esp_err_t newhaven_display_flush_framebuffer(newhaven_display_t *display);
esp_err_t newhaven_display_draw_rgb565(newhaven_display_t *display,
                                       const uint8_t *image,
                                       size_t image_size);
