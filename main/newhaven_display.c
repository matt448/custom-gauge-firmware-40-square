#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_lcd_st7701.h"

#include "display_config.h"
#include "newhaven_display.h"
#include "newhaven_display_diagnostics.h"

static const char *TAG = "newhaven_display";

#if NEWHAVEN_DISPLAY_DSI_DATA_LANES != 2
#error "The NHD-4.0-480480AF-MSXP requires DSI data lanes 0 and 1."
#endif

#if (NEWHAVEN_DISPLAY_PIXEL_FORMAT != NEWHAVEN_DISPLAY_PIXEL_FORMAT_RGB888) && \
    (NEWHAVEN_DISPLAY_PIXEL_FORMAT != NEWHAVEN_DISPLAY_PIXEL_FORMAT_RGB565)
#error "NEWHAVEN_DISPLAY_PIXEL_FORMAT must be RGB888 or RGB565."
#endif

#if (NEWHAVEN_DISPLAY_COLOR_ORDER != NEWHAVEN_DISPLAY_COLOR_ORDER_RGB) && \
    (NEWHAVEN_DISPLAY_COLOR_ORDER != NEWHAVEN_DISPLAY_COLOR_ORDER_BGR)
#error "NEWHAVEN_DISPLAY_COLOR_ORDER must be RGB or BGR."
#endif

#if (NEWHAVEN_DISPLAY_ROTATION != NEWHAVEN_DISPLAY_ROTATION_0) && \
    (NEWHAVEN_DISPLAY_ROTATION != NEWHAVEN_DISPLAY_ROTATION_90) && \
    (NEWHAVEN_DISPLAY_ROTATION != NEWHAVEN_DISPLAY_ROTATION_180) && \
    (NEWHAVEN_DISPLAY_ROTATION != NEWHAVEN_DISPLAY_ROTATION_270)
#error "NEWHAVEN_DISPLAY_ROTATION must be 0, 90, 180, or 270."
#endif

#if (NEWHAVEN_DISPLAY_FB_COUNT < 1) || (NEWHAVEN_DISPLAY_FB_COUNT > 2)
#error "NEWHAVEN_DISPLAY_FB_COUNT must be 1 or 2."
#endif

#if NEWHAVEN_DISPLAY_PIXEL_FORMAT == NEWHAVEN_DISPLAY_PIXEL_FORMAT_RGB888
#define NEWHAVEN_DISPLAY_BYTES_PER_PIXEL 3
#define NEWHAVEN_DISPLAY_LCD_COLOR_FORMAT LCD_COLOR_FMT_RGB888
#else
#define NEWHAVEN_DISPLAY_BYTES_PER_PIXEL 2
#define NEWHAVEN_DISPLAY_LCD_COLOR_FORMAT LCD_COLOR_FMT_RGB565
#endif

#define NEWHAVEN_DISPLAY_FRAMEBUFFER_SIZE \
    (NEWHAVEN_DISPLAY_H_RES * NEWHAVEN_DISPLAY_V_RES * NEWHAVEN_DISPLAY_BYTES_PER_PIXEL)

#if NEWHAVEN_DISPLAY_PIXEL_FORMAT == NEWHAVEN_DISPLAY_PIXEL_FORMAT_RGB888
#define NEWHAVEN_DISPLAY_BITS_PER_PIXEL 24
#else
#define NEWHAVEN_DISPLAY_BITS_PER_PIXEL 16
#endif

#if NEWHAVEN_DISPLAY_COLOR_ORDER == NEWHAVEN_DISPLAY_COLOR_ORDER_RGB
#define NEWHAVEN_DISPLAY_RGB_ELEMENT_ORDER LCD_RGB_ELEMENT_ORDER_RGB
#else
#define NEWHAVEN_DISPLAY_RGB_ELEMENT_ORDER LCD_RGB_ELEMENT_ORDER_BGR
#endif

#define RETURN_ON_ERROR(expression, stage) do { \
    esp_err_t __err = (expression); \
    if (__err != ESP_OK) { \
        ESP_LOGE(TAG, "%s failed: %s", stage, esp_err_to_name(__err)); \
        return __err; \
    } \
} while (0)

