#include <stdbool.h>
#include <stdint.h>

#include "esp_attr.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "display_config.h"
#include "newhaven_display_diagnostics.h"

#if NEWHAVEN_DISPLAY_DIAGNOSTICS
#if CONFIG_IDF_TARGET_ESP32P4 && CONFIG_ESP32P4_SELECTS_REV_LESS_V3
#include "soc/hp_sys_clkrst_struct.h"
#include "soc/mipi_dsi_bridge_struct.h"
#include "soc/mipi_dsi_host_struct.h"
#endif

static const char *TAG = "dsi_host_diag";
static DRAM_ATTR uint32_t refresh_count;
static bool refresh_callback_registered;

_Static_assert(__atomic_always_lock_free(sizeof(refresh_count), 0),
               "Refresh counter must use lock-free ISR-safe atomics");

static bool IRAM_ATTR on_refresh_done(esp_lcd_panel_handle_t panel,
                                     esp_lcd_dpi_panel_event_data_t *event_data,
                                     void *user_ctx)
{
    (void)panel;
    (void)event_data;
    (void)user_ctx;
    __atomic_fetch_add(&refresh_count, 1, __ATOMIC_RELAXED);
    return false;
}

void newhaven_display_diagnostics_register(esp_lcd_panel_handle_t panel)
{
    const esp_lcd_dpi_panel_event_callbacks_t callbacks = {
        .on_refresh_done = on_refresh_done,
    };
    const esp_err_t err = esp_lcd_dpi_panel_register_event_callbacks(panel, &callbacks, NULL);
    refresh_callback_registered = err == ESP_OK;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Refresh callback registration failed: %s; diagnostics will continue",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Refresh counter registered; pre-v3 IDF uses DMA completion, not bridge VSYNC");
        ESP_LOGI(TAG, "Refresh progress does not prove LCD reception or a displayed frame");
    }
}

uint32_t newhaven_display_refresh_count(void)
{
    return __atomic_load_n(&refresh_count, __ATOMIC_RELAXED);
}

