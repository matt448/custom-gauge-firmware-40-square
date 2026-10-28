# Newhaven NHD-4.0-480480AF-MSXP ESP32-P4 gauge driver

Native ESP-IDF firmware for the 4.0-inch Newhaven NHD-4.0-480480AF-MSXP gauge
driver board. The 480 x 480 ST7701SN LCD uses both ESP32-P4 MIPI DSI data
lanes; GPIO9 controls its active-low RESETX signal and GPIO8 supplies a PWM
DIM signal to the TPS92511DDA backlight driver.

## ESP-IDF requirement

This project was written and checked against **ESP-IDF v6.0.2**. The connected
ESP32-P4 is revision v1.3, so `sdkconfig.defaults` selects the mutually
exclusive pre-v3 SoC family with `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` and a
minimum revision of v1.0. Pre-v3 silicon requires the 360 MHz CPU setting,
which is also pinned in the defaults. Its legacy DSI PHY requires the
`PLL_F20M` reference clock; the application selects ESP-IDF's
`MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT_LEGACY` rather than the v3+ XTAL default.
It uses the native `esp_lcd` MIPI DSI APIs:

- `esp_lcd_new_dsi_bus()`
- `esp_lcd_new_panel_io_dbi()`
- `esp_lcd_new_panel_dpi()`
- `esp_lcd_dpi_panel_get_frame_buffer()`

The reference installed example is
`examples/peripherals/lcd/mipi_dsi/main/mipi_dsi_lcd_example_main.c`.

## Build, flash, and monitor

Open an ESP-IDF shell with ESP-IDF v6.0.2 active, then run:

```text
idf.py set-target esp32p4
idf.py build
idf.py -p COMx flash monitor
```

Replace `COMx` with the serial port selected for the Waveshare ESP32-P4-WIFI6
board. Exit the monitor with `Ctrl+]`.

## Flash storage and partitions

The module has 16 MiB of physical flash. [`sdkconfig.defaults`](sdkconfig.defaults)
pins `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y` and selects the project-local
[`partitions.csv`](partitions.csv), so regenerating the ignored `sdkconfig`
retains the flash capacity and partition layout.

| Partition | Offset | Size | Purpose |
| --- | --- | --- | --- |
| `nvs` | `0x9000` | `0x6000` (24 KiB) | Existing settings/data, unchanged |
| `phy_init` | `0xF000` | `0x1000` (4 KiB) | Existing PHY data, unchanged |
| `factory` | `0x10000` | `0x400000` (4 MiB) | Application |

The partition table remains at `0x8000`. The factory application partition
has grown from 1 MiB to 4 MiB, giving the current roughly 746 KiB image ample
growth room. Its starting offset is unchanged. The remaining `0xBF0000` bytes
(11.9375 MiB), starting at `0x410000`, are unallocated for future storage.
OTA was not configured; this retains the single factory app and does not
introduce OTA slots or move existing data partitions.

The flash header capacity and application partition size are separate:
selecting 16 MiB alone would not enlarge the app partition. An existing
`sdkconfig` also retains its saved choices unless updated; this change updates
both the active configuration and regeneration defaults.

## CAN reception

The gauge uses the ESP32-P4's internal TWAI controller through U2, the external
TI TCAN3404-Q1 transceiver. It is not an SPI CAN interface. The application uses
only ESP-IDF **6.0.2's new public `esp_twai.h` / `esp_twai_onchip.h` API**;
it does not use or mix in the legacy `driver/twai.h` API.

### Wiring and bus termination

| Connection | MCU GPIO | Module pad | Transceiver connection |
| --- | --- | --- | --- |
| CAN_TX | **GPIO5** | 20 | U2 pin 1, TXD |
| CAN_RX | **GPIO6** | 21 | U2 pin 4, RXD |

Firmware uses GPIO numbers **5 and 6**, not pad numbers 20 and 21. U2 pin 3
is 3.3 V and pin 2 is ground; SHDN (pin 5) and STB (pin 8) are grounded.
There is no GPIO-controlled enable to configure. U2 pin 7 is CANH and pin 6
is CANL. Connect CANH/CANL with the appropriate twisted-pair bus wiring and a
common signal-ground reference between the gauge and ECU.