#if NEWHAVEN_DISPLAY_DIAGNOSTICS
static void newhaven_display_log_panel_status(esp_lcd_panel_io_handle_t io, const char *stage)
{
    static const struct {
        uint8_t command;
        uint8_t size;
        const char *name;
    } registers[] = {
        {0x04, 3, "display ID"},
        {0x05, 1, "DSI error count"},
        {0x0A, 1, "power mode"},
        {0x0B, 1, "address mode"},
        {0x0C, 1, "pixel format"},
        {0x0D, 1, "display mode"},
        {0x0F, 1, "self diagnostic"},
    };
    ESP_LOGI(TAG, "DCS readback: %s (raw responses; ESP_OK alone does not validate a reply)", stage);
    for (size_t i = 0; i < sizeof(registers) / sizeof(registers[0]); i++) {
        uint8_t data[3] = {0xA5, 0xA5, 0xA5};
        const esp_err_t err = esp_lcd_panel_io_rx_param(io, registers[i].command,
                                                       data, registers[i].size);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "DCS 0x%02X %s read failed: %s",
                     registers[i].command, registers[i].name, esp_err_to_name(err));
            continue;
        }
        ESP_LOGI(TAG, "DCS 0x%02X %s (%u byte(s)):", registers[i].command,
                 registers[i].name, (unsigned int)registers[i].size);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, registers[i].size, ESP_LOG_INFO);
        if (registers[i].command == 0x04 && data[0] == 0xFF &&
            data[1] == 0xFF && data[2] == 0xFF) {
            ESP_LOGI(TAG, "FF FF FF is the ST7701SN default ID; it does not establish link health");
        } else if (registers[i].command == 0x0C) {
            ESP_LOGI(TAG, "Pixel-format readback VIPF=%u (5=RGB565, 6=RGB666, 7=RGB888)",
                     (unsigned int)((data[0] >> 4) & 0x07));
        }
    }
}
#endif

static void newhaven_display_map_coordinate(int x, int y, int *mapped_x, int *mapped_y)
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

static void newhaven_display_put_pixel(void *framebuffer, int x, int y, uint32_t rgb)
{
    int mapped_x;
    int mapped_y;
    newhaven_display_map_coordinate(x, y, &mapped_x, &mapped_y);

#if NEWHAVEN_DISPLAY_PIXEL_FORMAT == NEWHAVEN_DISPLAY_PIXEL_FORMAT_RGB888
    uint8_t *pixel = (uint8_t *)framebuffer +
                     ((mapped_y * NEWHAVEN_DISPLAY_H_RES + mapped_x) * NEWHAVEN_DISPLAY_BYTES_PER_PIXEL);
    uint8_t red = (rgb >> 16) & 0xff;
    uint8_t green = (rgb >> 8) & 0xff;
    uint8_t blue = rgb & 0xff;
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
    uint16_t temporary = red;
    red = blue;
    blue = temporary;
#endif
    ((uint16_t *)framebuffer)[mapped_y * NEWHAVEN_DISPLAY_H_RES + mapped_x] =
        (red << 11) | (green << 5) | blue;
#endif
}

