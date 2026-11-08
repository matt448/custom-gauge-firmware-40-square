idf_component_register(
    SRCS
        "main.c"
        "backlight.c"
        "can_receiver.c"
        "gauge_renderer.c"
        "ms3_decoder.c"
        "newhaven_display.c"
        "newhaven_display_diagnostics.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        esp_driver_gpio
        esp_driver_ledc
        esp_hw_support
        esp_lcd
        esp_lcd_st7701
        esp_lvgl_port
        esp_timer
        lvgl
        esp_mm
    PRIV_REQUIRES
        soc
        esp_driver_twai
    EMBED_FILES
        "../assets/oil_pressure_gauge.rgb565"
        "../assets/needle.rgba8888"
)

# esp_lvgl_port 2.9.0 uses an RGB-only callback field for its DSI panel path.
set(lvgl_port_dsi_source
    "${CMAKE_CURRENT_LIST_DIR}/../managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port_disp.c")
if(EXISTS "${lvgl_port_dsi_source}")
    file(READ "${lvgl_port_dsi_source}" lvgl_port_dsi_source_text)
    set(lvgl_port_invalid_dsi_callback
        "cbs.on_frame_buf_complete = lvgl_port_flush_dpi_vsync_ready_callback;")
    string(FIND "${lvgl_port_dsi_source_text}" "${lvgl_port_invalid_dsi_callback}"
           lvgl_port_invalid_dsi_callback_index)
    if(NOT lvgl_port_invalid_dsi_callback_index EQUAL -1)
        string(REPLACE "${lvgl_port_invalid_dsi_callback}"
                       "cbs.on_refresh_done = lvgl_port_flush_dpi_vsync_ready_callback;"
                       lvgl_port_dsi_source_text
                       "${lvgl_port_dsi_source_text}")
        file(WRITE "${lvgl_port_dsi_source}" "${lvgl_port_dsi_source_text}")
    endif()
endif()

idf_component_get_property(lvgl_component_lib lvgl__lvgl COMPONENT_LIB)
target_compile_definitions(${lvgl_component_lib} PUBLIC LV_DRAW_SW_SUPPORT_RGB565_SWAPPED=1)