CAN needs **one 120-ohm terminator at each of the two physical bus ends**,
not one at every gauge. This PCB includes **R5, 120 ohms**: populate/use it
only when this board is a bus endpoint that needs termination. For multiple
gauges, intermediate boards must not add R5 termination. Account for any ECU
or other endpoint terminator already fitted. With the network unpowered,
two parallel 120-ohm terminations normally measure about 60 ohms across
CANH/CANL. Do not add a third terminator merely because another gauge is added.

### Configuration and behavior

[`main/can_config.h`](main/can_config.h) centralizes GPIOs, **500000 bit/s
Classical CAN**, operating mode, diagnostic rate, bounded queue sizes, and the
future MegaSquirt decoder's realtime base ID. There is no CAN FD data-phase
configuration or bitrate switching.

Default `CAN_LISTEN_ONLY=0` is **normal CAN mode**. There are no application
frame transmissions, but the controller **acknowledges valid received frames**.
That ACK is essential if the ECU and this gauge are the only active nodes.
The installed driver requires a TX queue of at least one entry in normal
mode; the module allocates that minimum but never submits a transmit request.

Set `CAN_LISTEN_ONLY=1` for passive diagnostics. Listen-only sends **no ACK**:
another normal-mode receiver must acknowledge the ECU's transmissions.
Otherwise the ECU may retry and accumulate transmission errors; an ECU and a
listen-only gauge alone are not a correctly acknowledged network.

The receiver starts once after gauge-renderer initialization. A separate
priority-1 task receives/processes CAN without waiting in the rendering loop.
The synthetic oil-pressure animation is unchanged; no CAN value drives it yet.
CAN startup failure is logged and published as `CAN_RECEIVER_FAILED`, not
passed to `ESP_ERROR_CHECK`, so the gauge continues rendering. Missing traffic
is a valid idle state, not an initialization failure.

The accept-all filter uses IDF's documented-in-source P4 HAL full-open case
(`id=0`, `mask=0`), including standard **and** extended identifiers and RTR.
The ISR copies each received frame into owned queue storage before returning.
Each record contains its identifier, STD/EXT flag, RTR flag, original DLC,
payload length/bytes, and a 1-MHz reception timestamp in microseconds.
RTR records have zero payload length even if their DLC requests data.
Classical DLC values above eight retain their raw DLC but expose at most
eight data bytes. Unexpected FD/BRS records are counted and not dispatched.

The RX queue holds 128 complete records; state transitions have a separate
16-entry queue. Both use static internal-DRAM storage. Callbacks perform
only ISR-safe reception, atomic counter updates, and nonblocking
`xQueueSendFromISR`; they do not log, allocate, decode, or block.
Task-context diagnostics default to one summary every **5 seconds**, at most
**3 raw frame samples** and **4 transition logs per interval**. Change
`CAN_DIAGNOSTICS_ENABLED` to disable routine summaries/samples; errors and
recovery requests still surface. Suppressed transition logs and dropped
state events are counted separately.

Diagnostics report cumulative received frames (including software queue
drops), frames/second averaged over the actual report interval, software
queue drops, receive errors, unexpected frame types, cumulative bus errors,
error flags, TEC/REC, controller state, recovery requests, and age of the last
received frame. Application counters remain cumulative across recovery even
though IDF resets its own bus-error history on recovery. ISR counters are
lock-free 32-bit values accumulated into task-owned 64-bit totals; the status
snapshot is updated approximately every 100 ms. Time-since-RX uses tick
differences and is intended for ordinary timeout intervals, not a bus idle
for an entire tick-counter wrap period.

The installed public driver does **not** expose hardware RX FIFO-overrun
counts. The logs explicitly label that measurement unavailable: zero software
queue drops does not prove that the hardware lost no frames.

On bus-off, the CAN task calls the installed driver's `twai_node_recover()`
once for that episode and waits for state-change/status evidence of recovery.
Hardware must observe 128 occurrences of 11 recessive bits. It does not
reinstall the driver, enqueue frames, or restart the display. Attempts for
different episodes are separated by at least 10 seconds, and a failed request
is not retried in a tight loop. A bus that never recovers remains visibly
`recovering`/`bus_off` in the periodic status.

### Expected logs and future decoder interface

With or without an ECU, startup should include:

```text
can_rx: Classical CAN 500000 bit/s TX=GPIO5 RX=GPIO6 mode=normal (ACK enabled); no application TX
can_rx: Accept STD/EXT and RTR; RX queue=128; timestamps=1 MHz; hardware RX-overrun count unavailable
can_rx: Realtime broadcast reference base=1520 (0x5F0); no ECU decoding or gauge updates
```

