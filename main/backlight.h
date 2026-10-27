#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t backlight_init(void);
esp_err_t backlight_set_brightness(uint8_t percent);
