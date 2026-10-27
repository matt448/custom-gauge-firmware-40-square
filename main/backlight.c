#include <stdint.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"

#include "backlight.h"
#include "display_config.h"

static const char *TAG = "backlight";

#define BACKLIGHT_LEDC_MODE LEDC_LOW_SPEED_MODE
#define BACKLIGHT_LEDC_TIMER LEDC_TIMER_0
#define BACKLIGHT_LEDC_CHANNEL LEDC_CHANNEL_0
#define BACKLIGHT_LEDC_DUTY_RESOLUTION LEDC_TIMER_10_BIT
#define BACKLIGHT_LEDC_MAX_DUTY ((1U << BACKLIGHT_LEDC_DUTY_RESOLUTION) - 1U)

esp_err_t backlight_init(void)
{
    if (!GPIO_IS_VALID_OUTPUT_GPIO(NEWHAVEN_BACKLIGHT_PWM_GPIO)) {
        ESP_LOGE(TAG, "DIM GPIO is not a valid output GPIO");
        return ESP_ERR_INVALID_ARG;
    }

    const ledc_timer_config_t timer_config = {
        .speed_mode = BACKLIGHT_LEDC_MODE,
        .duty_resolution = BACKLIGHT_LEDC_DUTY_RESOLUTION,
        .timer_num = BACKLIGHT_LEDC_TIMER,
        .freq_hz = NEWHAVEN_BACKLIGHT_PWM_FREQUENCY_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "configure PWM timer failed: %s", esp_err_to_name(err));
        return err;
    }

    const ledc_channel_config_t channel_config = {
        .gpio_num = NEWHAVEN_BACKLIGHT_PWM_GPIO,
        .speed_mode = BACKLIGHT_LEDC_MODE,
        .channel = BACKLIGHT_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BACKLIGHT_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&channel_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "configure DIM PWM channel failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Configured TPS92511DDA DIM PWM on GPIO%d at %d Hz",
             NEWHAVEN_BACKLIGHT_PWM_GPIO, NEWHAVEN_BACKLIGHT_PWM_FREQUENCY_HZ);
    return ESP_OK;
}

esp_err_t backlight_set_brightness(uint8_t percent)
{
    if (percent > 100) {
        ESP_LOGE(TAG, "brightness %u is outside the valid 0-100 range", percent);
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t duty = (BACKLIGHT_LEDC_MAX_DUTY * percent) / 100;
    esp_err_t err = ledc_set_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL, duty);
    if (err == ESP_OK) {
        err = ledc_update_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set DIM PWM duty failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Set display brightness to %u%%", percent);
    return ESP_OK;
}