With no traffic, summaries show `rx=0 rate=0.0 fps last_rx=never`, normally
`state=error_active`. An active controller does not establish that an ECU is
connected or broadcasting. When traffic arrives, counts/rate increase and
sample lines show `t=... us ID=0x... STD/EXT RTR=... DLC=... data=...`.
RTR lines explicitly say `<RTR: no payload>`. Bus errors/state transitions
point to checking bitrate, termination, wiring, ground reference, and ACKs;
the module does not interpret silence as a particular wiring fault.

[`main/can_receiver.h`](main/can_receiver.h) provides:

- `can_receiver_start(handler, context)`: called once; an optional handler
  receives owned frame records in the CAN task. The handler must return
  promptly and copy any record it retains. Main currently passes `NULL`.
- `can_receiver_get_status(&status)`: a task-safe status snapshot, including
  initialization errors and frame/drop/error counts, for a future UI.

A separate MegaSquirt decoder can consume the timestamped records without
changing the TWAI ISR. Do not add DCS reads to expose CAN status: post-panel-init
DCS reads previously forced command mode and blanked the display.

### MegaSquirt MS3 Pro 1.6.2 / TunerStudio setup

Reception does **not** enable broadcasting on the ECU. Use the actual MS3 Pro
**1.6.2 ECU definition** in TunerStudio and back up the tune before changing ECU
settings. For this integration, choose **CAN Realtime Data Broadcasting**, not
**Simplified Dash Broadcasting**:

- Realtime broadcasting's default base is **1520 decimal (`0x5F0`)**.
  Group N uses standard 11-bit ID **base + N**, with big-endian multi-byte
  values and the realtime group's documented layout.
- Simplified Dash Broadcasting uses a different, fixed dash layout and its own
  default base identifiers. Do not apply realtime group offsets, scalings, or
  channel mappings to those frames. No simplified-dash decoder is implemented.
- `CAN_MS_REALTIME_BASE_ID` is configurable reference metadata for a future
  decoder; changing it neither configures the ECU nor restricts raw reception.

In TunerStudio's **CAN-bus/Testmodes** menu (wording depends on the loaded ECU
definition), open **CAN Realtime Data Broadcasting**:

1. Enable realtime broadcasting; set/confirm the base identifier as **1520**,
   or choose another valid standard-ID base and match the firmware reference.
   Keep the ECU's CAN-node ID separate from this broadcast base.
2. Enable the documented realtime data groups you want to inspect and choose
   a nonzero broadcast rate. Start with a small selection at a modest rate
   (for example 10 Hz if offered), not every group at its maximum rate.
3. Use the ECU's **500 kbit/s** CAN bus. If its definition exposes a bitrate
   option, confirm it matches; changing this gauge's bitrate cannot configure
   the ECU.
4. Burn/save the settings to the ECU and follow any ECU-definition prompt
   requiring a restart. Confirm actual IDs, payloads, and rates in the gauge's
   sampled CAN logs before selecting/scaling any sensor channel.