esp_err_t newhaven_display_reset(void)
{
#if !NEWHAVEN_DISPLAY_RESET_GPIO_CONFIGURED
    ESP_LOGE(TAG, "RESETX GPIO is not configured; set NEWHAVEN_DISPLAY_RESET_GPIO and NEWHAVEN_DISPLAY_RESET_GPIO_CONFIGURED in display_config.h");
    return ESP_ERR_INVALID_STATE;
#else
    const gpio_num_t reset_gpio = NEWHAVEN_DISPLAY_RESET_GPIO;

    if (!GPIO_IS_VALID_GPIO(reset_gpio)) {
        ESP_LOGE(TAG, "RESETX GPIO is not configured; set NEWHAVEN_DISPLAY_RESET_GPIO in display_config.h");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Configuring and resetting RESETX");
    const gpio_config_t reset_config = {
        .pin_bit_mask = 1ULL << reset_gpio,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    RETURN_ON_ERROR(gpio_config(&reset_config), "configure RESETX GPIO");
    RETURN_ON_ERROR(gpio_set_level(reset_gpio, 1), "drive RESETX high");
    RETURN_ON_ERROR(gpio_set_level(reset_gpio, 0), "drive RESETX low");
#if NEWHAVEN_DISPLAY_DIAGNOSTICS
    ESP_LOGI(TAG, "RESETX GPIO%d low readback: %d", reset_gpio, gpio_get_level(reset_gpio));
#endif
    esp_rom_delay_us(NEWHAVEN_DISPLAY_RESET_PULSE_MS * 1000);
    RETURN_ON_ERROR(gpio_set_level(reset_gpio, 1), "release RESETX");
    vTaskDelay(pdMS_TO_TICKS(120));
#if NEWHAVEN_DISPLAY_DIAGNOSTICS
    ESP_LOGI(TAG, "RESETX GPIO%d released readback: %d (MCU pin only, not panel acknowledgement)",
             reset_gpio, gpio_get_level(reset_gpio));
#endif
    return ESP_OK;
#endif
}

/*
 * Exact ST7701SN register sequence published by Newhaven for the
 * NHD-4.0-480480AF-MSXP:
 * https://support.newhavendisplay.com/hc/en-us/community/posts/39646987900567-Any-initialization-code-for-NHD-4-0-480480AF-MSXP-CTP
 *
 * The published example uses WriteComm/WriteData calls. This project sends
 * those panel-register commands through the native MIPI DSI DBI channel.
 */
static const st7701_lcd_init_cmd_t newhaven_st7701s_init_cmds[] = {
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xEF, (uint8_t []){0x08}, 1, 0},
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, (uint8_t []){0x3B, 0x00}, 2, 0},
    {0xC1, (uint8_t []){0x0D, 0x02}, 2, 0},
    {0xC2, (uint8_t []){0x21, 0x08}, 2, 0},
    {0xC7, (uint8_t []){0x00}, 1, 0},
    {0xCC, (uint8_t []){0x18}, 1, 0},
    {0xB0, (uint8_t []){0x00, 0x13, 0x1E, 0x0E, 0x11, 0x05, 0x09, 0x07, 0x07, 0x23, 0x04, 0x12, 0x0F, 0xA7, 0x2C, 0x18}, 16, 0},
    {0xB1, (uint8_t []){0x00, 0x14, 0x1B, 0x0E, 0x11, 0x06, 0x06, 0x08, 0x07, 0x20, 0x04, 0x12, 0x11, 0xA5, 0x2F, 0x18}, 16, 0},
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, (uint8_t []){0x60}, 1, 0},
    {0xB1, (uint8_t []){0x31}, 1, 0},
    {0xB2, (uint8_t []){0x8A}, 1, 0},
    {0xB3, (uint8_t []){0x80}, 1, 0},
    {0xB5, (uint8_t []){0x4B}, 1, 0},
    {0xB7, (uint8_t []){0x85}, 1, 0},
    {0xB8, (uint8_t []){0x21}, 1, 0},
    {0xC0, (uint8_t []){0x07}, 1, 0},
    {0xC1, (uint8_t []){0x78}, 1, 0},
    {0xC2, (uint8_t []){0x78}, 1, 0},
    {0xE0, (uint8_t []){0x00, 0x1B, 0x02}, 3, 0},
    {0xE1, (uint8_t []){0x08, 0xA0, 0x00, 0x00, 0x07, 0xA0, 0x00, 0x00, 0x00, 0x44, 0x44}, 11, 0},
    {0xE2, (uint8_t []){0x11, 0x11, 0x44, 0x44, 0xED, 0xA0, 0x00, 0x00, 0xEC, 0xA0, 0x00, 0x00}, 12, 0},
    {0xE3, (uint8_t []){0x00, 0x00, 0x11, 0x11}, 4, 0},
    {0xE4, (uint8_t []){0x44, 0x44}, 2, 0},
    {0xE5, (uint8_t []){0x0A, 0xE9, 0xD8, 0xA0, 0x0C, 0xEB, 0xD8, 0xA0, 0x0E, 0xED, 0xD8, 0xA0, 0x10, 0xEF, 0xD8, 0xA0}, 16, 0},
    {0xE6, (uint8_t []){0x00, 0x00, 0x11, 0x11}, 4, 0},
    {0xE7, (uint8_t []){0x44, 0x44}, 2, 0},
    {0xE8, (uint8_t []){0x09, 0xE8, 0xD8, 0xA0, 0x0B, 0xEA, 0xD8, 0xA0, 0x0D, 0xEC, 0xD8, 0xA0, 0x0F, 0xEE, 0xD8, 0xA0}, 16, 0},
    {0xEB, (uint8_t []){0x02, 0x00, 0xE4, 0xE4, 0x88, 0x00, 0x40}, 7, 0},
    {0xEC, (uint8_t []){0x3C, 0x00}, 2, 0},
    {0xED, (uint8_t []){0xAB, 0x89, 0x76, 0x54, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x20, 0x45, 0x67, 0x98, 0xBA}, 16, 0},
    {0xEF, (uint8_t []){0x08, 0x08, 0x08, 0x45, 0x3F, 0x54}, 6, 0},
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xE8, (uint8_t []){0x00, 0x0E}, 2, 0},
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    {0x11, NULL, 0, 120},
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xE8, (uint8_t []){0x00, 0x0C}, 2, 10},
    {0xE8, (uint8_t []){0x00, 0x00}, 2, 0},
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    {0x29, NULL, 0, 120},
    {0x36, (uint8_t []){0x00}, 1, 0},
};

