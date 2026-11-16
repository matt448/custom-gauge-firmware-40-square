#include <stdint.h>
#include <stdbool.h>
#include <limits.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_attr.h"
#include "esp_log.h"

#include "backlight.h"
#include "display_config.h"

static const char *TAG = "backlight";

#define BACKLIGHT_LEDC_MODE LEDC_LOW_SPEED_MODE
#define BACKLIGHT_LEDC_TIMER LEDC_TIMER_0
#define BACKLIGHT_LEDC_CHANNEL LEDC_CHANNEL_0
#define BACKLIGHT_LEDC_DUTY_RESOLUTION LEDC_TIMER_10_BIT
#define BACKLIGHT_LEDC_MAX_DUTY ((1U << BACKLIGHT_LEDC_DUTY_RESOLUTION) - 1U)

static bool initialized;
static uint32_t duty_update_busy;
_Static_assert(__atomic_always_lock_free(sizeof(duty_update_busy), 0),
               "Backlight fade completion must use lock-free ISR-safe atomics");

static bool IRAM_ATTR fade_complete_callback(const ledc_cb_param_t *param, void *user_arg)
{
    (void)param;
    (void)user_arg;
    __atomic_store_n(&duty_update_busy, 0, __ATOMIC_RELEASE);
    return false;
}

static esp_err_t acquire_duty_update(void)
{
    if (!initialized) {
        ESP_LOGE(TAG, "Backlight is not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (__atomic_exchange_n(&duty_update_busy, 1, __ATOMIC_ACQUIRE)) {
        ESP_LOGE(TAG, "A backlight duty update or fade is already active");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t backlight_init(void)
{
    if (initialized) {
        ESP_LOGE(TAG, "Backlight is already initialized");
        return ESP_ERR_INVALID_STATE;
    }
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

    err = ledc_fade_func_install(0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "install PWM fade service failed: %s", esp_err_to_name(err));
        return err;
    }
    ledc_cbs_t callbacks = {
        .fade_cb = fade_complete_callback,
    };
    err = ledc_cb_register(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL, &callbacks, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register PWM fade callback failed: %s", esp_err_to_name(err));
        ledc_fade_func_uninstall();
        return err;
    }
    initialized = true;

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

    esp_err_t err = acquire_duty_update();
    if (err != ESP_OK) {
        return err;
    }
    const uint32_t duty = (BACKLIGHT_LEDC_MAX_DUTY * percent) / 100;
    err = ledc_set_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL, duty);
    if (err == ESP_OK) {
        err = ledc_update_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL);
    }
    __atomic_store_n(&duty_update_busy, 0, __ATOMIC_RELEASE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set DIM PWM duty failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Set display brightness to %u%%", percent);
    return ESP_OK;
}

esp_err_t backlight_fade_to(uint8_t percent, uint32_t duration_ms)
{
    if (percent > 100 || duration_ms > INT_MAX ||
        duration_ms > UINT32_MAX / NEWHAVEN_BACKLIGHT_PWM_FREQUENCY_HZ) {
        ESP_LOGE(TAG, "Invalid fade brightness %u or duration %lu ms",
                 percent, (unsigned long)duration_ms);
        return ESP_ERR_INVALID_ARG;
    }
    if (duration_ms == 0) {
        return backlight_set_brightness(percent);
    }

    esp_err_t err = acquire_duty_update();
    if (err != ESP_OK) {
        return err;
    }
    const uint32_t duty = (BACKLIGHT_LEDC_MAX_DUTY * percent) / 100;
    err = ledc_set_fade_time_and_start(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL,
                                      duty, duration_ms, LEDC_FADE_NO_WAIT);
    if (err != ESP_OK) {
        __atomic_store_n(&duty_update_busy, 0, __ATOMIC_RELEASE);
        ESP_LOGE(TAG, "start DIM PWM fade failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Fading display brightness to %u%% over %lu ms",
             percent, (unsigned long)duration_ms);
    return ESP_OK;
}