Reference: [MegaSquirt CAN broadcast protocol](https://www.msextra.com/doc/pdf/Megasquirt_CAN_Broadcast.pdf)
and the matching MS3 Pro 1.6.2/TunerStudio ECU definition. The firmware does not
assume an oil-pressure ID, sensor-channel assignment, scale, or unit.

## Required hardware wiring

| LCD FFC pin | Function | Connection |
| --- | --- | --- |
| 4 | VDD | Regulated display logic supply; **do not exceed 3.3 V** |
| 5, 8, 11, 14 | GND | Common ground |
| 6, 7 | DN0, DP0 | ESP32-P4 dedicated DSI data lane 0 negative, positive |
| 9, 10 | CN, CP | ESP32-P4 dedicated DSI clock negative, positive |
| 12, 13 | DN1, DP1 | ESP32-P4 dedicated DSI data lane 1 negative, positive |
| 15 | RESETX | ESP32-P4 GPIO9; active low |
| 21 | IM0 | Tie high for MIPI DSI |
| 22 | IM1 | Tie low for MIPI DSI |
| 23 | IM3 | Tied high on this board; panel permits either level in MIPI DSI mode |
| 24 | LANSEL | Tie high for two-lane MIPI DSI |

IM0=1 and IM1=0 select MIPI DSI. IM3 is high on this board, and LANSEL=1
selects two data lanes. Both DSI data lanes are connected and configured.

Pins 1 (LED_K) and 2 (LED_A) require an **external constant-current backlight
driver**. The board's TPS92511DDA drives those pins; GPIO8 connects only to its
DIM input and is configured as 20 kHz, 10-bit PWM. Do not connect either LED
pin directly to an ESP32-P4 GPIO.

## Configuration

All panel settings are in [`main/display_config.h`](main/display_config.h).
RESETX on FFC pin 15 is configured for ESP32-P4 GPIO9, and
`NEWHAVEN_DISPLAY_RESET_GPIO_CONFIGURED` is enabled.

The reset sequence drives RESETX high, drives it low for 10 ms, releases it
high, then waits 120 ms before any DSI command transaction. It assumes the
specified external 10 kOhm pull-up to display logic VDD.
The panel's reset timing table specifies a minimum low pulse of 10 us, not
10 ms; the current 10 ms pulse provides margin. The earlier 1 ms pulse also
exceeded that minimum, so pulse width alone was not a demonstrated mismatch.

Compile-time settings include:

- `NEWHAVEN_DISPLAY_COLOR_ORDER`: RGB or BGR test-pixel byte order
- `NEWHAVEN_DISPLAY_ROTATION`: 0, 90, 180, or 270 degrees
- `NEWHAVEN_DISPLAY_RESET_GPIO` (GPIO9)
- `NEWHAVEN_DISPLAY_DIAGNOSTICS`: log DCS registers before panel initialization,
  host snapshots, refresh counters, MCU RESETX input readback, and link/LDO settings
- `NEWHAVEN_DISPLAY_HARDWARE_PATTERN_MS`: optional DSI host color-bar test;
  disabled (`0`) by default; use `5000` for a 5-second test
- `NEWHAVEN_DISPLAY_STARTUP_COLOR_TEST_MS`: optional black, red, green, blue,
  and white tests; disabled (`0`) by default; use `2000` for 2 seconds per color
  and keep above 1000 ms for refresh sampling
- `NEWHAVEN_BACKLIGHT_PWM_GPIO` (GPIO8)
- `NEWHAVEN_BACKLIGHT_DEFAULT_PERCENT` (0 through 100)
- `NEWHAVEN_DISPLAY_FB_COUNT`: one or two PSRAM-backed framebuffers
- `NEWHAVEN_DISPLAY_PIXEL_FORMAT`: RGB888 (default) or RGB565

ESP-IDF v6.0.2 supports RGB888 and RGB565 for the ESP32-P4 MIPI DSI DPI
driver. RGB888 is the default. The driver allocates display framebuffers in
DMA-capable PSRAM; `sdkconfig.defaults` enables the board's PSRAM support.

Rotation is applied while drawing the diagnostic framebuffer. Do not send an
unverified ST7701SN orientation/MADCTL command solely to rotate the panel.

The DSI PHY is powered before DSI bus creation through ESP32-P4 internal LDO
channel 3 at 2.5 V. This follows the installed ESP-IDF MIPI DSI LCD example
and Espressif's ST7701 MIPI test. It powers only `VDD_MIPI_DPHY`; it does not
power the LCD's VDD or the external backlight driver.

### Firmware diagnostics

Startup first logs raw DCS reads for display ID (0x04), DSI error count (0x05),
power mode (0x0A), address mode (0x0B), pixel format (0x0C), display mode
(0x0D), and self diagnostic (0x0F), only before panel initialization.
Read errors are reported without aborting these optional diagnostics.
ESP-IDF's DSI read API returning success does not validate the response.
Zero values are normal for registers such as the DSI error count. The ST7701SN
controller manual lists FF FF FF as the default display ID, so that value is
not an error or proof of communication. Pixel-format readback 0x70 reports
RGB888 in bits 6:4; its unused lower bits read zero even after writing 0x77.
Neither `00` nor `FF` is inherently invalid, and a zero parity-error count
does not prove video reception.

**Controlled no-post-init-read test:** the installed ESP-IDF 6.0.2
`mipi_dsi_hal_host_gen_read_short_packet()` selects command mode and does not
restore video mode. Previous logs showed video mode immediately after panel
initialization, but command mode during both visual tests after diagnostic
reads. All application DCS reads after `esp_lcd_panel_init()` returns have
therefore been removed, including post-initialization status and per-color
pixel/error/status reads. Pre-initialization reads remain. The managed driver's
ID read occurs inside panel initialization, before DPI initialization enables
video; no IDF or managed-component changes are needed.

After flashing, test from a fresh reboot. Host snapshots should report
`mode=command` after bus creation, then `mode=video` after panel initialization,
during hardware bars, and during the red framebuffer test. Snapshots only read
host registers and do not perform DCS transactions or restore/change mode.

Earlier captures established power mode changing from 0x08 to 0x9C and
pixel format reading 0x70. Those post-initialization DCS values are not sampled
in this test.
The ST7701SN documentation is inconsistent about availability of 0x06-0x08
over MIPI. Their previously observed all-zero responses are inconclusive and
must not be used to conclude that video is absent.

The display was confirmed working after removing post-initialization DCS
reads. Startup now proceeds directly from backlight initialization to the
gauge, with hardware bars and solid-color tests disabled. Pre-initialization
DCS reads, host snapshots, and the refresh counter remain enabled; visual-test
snapshots and counter-delta reports run only when those tests are re-enabled.
The optional tests fill and present every framebuffer through the existing
API. These tests preserve the initialization sequence, GPIO assignments,
lane count/rate, pixel format, clock settings, and video timings.

Visible hardware bars but no framebuffer colors would narrow
the investigation to the framebuffer/bridge path. Neither being visible leaves
panel commands, link configuration, and electrical connectivity under investigation.
MCU RESETX readback does not verify continuity to the panel. The LDO log is
the software voltage request, not a measurement of the internal module rail.

### ESP32-P4 host snapshots and refresh progress

With `NEWHAVEN_DISPLAY_DIAGNOSTICS` enabled, `dsi_host_diag` logs read-only
snapshots after DSI bus creation and after panel initialization. When the
optional visual tests are enabled, snapshots also run during hardware color
bars and the red framebuffer test. No framebuffer synchronization
changes are made. The existing initialization, GPIOs, two lanes at 500 Mbps,
25 MHz RGB888 video, forced-HS clock mode, and video timings are unchanged.

The implementation uses the installed ESP-IDF 6.0.2 `soc` register structures.
`CONFIG_ESP32P4_SELECTS_REV_LESS_V3` selects `register/hw_ver1` in IDF's
`components/soc/CMakeLists.txt`; a runtime chip-revision check limits snapshots
to v1.x and bus 0. No hardcoded register addresses, opaque driver-handle casts,
PHY test-interface accesses, managed-component edits, or IDF edits are used.
Snapshot reads are skipped with a warning if the revision is unsupported or
the peripheral bus clock/reset state prevents safe access.

The logs report:

- RCC clock gates, reset, clock-source selection, and DPI divider.
- Host power and command/video mode; PHY enable/reset, PLL lock, clock-lane
  HS request/automatic control, lane count, stop states, and ULPS status.
- Programmed video packet size, virtual channel, RGB color coding, burst and
  low-power settings, hardware pattern settings, and horizontal/vertical
  timings. Horizontal host timings are **lane-byte clock cycles**, not pixels;
  vertical timings are lines. There is no separate host HFP register.
- Active (`ACT`) timing/configuration mirrors and shadow control, command and
  video FIFO status, bridge enable/DPI output, FIFO depth, and bridge underrun.

Bus creation already waited for PLL lock and initial lane stop states; the
snapshot is a later observation, not a new bring-up test. Stop bits need not
remain asserted while video/continuous clock is active. Clock control bits
and PLL lock do not measure physical waveforms, actual lane frequency, supply
voltage, or panel reception. Snapshots are sequential, not atomic or a trace.

**Host error-register limitation:** the installed hw_ver1 host headers label
`INT_ST0`/`INT_ST1` fields `RO` with descriptions `NA`, but do not establish
whether reading consumes/clears error latches. To honor the no-clear constraint,
diagnostics deliberately do **not** read them. Host ACK/DPHY, timeout, ECC/CRC,
packet, and FIFO error-latch values therefore remain unavailable, not zero.
Only header-defined FIFO/status fields and bridge underrun are decoded.
The bridge has explicit write-to-clear interrupt definitions and IDF reads
`INT_ST` separately from clearing it. Diagnostics read bridge RAW/ST/ENA once
per snapshot and never write clear/mask/force/configuration registers. The
existing IDF bridge ISR still clears its own interrupts, so a sampled zero
does not rule out earlier underruns. No interrupt history is claimed.

The application registers `on_refresh_done` before panel initialization.
There were no existing application/DPI callbacks to replace; IDF's internal
DMA callbacks remain untouched. Keep future application event callbacks in
the same registration because IDF's registration API replaces the entire set.
Registration failure is logged and nonfatal.

On pre-v3 P4, IDF sets `MIPI_DSI_BRG_LL_EVENT_VSYNC` to zero and invokes
`on_refresh_done` from the DMA full-transfer completion callback. The new
callback only increments a lock-free 32-bit counter in internal DRAM and
returns false; it is in IRAM and performs no logging, allocation, waiting,
or DCS transactions. Task-context atomic loads report counter deltas over
approximately one second for hardware bars and the red framebuffer.
Sampling uses the existing test intervals; the normal gauge resumes afterward.

For example, a positive framebuffer delta means the host/DMA pipeline is
advancing; it does **not** prove the LCD received/displayed a frame. At the
configured timing, roughly 60 callbacks per second is plausible during normal
framebuffer scanout, not a pass/fail threshold. A zero delta during host
patterns can be expected: IDF disables bridge DPI output while its host pattern
generator bypasses the bridge, so DMA completions can slow or stop. A small
nonzero pattern delta can include in-flight DMA. Check callback registration,
mode, PLL lock, FIFO/bridge state, and the test stage before interpreting zeros.

## Video timing and lane configuration

| Setting | Value |
| --- | ---: |
| DSI data lanes | 2 (lanes 0 and 1) |
| Pixel clock | 25 MHz |
| Active resolution | 480 x 480 |
| Horizontal front porch | 150 |
| Horizontal back porch | 150 |
| Horizontal sync pulse | 10 |
| Vertical front porch | 20 |
| Vertical back porch | 20 |
| Vertical sync pulse | 10 |

These are Newhaven's published recommended MIPI timing values: 790 horizontal
clocks and 530 vertical lines per frame. The configured 500 Mbps lane rate
carries the 25 MHz RGB888 stream across both data lanes with DSI protocol
margin.

ESP-IDF v6.0.2 uses burst video with sync pulses. Video blanking permits
low-power transitions by default. These are driver defaults, not requirements
established by the Newhaven timing table.

### DSI clock A/B test

`NEWHAVEN_DISPLAY_DSI_CLOCK_FORCE_HS` in `main/display_config.h` selects the
clock-lane experiment:

- `0`: automatic HS/LP clock transitions (the previous black-screen baseline).
- `1`: continuous high-speed clock (the current test).

Rebuild and flash after changing this setting. The boot log identifies the
selected mode. Leave the lane rate, video timing, low-power blanking settings,
and panel commands unchanged so only clock-lane behavior differs.

For this optional experiment, first re-enable the visual tests with intervals
of `5000` ms for hardware bars and `2000` ms per framebuffer color. After boot,
watch for the bars, five colors, then the gauge. If continuous clock produces an image, repeat with
`0` and then `1` to confirm the result is reproducible. If both modes remain
black, this test has not identified a fix; it does not prove a hardware fault
or verify the physical clock waveform. Measuring that waveform would require
suitable high-speed differential probing equipment.

## ST7701SN initialization status

This project depends on `espressif/esp_lcd_st7701` v2.0.2~2 from Espressif's
`esp-iot-solution` repository, commit
`b9b4cc9fa85345b46af366a002785e7c6e8ef45d`. It is an ESP32-P4-capable MIPI
DSI driver and accepts a custom `st7701_lcd_init_cmd_t` vendor command array.
Its MIPI initialization routine transmits each entry over the native DSI
command channel with `esp_lcd_panel_io_tx_param()`; no external SPI wiring is
used.

[`newhaven_display.c`](main/newhaven_display.c) contains the exact ST7701SN
register sequence published by Newhaven for this panel:

<https://support.newhavendisplay.com/hc/en-us/community/posts/39646987900567-Any-initialization-code-for-NHD-4-0-480480AF-MSXP-CTP>

The published reference sends register commands through a separate command
transport. This project retains native ESP32-P4 MIPI DSI video output and sends
the panel register commands through the DSI DBI command channel.

The Newhaven sequence contains generic DCS commands (`0x11`, `0x29`, and
`0x36`), ST7701SN command-page/control commands, and its panel-specific
gamma, voltage, gate, and source settings. It preserves the published 120 ms
Sleep Out delay and 10 ms intermediate E8 delay. The port additionally waits
120 ms after Display On; that delay is not specified by the published example.
The managed driver also writes MADCTL and COLMOD before the vendor array,
and the application sends Display On again after initialization.

## Expected startup logs

With the Newhaven initialization sequence:

```text
I (....) main: Starting Newhaven NHD-4.0-480480AF-MSXP bring-up
I (....) newhaven_display: Configuring and resetting RESETX
I (....) newhaven_display: Creating two-lane MIPI DSI bus on data lanes 0 and 1
I (....) newhaven_display: Creating MIPI DSI command interface
I (....) newhaven_display: Creating 480x480 MIPI DSI ST7701SN panel with 2 framebuffer(s)
I (....) newhaven_display: Initializing ST7701SN controller
I (....) newhaven_display: Sending Newhaven NHD-4.0-480480AF-MSXP ST7701SN initialization sequence
I (....) backlight: Configured TPS92511DDA DIM PWM on GPIO8 at 20000 Hz
```

The driver then starts MIPI DSI video output and animates the supplied needle
sprite over the embedded oil-pressure face at
[`assets/oil_pressure_gauge.rgb565`](assets/oil_pressure_gauge.rgb565). The
face is a centered 480 x 480, big-endian RGB565 image (460,800 bytes); its
source PNG may be smaller than the display. The needle is an RGBA8888 sprite.
Generate both embedded assets from their source PNGs with:

```powershell
python tools\png_to_rgb565.py assets\oil_pressure_gauge.png --canvas-size 480 -o assets\oil_pressure_gauge.rgb565
python tools\png_to_rgb565.py assets\needle.png --format rgba8888 -o assets\needle.rgba8888
```

Rebuild and flash after replacing either source asset. The application expands
the face into both RGB888 framebuffers at startup. Each 30 FPS frame restores
the previous needle sprite's rotated bounds from the RGB565 face, alpha-blends
the newly rotated sprite into the inactive framebuffer, then presents that
completed framebuffer to DSI scanout.

## Gauge animation

The independent `gauge_renderer` module currently generates a synthetic test
value that sweeps from 0 to 150 PSI over approximately 1.5 seconds and
returns to 0 PSI over the next 1.5 seconds, then holds at 0 PSI. Its
displayed value is set directly during the startup sweep so it reaches both
endpoints exactly.

The needle sprite's pivot is `(5, 200)`: centered horizontally and 200 pixels
from its top edge. It therefore has a 200-pixel forward length and a 49-pixel
counterweight. Values map linearly from 0 PSI at 225 degrees (lower-left) to
150 PSI at -45 degrees (lower-right), following the face's counterclockwise
scale. The renderer uses Cartesian angles, so it negates the display-space Y
sine term; these constants can be calibrated later without changing the panel
setup.

The default framebuffer count is two. The DSI driver scans one PSRAM buffer
while the renderer updates the other, eliminating visible tearing and avoiding
the memory-bandwidth spikes that can cause a DSI-bridge blue underrun frame.

## Troubleshooting

- **Blank display:** Confirm VDD is within range, the backlight driver is
  enabled independently, the reset GPIO is correct, IM straps are 1/0/1, and
  verify the panel supply rails and lane wiring.
- **White display:** Check RESETX wiring and the table's sleep-out/display-on
  sequence. A lit white panel can indicate that backlight power is present
  while the controller was never initialized.
- **Flickering display:** Verify DSI clock/data lane differential routing,
  the board's DSI PHY power source, PSRAM configuration, lane rate, and
  25 MHz pixel-clock timing.
- **Shifted or clipped image:** Recheck all porch and sync values in
  `display_config.h` and confirm the source PNG is exactly 480 x 480.
- **Swapped red and blue:** Change `NEWHAVEN_DISPLAY_COLOR_ORDER` and, once
  documented, add the corresponding ST7701SN controller color-order command
  to the verified initialization table.

# Newhaven example code
GitHub: https://github.com/newhavendisplay
List of Example Code: https://support.newhavendisplay.com/hc/en-us/categories/4409527834135-Example-Code/
4.0" Display initialization: https://support.newhavendisplay.com/hc/en-us/community/posts/39646987900567-Any-initialization-code-for-NHD-4-0-480480AF-MSXP-CTP