esp_err_t newhaven_st7701s_init(esp_lcd_panel_handle_t panel)
{
    if (panel == NULL) {
        ESP_LOGE(TAG, "ST7701SN panel is unavailable");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Sending Newhaven NHD-4.0-480480AF-MSXP ST7701SN initialization sequence");
    RETURN_ON_ERROR(esp_lcd_panel_init(panel), "send ST7701SN initialization commands");
    RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), "send Display On command");
    return ESP_OK;
}

esp_err_t newhaven_display_init(newhaven_display_t *display)
{
    if (display == NULL) {
        ESP_LOGE(TAG, "display handle is null");
        return ESP_ERR_INVALID_ARG;
    }
    memset(display, 0, sizeof(*display));
#if NEWHAVEN_DISPLAY_DIAGNOSTICS
    ESP_LOGI(TAG, "DSI configuration: %d lanes, %d Mbps/lane, %d MHz pixel clock, %d bpp",
             NEWHAVEN_DISPLAY_DSI_DATA_LANES, NEWHAVEN_DISPLAY_DSI_LANE_BIT_RATE_MBPS,
             NEWHAVEN_DISPLAY_PIXEL_CLOCK_MHZ, NEWHAVEN_DISPLAY_BITS_PER_PIXEL);
    ESP_LOGI(TAG, "LDO software request: channel %d at %d mV; this is not a voltage measurement",
             NEWHAVEN_DISPLAY_DSI_PHY_LDO_CHANNEL, NEWHAVEN_DISPLAY_DSI_PHY_LDO_MV);
#endif

#if NEWHAVEN_DISPLAY_ENABLE_DSI_PHY_LDO
    ESP_LOGI(TAG, "Enabling MIPI DSI PHY internal LDO");
    const esp_ldo_channel_config_t ldo_config = {
        .chan_id = NEWHAVEN_DISPLAY_DSI_PHY_LDO_CHANNEL,
        .voltage_mv = NEWHAVEN_DISPLAY_DSI_PHY_LDO_MV,
    };
    RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_config, &display->dsi_phy_ldo),
                    "enable MIPI DSI PHY LDO");
#endif

    RETURN_ON_ERROR(newhaven_display_reset(), "hardware reset");

    ESP_LOGI(TAG, "Creating two-lane MIPI DSI bus on data lanes 0 and 1");
    ESP_LOGI(TAG, "DSI clock lane: %s",
             NEWHAVEN_DISPLAY_DSI_CLOCK_FORCE_HS ? "continuous HS (clock test)" : "automatic HS/LP");
    const esp_lcd_dsi_bus_config_t dsi_bus_config = {
        .bus_id = NEWHAVEN_DISPLAY_DSI_BUS_ID,
        .num_data_lanes = NEWHAVEN_DISPLAY_DSI_DATA_LANES,
        /*
         * ESP32-P4 v1.x uses the legacy DSI PHY implementation, which does
         * not accept the v3+ XTAL default. PLL_F20M is ESP-IDF's documented
         * legacy default and is valid on the connected v1.3 board.
         */
        .phy_clk_src = MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT_LEGACY,
        .lane_bit_rate_mbps = NEWHAVEN_DISPLAY_DSI_LANE_BIT_RATE_MBPS,
        .flags.clock_lane_force_hs = NEWHAVEN_DISPLAY_DSI_CLOCK_FORCE_HS,
    };
    RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&dsi_bus_config, &display->dsi_bus), "create MIPI DSI bus");
