#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t backlight_init(void);
esp_err_t backlight_set_brightness(uint8_t percent);
/* Nonblocking linear duty fade; zero duration sets brightness immediately.
 * Duty updates during a fade return ESP_ERR_INVALID_STATE.
 */
esp_err_t backlight_fade_to(uint8_t percent, uint32_t duration_ms);
