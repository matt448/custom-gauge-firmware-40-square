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

idf_component_get_property(lvgl_port_dir espressif__esp_lvgl_port COMPONENT_DIR)
idf_component_get_property(lvgl_port_version espressif__esp_lvgl_port COMPONENT_VERSION)
idf_component_get_property(esp_lcd_dir esp_lcd COMPONENT_DIR)

if(NOT lvgl_port_version STREQUAL "2.9.0")
    message(FATAL_ERROR
        "The DSI completion compatibility patch is verified for esp_lvgl_port 2.9.0, "
        "but found '${lvgl_port_version}'. Update the versioned patch before changing the dependency.")
endif()

if(NOT TARGET lvgl_port_lib)
    message(FATAL_ERROR "esp_lvgl_port 2.9.0 did not create its expected lvgl_port_lib target.")
endif()

set(lvgl_port_dsi_source "${lvgl_port_dir}/src/lvgl9/esp_lvgl_port_disp.c")
set(esp_lcd_dsi_header "${esp_lcd_dir}/dsi/include/esp_lcd_mipi_dsi.h")
if(NOT EXISTS "${lvgl_port_dsi_source}" OR NOT EXISTS "${esp_lcd_dsi_header}")
    message(FATAL_ERROR
        "Cannot verify the ESP-LCD DSI completion API or esp_lvgl_port DSI source.")
endif()

file(READ "${esp_lcd_dsi_header}" esp_lcd_dsi_header_text)
string(FIND "${esp_lcd_dsi_header_text}"
    "esp_lcd_dpi_panel_refresh_done_cb_t on_refresh_done"
    esp_lcd_refresh_callback_index)
if(esp_lcd_refresh_callback_index EQUAL -1)
    message(FATAL_ERROR
        "The installed ESP-LCD DSI API lacks on_refresh_done; LVGL tear-avoidance completion is unavailable.")
endif()

file(READ "${lvgl_port_dsi_source}" lvgl_port_dsi_source_text)
set(lvgl_port_invalid_dsi_assignment
    "cbs.on_frame_buf_complete = lvgl_port_flush_dpi_vsync_ready_callback;")
set(lvgl_port_valid_dsi_assignment
    "cbs.on_refresh_done = lvgl_port_flush_dpi_vsync_ready_callback;")
string(LENGTH "${lvgl_port_dsi_source_text}" lvgl_port_source_length)
string(LENGTH "${lvgl_port_invalid_dsi_assignment}" lvgl_port_invalid_assignment_length)
string(LENGTH "${lvgl_port_valid_dsi_assignment}" lvgl_port_valid_assignment_length)
string(REPLACE "${lvgl_port_invalid_dsi_assignment}" ""
    lvgl_port_without_invalid_callbacks "${lvgl_port_dsi_source_text}")
string(REPLACE "${lvgl_port_valid_dsi_assignment}" ""
    lvgl_port_without_valid_callbacks "${lvgl_port_dsi_source_text}")
string(LENGTH "${lvgl_port_without_invalid_callbacks}" lvgl_port_without_invalid_length)
string(LENGTH "${lvgl_port_without_valid_callbacks}" lvgl_port_without_valid_length)
math(EXPR lvgl_port_invalid_dsi_callback_count
    "(${lvgl_port_source_length} - ${lvgl_port_without_invalid_length}) / ${lvgl_port_invalid_assignment_length}")
math(EXPR lvgl_port_valid_dsi_callback_count
    "(${lvgl_port_source_length} - ${lvgl_port_without_valid_length}) / ${lvgl_port_valid_assignment_length}")

if(lvgl_port_invalid_dsi_callback_count EQUAL 2 AND
   lvgl_port_valid_dsi_callback_count EQUAL 0)
    string(REPLACE "${lvgl_port_invalid_dsi_assignment}"
        "${lvgl_port_valid_dsi_assignment}" lvgl_port_dsi_source_text
        "${lvgl_port_dsi_source_text}")
elseif(lvgl_port_invalid_dsi_callback_count EQUAL 0 AND
       lvgl_port_valid_dsi_callback_count EQUAL 2)
    message(STATUS "esp_lvgl_port DSI completion callback is already ESP-IDF compatible")
else()
    message(FATAL_ERROR
        "Unexpected esp_lvgl_port 2.9.0 DSI completion callbacks; refusing to build without verified tear-avoidance synchronization.")
endif()

set(lvgl_port_patched_dir "${CMAKE_BINARY_DIR}/lvgl_port_compat")
set(lvgl_port_patched_source "${lvgl_port_patched_dir}/esp_lvgl_port_disp.c")
file(MAKE_DIRECTORY "${lvgl_port_patched_dir}")
file(WRITE "${lvgl_port_patched_source}" "${lvgl_port_dsi_source_text}")

get_target_property(lvgl_port_sources lvgl_port_lib SOURCES)
set(lvgl_port_sources_without_original_disp)
set(lvgl_port_original_disp_count 0)
foreach(lvgl_port_source IN LISTS lvgl_port_sources)
    get_filename_component(lvgl_port_source_name "${lvgl_port_source}" NAME)
    if(lvgl_port_source_name STREQUAL "esp_lvgl_port_disp.c")
        math(EXPR lvgl_port_original_disp_count "${lvgl_port_original_disp_count} + 1")
    else()
        list(APPEND lvgl_port_sources_without_original_disp "${lvgl_port_source}")
    endif()
endforeach()
if(NOT lvgl_port_original_disp_count EQUAL 1)
    message(FATAL_ERROR
        "Expected exactly one original LVGL 9 display source in lvgl_port_lib; refusing to build an unverified DSI path.")
endif()
set_property(TARGET lvgl_port_lib PROPERTY SOURCES
    "${lvgl_port_sources_without_original_disp}")
target_sources(lvgl_port_lib PRIVATE "${lvgl_port_patched_source}")
message(STATUS
    "Compiling verified esp_lvgl_port 2.9.0 DSI completion source from the build directory")
