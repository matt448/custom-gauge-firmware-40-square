#pragma once

#include <stdint.h>

#include "esp_lcd_mipi_dsi.h"

void newhaven_display_diagnostics_register(esp_lcd_panel_handle_t panel);
uint32_t newhaven_display_refresh_count(void);
void newhaven_display_log_host_snapshot(const char *stage);