void newhaven_display_log_host_snapshot(const char *stage)
{
    ESP_LOGI(TAG, "Host snapshot: %s; refresh_callback_registered=%u, total=%u",
             stage, refresh_callback_registered, (unsigned int)newhaven_display_refresh_count());
#if CONFIG_IDF_TARGET_ESP32P4 && CONFIG_ESP32P4_SELECTS_REV_LESS_V3
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    if (chip.model != CHIP_ESP32P4 || chip.revision < 100 || chip.revision >= 200 ||
        NEWHAVEN_DISPLAY_DSI_BUS_ID != 0) {
        ESP_LOGW(TAG, "Register snapshot skipped: supported only on ESP32-P4 v1.x, bus 0");
        return;
    }
    const uint32_t bus_clock = HP_SYS_CLKRST.soc_clk_ctrl1.reg_dsi_sys_clk_en;
    const uint32_t reset = HP_SYS_CLKRST.hp_rst_en0.reg_rst_en_dsi_brg;
    ESP_LOGI(TAG, "hw_ver1 headers; chip v%u.%u; RCC bus_clock=%u reset_asserted=%u",
             chip.revision / 100, chip.revision % 100,
             (unsigned int)bus_clock, (unsigned int)reset);
    if (!bus_clock || reset) {
        ESP_LOGW(TAG, "Host/bridge register reads skipped: bus clock disabled or reset asserted");
        return;
    }

    const dsi_host_dev_t *host = &MIPI_DSI_HOST;
    const dsi_brg_dev_t *bridge = &MIPI_DSI_BRIDGE;
    const dsi_host_pwr_up_reg_t power = {.val = host->pwr_up.val};
    const dsi_host_mode_cfg_reg_t mode = {.val = host->mode_cfg.val};
    const dsi_host_phy_rstz_reg_t phy_reset = {.val = host->phy_rstz.val};
    const dsi_host_phy_status_reg_t phy = {.val = host->phy_status.val};
    const dsi_host_phy_if_cfg_reg_t lanes = {.val = host->phy_if_cfg.val};
    const dsi_host_lpclk_ctrl_reg_t clock = {.val = host->lpclk_ctrl.val};
    const dsi_host_vid_mode_cfg_reg_t video = {.val = host->vid_mode_cfg.val};
    const dsi_host_dpi_color_coding_reg_t coding = {.val = host->dpi_color_coding.val};
    const dsi_host_cmd_pkt_status_reg_t command = {.val = host->cmd_pkt_status.val};
    const dsi_host_vid_pkt_status_reg_t packet = {.val = host->vid_pkt_status.val};
    const dsi_brg_int_raw_reg_t bridge_raw = {.val = bridge->int_raw.val};
    const dsi_brg_int_st_reg_t bridge_status = {.val = bridge->int_st.val};
    const dsi_brg_int_ena_reg_t bridge_enable = {.val = bridge->int_ena.val};

    ESP_LOGI(TAG, "RCC PHY cfg_clock=%u ref_clock=%u source_sel=%u (0=PLL_F20M,1=RC_FAST,2=PLL_F25M)",
             HP_SYS_CLKRST.peri_clk_ctrl03.reg_mipi_dsi_dphy_cfg_clk_en,
             HP_SYS_CLKRST.peri_clk_ctrl03.reg_mipi_dsi_dphy_pll_refclk_en,
             HP_SYS_CLKRST.peri_clk_ctrl02.reg_mipi_dsi_dphy_clk_src_sel);
    ESP_LOGI(TAG, "RCC DPI clock=%u source_sel=%u divider=%u",
             HP_SYS_CLKRST.peri_clk_ctrl03.reg_mipi_dsi_dpiclk_en,
             HP_SYS_CLKRST.peri_clk_ctrl03.reg_mipi_dsi_dpiclk_src_sel,
             HP_SYS_CLKRST.peri_clk_ctrl03.reg_mipi_dsi_dpiclk_div_num + 1);
    ESP_LOGI(TAG, "PWR_UP=0x%08X shutdownz=%u (1=on); MODE_CFG=0x%08X mode=%s",
             (unsigned int)power.val, power.shutdownz, (unsigned int)mode.val,
             mode.cmd_video_mode ? "command" : "video");
    ESP_LOGI(TAG, "PHY_RSTZ=0x%08X shutdownz=%u rstz=%u (1=released) enableclk=%u forcepll=%u",
             (unsigned int)phy_reset.val, phy_reset.phy_shutdownz, phy_reset.phy_rstz,
             phy_reset.phy_enableclk, phy_reset.phy_forcepll);
    ESP_LOGI(TAG, "PHY_STATUS=0x%08X pll_lock=%u direction=%u stop[clk,d0,d1]=%u,%u,%u",
             (unsigned int)phy.val, phy.phy_lock, phy.phy_direction,
             phy.phy_stopstateclklane, phy.phy_stopstate0lane, phy.phy_stopstate1lane);
    ESP_LOGI(TAG, "PHY ULPS active-not[clk,d0,d1]=%u,%u,%u rxulpsesc0=%u; "
             "stop bits need not be 1 during transmission",
             phy.phy_ulpsactivenotclk, phy.phy_ulpsactivenot0lane,
             phy.phy_ulpsactivenot1lane, phy.phy_rxulpsesc0lane);
    ESP_LOGI(TAG, "LPCLK_CTRL=0x%08X HS_request=%u auto_clock=%u; "
             "PHY_IF_CFG=0x%08X n_lanes=%u => %u data lanes",
             (unsigned int)clock.val, clock.phy_txrequestclkhs, clock.auto_clklane_ctrl,
             (unsigned int)lanes.val, lanes.n_lanes, lanes.n_lanes + 1);
    ESP_LOGI(TAG, "VID_MODE_CFG=0x%08X type=%u (0=nonburst-pulses,1=nonburst-events,2=burst-pulses) "
             "pattern_enable=%u mode=%u orientation=%u",
             (unsigned int)video.val, video.vid_mode_type, video.vpg_en,
             video.vpg_mode, video.vpg_orientation);
    ESP_LOGI(TAG, "LP permitted HBP/HFP=%u/%u VSA/VBP/VFP/VACT=%u/%u/%u/%u "
             "LP_command=%u frame_BTA_ACK=%u",
             video.lp_hbp_en, video.lp_hfp_en, video.lp_vsa_en, video.lp_vbp_en,
             video.lp_vfp_en, video.lp_vact_en, video.lp_cmd_en, video.frame_bta_ack_en);
    ESP_LOGI(TAG, "DPI_COLOR_CODING=0x%08X code=%u (5=RGB888) loosely18=%u VC=%u; "
             "packet_pixels=%u chunks=%u null_size=%u",
             (unsigned int)coding.val, coding.dpi_color_coding, coding.loosely18_en,
             host->dpi_vcid.dpi_vcid, host->vid_pkt_size.vid_pkt_size,
             host->vid_num_chunks.vid_num_chunks, host->vid_null_size.vid_null_size);
    ESP_LOGI(TAG, "Host H timing (lane-byte clock cycles): HSA=%u HBP=%u HLINE=%u",
             host->vid_hsa_time.vid_hsa_time, host->vid_hbp_time.vid_hbp_time,
             host->vid_hline_time.vid_hline_time);
    ESP_LOGI(TAG, "Host V timing (lines): VSA=%u VBP=%u VFP=%u VACTIVE=%u",
             host->vid_vsa_lines.vsa_lines, host->vid_vbp_lines.vbp_lines,
             host->vid_vfp_lines.vfp_lines, host->vid_vactive_lines.v_active_lines);
    ESP_LOGI(TAG, "ACT mirrors raw: coding=0x%08X video=0x%08X packet=0x%08X shadow_ctrl=0x%08X",
             (unsigned int)host->dpi_color_coding_act.val, (unsigned int)host->vid_mode_cfg_act.val,
             (unsigned int)host->vid_pkt_size_act.val, (unsigned int)host->vid_shadow_ctrl.val);
    ESP_LOGI(TAG, "ACT H timing: HSA=%u HBP=%u HLINE=%u; V timing: VSA=%u VBP=%u VFP=%u VACTIVE=%u",
             host->vid_hsa_time_act.vid_hsa_time_act, host->vid_hbp_time_act.vid_hbp_time_act,
             host->vid_hline_time_act.vid_hline_time_act, host->vid_vsa_lines_act.vsa_lines_act,
             host->vid_vbp_lines_act.vbp_lines_act, host->vid_vfp_lines_act.vfp_lines_act,
             host->vid_vactive_lines_act.v_active_lines_act);
    ESP_LOGI(TAG, "CMD_PKT_STATUS=0x%08X cmd_empty/full=%u/%u write_empty/full=%u/%u "
             "read_empty/full=%u/%u read_busy=%u",
             (unsigned int)command.val, command.gen_cmd_empty, command.gen_cmd_full,
             command.gen_pld_w_empty, command.gen_pld_w_full, command.gen_pld_r_empty,
             command.gen_pld_r_full, command.gen_rd_cmd_busy);
    ESP_LOGI(TAG, "VID_PKT_STATUS=0x%08X cmd_empty/full=%u/%u payload_empty/full=%u/%u "
             "buffer_empty/full=%u/%u",
             (unsigned int)packet.val, packet.dpi_cmd_w_empty, packet.dpi_cmd_w_full,
             packet.dpi_pld_w_empty, packet.dpi_pld_w_full,
             packet.dpi_buff_pld_empty, packet.dpi_buff_pld_full);
    ESP_LOGI(TAG, "Bridge enable=%u DPI_output=%u host_ref_cfg_clock=%u FIFO_depth=%u",
             bridge->en.dsi_en, bridge->dpi_misc_config.dpi_en,
             bridge->host_ctrl.dsi_cfg_ref_clk_en, bridge->fifo_flow_status.raw_buf_depth);
    ESP_LOGI(TAG, "Bridge INT_RAW=0x%08X INT_ST=0x%08X INT_ENA=0x%08X "
             "underrun[raw,masked,enabled]=%u,%u,%u (driver ISR may already have cleared it)",
             (unsigned int)bridge_raw.val, (unsigned int)bridge_status.val,
             (unsigned int)bridge_enable.val, bridge_raw.underrun_int_raw,
             bridge_status.underrun_int_st, bridge_enable.underrun_int_ena);
    ESP_LOGI(TAG, "Host INT_ST0/1 NOT READ: hw_ver1 headers say RO/NA; read-to-clear behavior "
             "not established. ACK/DPHY/timeout/ECC/CRC/FIFO error latches unavailable");
    ESP_LOGI(TAG, "Snapshot is non-atomic; control bits are not a waveform, frequency, or voltage measurement");
#else
    ESP_LOGW(TAG, "Register snapshot unavailable: requires ESP32-P4 pre-v3 register headers");
#endif
}
#endif
