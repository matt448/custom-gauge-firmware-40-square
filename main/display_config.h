#pragma once

#include "driver/gpio.h"

#define NEWHAVEN_DISPLAY_H_RES                 480
#define NEWHAVEN_DISPLAY_V_RES                 480
#define NEWHAVEN_DISPLAY_PIXEL_CLOCK_MHZ       25

#define NEWHAVEN_DISPLAY_H_FRONT_PORCH         150
#define NEWHAVEN_DISPLAY_H_BACK_PORCH          150
#define NEWHAVEN_DISPLAY_H_SYNC_PULSE          10
#define NEWHAVEN_DISPLAY_V_FRONT_PORCH         20
#define NEWHAVEN_DISPLAY_V_BACK_PORCH          20
#define NEWHAVEN_DISPLAY_V_SYNC_PULSE          10

#define NEWHAVEN_DISPLAY_DSI_BUS_ID            0
#define NEWHAVEN_DISPLAY_DSI_DATA_LANES        2
/*
 * Newhaven specifies a 25 MHz RGB888 pixel clock, equivalent to 300 Mbps
 * per lane across two DSI lanes. ESP-IDF uses burst video with sync pulses;
 * 500 Mbps per lane provides additional bandwidth for packet overhead.
 */
#define NEWHAVEN_DISPLAY_DSI_LANE_BIT_RATE_MBPS 500
#define NEWHAVEN_DISPLAY_DSI_CLOCK_FORCE_HS     1

/*
 * GPIO9 is wired to FFC pin 15 (RESETX). The display has an external
 * 10 kOhm pull-up.
 */
#define NEWHAVEN_DISPLAY_RESET_GPIO            GPIO_NUM_9
#define NEWHAVEN_DISPLAY_RESET_GPIO_CONFIGURED 1
#define NEWHAVEN_DISPLAY_RESET_PULSE_MS        10
#define NEWHAVEN_DISPLAY_DIAGNOSTICS           1
#define NEWHAVEN_DISPLAY_HARDWARE_PATTERN_MS   0

/*
 * Show black, red, green, blue, and white for this interval per color.
 * Keep this above 1000 ms to allow refresh sampling, or set to zero
 * after the panel is verified.
 */
#define NEWHAVEN_DISPLAY_STARTUP_COLOR_TEST_MS 0

/*
 * GPIO8 drives the TPS92511DDA DIM input. The DIM input accepts a PWM signal;
 * 20 kHz avoids visible flicker while retaining adequate duty-cycle resolution.
 */
#define NEWHAVEN_BACKLIGHT_PWM_GPIO            GPIO_NUM_8
#define NEWHAVEN_BACKLIGHT_PWM_FREQUENCY_HZ    20000
#define NEWHAVEN_BACKLIGHT_DEFAULT_PERCENT     100

/*
 * Keep one framebuffer under DSI scanout while the application renders into
 * the other. This prevents visible tearing and DSI bridge underruns caused by
 * modifying the actively scanned PSRAM framebuffer.
 */
#define NEWHAVEN_DISPLAY_FB_COUNT              2

#define NEWHAVEN_DISPLAY_PIXEL_FORMAT_RGB888   888
#define NEWHAVEN_DISPLAY_PIXEL_FORMAT_RGB565   565
#define NEWHAVEN_DISPLAY_PIXEL_FORMAT          NEWHAVEN_DISPLAY_PIXEL_FORMAT_RGB888

#define NEWHAVEN_DISPLAY_COLOR_ORDER_RGB       0
#define NEWHAVEN_DISPLAY_COLOR_ORDER_BGR       1
#define NEWHAVEN_DISPLAY_COLOR_ORDER           NEWHAVEN_DISPLAY_COLOR_ORDER_RGB

#define NEWHAVEN_DISPLAY_ROTATION_0            0
#define NEWHAVEN_DISPLAY_ROTATION_90           90
#define NEWHAVEN_DISPLAY_ROTATION_180          180
#define NEWHAVEN_DISPLAY_ROTATION_270          270
#define NEWHAVEN_DISPLAY_ROTATION              NEWHAVEN_DISPLAY_ROTATION_0

/*
 * VDD_MIPI_DPHY must be 2.5 V before the DSI PHY can lock. ESP-IDF's native
 * MIPI DSI example and ST7701 MIPI test use internal LDO channel 3 for this
 * rail. This powers the ESP32-P4 PHY only, not panel VDD or the backlight.
 */
#define NEWHAVEN_DISPLAY_ENABLE_DSI_PHY_LDO    1
#define NEWHAVEN_DISPLAY_DSI_PHY_LDO_CHANNEL   3
#define NEWHAVEN_DISPLAY_DSI_PHY_LDO_MV        2500