#if NEWHAVEN_DISPLAY_DIAGNOSTICS
    newhaven_display_log_host_snapshot("after DSI bus creation");
#endif

    ESP_LOGI(TAG, "Creating MIPI DSI command interface");
    const esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(display->dsi_bus, &dbi_config, &display->dbi_io),
                    "create MIPI DSI command interface");
#if NEWHAVEN_DISPLAY_DIAGNOSTICS
    /* IDF 6.0.2 DCS reads leave command mode selected; read only before panel init. */
    newhaven_display_log_panel_status(display->dbi_io, "after hardware reset, before panel initialization");
#endif

    ESP_LOGI(TAG, "Creating 480x480 MIPI DSI ST7701SN panel with %d framebuffer(s)",
             NEWHAVEN_DISPLAY_FB_COUNT);
    const esp_lcd_dpi_panel_config_t dpi_config = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = NEWHAVEN_DISPLAY_PIXEL_CLOCK_MHZ,
        .in_color_format = NEWHAVEN_DISPLAY_LCD_COLOR_FORMAT,
        .out_color_format = NEWHAVEN_DISPLAY_LCD_COLOR_FORMAT,
        .num_fbs = NEWHAVEN_DISPLAY_FB_COUNT,
        .video_timing = {
            .h_size = NEWHAVEN_DISPLAY_H_RES,
            .v_size = NEWHAVEN_DISPLAY_V_RES,
            .hsync_back_porch = NEWHAVEN_DISPLAY_H_BACK_PORCH,
            .hsync_pulse_width = NEWHAVEN_DISPLAY_H_SYNC_PULSE,
            .hsync_front_porch = NEWHAVEN_DISPLAY_H_FRONT_PORCH,
            .vsync_back_porch = NEWHAVEN_DISPLAY_V_BACK_PORCH,
            .vsync_pulse_width = NEWHAVEN_DISPLAY_V_SYNC_PULSE,
            .vsync_front_porch = NEWHAVEN_DISPLAY_V_FRONT_PORCH,
        },
    };
    st7701_vendor_config_t st7701_vendor_config = {
        .init_cmds = newhaven_st7701s_init_cmds,
        .init_cmds_size = sizeof(newhaven_st7701s_init_cmds) / sizeof(newhaven_st7701s_init_cmds[0]),
        .flags.use_mipi_interface = 1,
        .mipi_config = {
            .dsi_bus = display->dsi_bus,
            .dpi_config = &dpi_config,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        /*
         * RESETX was already pulsed and held high for 120 ms by
         * newhaven_display_reset(). Passing GPIO_NUM_NC prevents the managed
         * driver from replacing that required reset sequence.
         */
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = NEWHAVEN_DISPLAY_RGB_ELEMENT_ORDER,
        .bits_per_pixel = NEWHAVEN_DISPLAY_BITS_PER_PIXEL,
        .vendor_config = &st7701_vendor_config,
    };
    RETURN_ON_ERROR(esp_lcd_new_panel_st7701(display->dbi_io, &panel_config, &display->dpi_panel),
                    "create MIPI DSI ST7701SN panel");
#if NEWHAVEN_DISPLAY_FB_COUNT == 2
    RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(display->dpi_panel, 2,
                                                        &display->framebuffer,
                                                        &display->backbuffer),
                    "get framebuffers");
#else
    RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(display->dpi_panel, 1, &display->framebuffer),
                    "get framebuffer");
#endif

#if NEWHAVEN_DISPLAY_DIAGNOSTICS
    newhaven_display_diagnostics_register(display->dpi_panel);
#endif
    ESP_LOGI(TAG, "Initializing ST7701SN controller");
    RETURN_ON_ERROR(newhaven_st7701s_init(display->dpi_panel), "initialize ST7701SN");
#if NEWHAVEN_DISPLAY_DIAGNOSTICS
    newhaven_display_log_host_snapshot("after panel initialization");
#endif
    return ESP_OK;
}

esp_err_t newhaven_display_begin_frame(newhaven_display_t *display, void **framebuffer)
{
    if (display == NULL || display->framebuffer == NULL || framebuffer == NULL) {
        ESP_LOGE(TAG, "framebuffer is unavailable");
        return ESP_ERR_INVALID_STATE;
    }

    if (display->backbuffer != NULL) {
        void *previous_framebuffer = display->framebuffer;
        display->framebuffer = display->backbuffer;
        display->backbuffer = previous_framebuffer;
    }

    *framebuffer = display->framebuffer;
    return ESP_OK;
}

esp_err_t newhaven_display_prepare_rgb565(newhaven_display_t *display,
                                          const uint8_t *image,
                                          size_t image_size)
{
    const size_t expected_size = NEWHAVEN_DISPLAY_H_RES * NEWHAVEN_DISPLAY_V_RES * 2;

    if (display == NULL || display->framebuffer == NULL || image == NULL) {
        ESP_LOGE(TAG, "framebuffer is unavailable");
        return ESP_ERR_INVALID_STATE;
    }
    if (image_size != expected_size) {
        ESP_LOGE(TAG, "RGB565 asset has %u bytes; expected %u bytes",
                 (unsigned int)image_size, (unsigned int)expected_size);
        return ESP_ERR_INVALID_SIZE;
    }

    for (int y = 0; y < NEWHAVEN_DISPLAY_V_RES; y++) {
        for (int x = 0; x < NEWHAVEN_DISPLAY_H_RES; x++) {
            const size_t offset = ((size_t)y * NEWHAVEN_DISPLAY_H_RES + x) * 2;
            const uint16_t pixel = ((uint16_t)image[offset] << 8) | image[offset + 1];
            const uint8_t red = (pixel >> 11) & 0x1f;
            const uint8_t green = (pixel >> 5) & 0x3f;
            const uint8_t blue = pixel & 0x1f;
            const uint32_t rgb = ((uint32_t)((red << 3) | (red >> 2)) << 16) |
                                 ((uint32_t)((green << 2) | (green >> 4)) << 8) |
                                 ((uint32_t)((blue << 3) | (blue >> 2)));
            newhaven_display_put_pixel(display->framebuffer, x, y, rgb);
        }
    }

    return ESP_OK;
}

esp_err_t newhaven_display_fill(newhaven_display_t *display, uint32_t rgb)
{
    if (display == NULL || display->framebuffer == NULL) {
        ESP_LOGE(TAG, "framebuffer is unavailable");
        return ESP_ERR_INVALID_STATE;
    }

    for (int y = 0; y < NEWHAVEN_DISPLAY_V_RES; y++) {
        for (int x = 0; x < NEWHAVEN_DISPLAY_H_RES; x++) {
            newhaven_display_put_pixel(display->framebuffer, x, y, rgb);
        }
    }

    return ESP_OK;
}

esp_err_t newhaven_display_flush_framebuffer(newhaven_display_t *display)
{
    if (display == NULL || display->framebuffer == NULL) {
        ESP_LOGE(TAG, "framebuffer is unavailable");
        return ESP_ERR_INVALID_STATE;
    }

    RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(display->dpi_panel,
                                               0,
                                               0,
                                               NEWHAVEN_DISPLAY_H_RES,
                                               NEWHAVEN_DISPLAY_V_RES,
                                               display->framebuffer),
                    "present framebuffer");
    return ESP_OK;
}

esp_err_t newhaven_display_draw_rgb565(newhaven_display_t *display,
                                       const uint8_t *image,
                                       size_t image_size)
{
    void *framebuffer;
    RETURN_ON_ERROR(newhaven_display_begin_frame(display, &framebuffer), "begin framebuffer");
    RETURN_ON_ERROR(newhaven_display_prepare_rgb565(display, image, image_size),
                    "prepare RGB565 framebuffer");
    return newhaven_display_flush_framebuffer(display);
}
