// The Orb OS — CO5300 AMOLED over QSPI, driven by esp_lcd, with LVGL 8 on esp_lvgl_port.
//
// Implements src/display.h, the contract the shared UI code calls, for ESP-IDF (stage 2 of
// REWRITE_PLAN.md). The panel sequence is Waveshare's (board package
// waveshare/esp32_s3_touch_amoled_1_75 3.0.1). The sizes, offsets and the rules below are the
// ones the Arduino firmware has run on real Orbs: src/display.cpp is the reference for what
// this panel needs, and each choice here that differs from it says why.
#include "display.h"
#include "display_idf.h"
#include "board.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_co5300.h"
#include "esp_lvgl_port.h"

static const char *TAG = "display";

// Log and fail. Not ESP_RETURN_ON_ERROR: that returns the esp_err_t, and in a function
// returning bool every error code converts to true, so a dead panel would report success.
#define TRY(expr, what) do {                                              \
        const esp_err_t err_ = (expr);                                    \
        if (err_ != ESP_OK) {                                             \
            ESP_LOGE(TAG, "%s failed: %s", what, esp_err_to_name(err_));  \
            return false;                                                 \
        }                                                                 \
    } while (0)

// The partial draw buffer: 40 full-width lines in internal DMA RAM, one buffer, exactly as
// the Arduino firmware has it (LVGL_BUF_LINES). Rendering into PSRAM was measured slower
// than the QSPI link itself, and a second buffer would cost 37 KB of the internal RAM the
// network stack competes for.
static constexpr int DRAW_BUF_LINES = 40;

// (Not config.h's LCD_QSPI_HZ, which is the Arduino build's 80 MHz.)
// 40 MHz, the vendor's figure and esp_lcd_co5300's default. The Arduino build runs this
// panel at 80 MHz, but that was tuned on Arduino_GFX; on a new driver, first light comes
// at the clock the vendor stands behind, and 80 is a measured step afterwards.
// Not config.h's LCD_QSPI_HZ, which is the Arduino build's 80 MHz.
static constexpr int PANEL_QSPI_HZ = 40 * 1000 * 1000;

// Waveshare's init sequence for this panel. The column window starts at 6 (0x0006..0x01D7),
// which is the BOARD_LCD_COL_OFFSET gap set again below for every later window.
static const co5300_lcd_init_cmd_t kInitCmds[] = {
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},           // RGB565
    {0x35, (uint8_t[]){0x00}, 1, 0},           // tearing-effect output on
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},           // brightness, corrected straight after init
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 600},
    {0x11, nullptr, 0, 600},                   // sleep out
    {0x29, nullptr, 0, 0},                     // display on
};

static esp_lcd_panel_handle_t s_panel = nullptr;
static lv_disp_t             *s_disp  = nullptr;
static lv_disp_drv_t         *s_drv   = nullptr;
static void (*s_portFlush)(lv_disp_drv_t *, const lv_area_t *, lv_color_t *) = nullptr;
static uint16_t               s_rotation = 0;
static volatile bool          s_quiet = false;

// The counters src/display.h promises, for /health and [memdbg] once those are ported.
// One writer (the flush, in the port task); readers only want a trend, so plain words.
static uint32_t s_frames = 0, s_flushUs = 0, s_flushedPx = 0;

// The CO5300 only accepts windows that start on an even pixel and end on an odd one, in
// both axes. Anything else draws the strip one pixel off. Same rule as src/display.cpp.
static void rounder(lv_area_t *area) {
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

// LVGL renders RGB565 low byte first (LV_COLOR_16_SWAP is 0 in the shared lv_conf.h,
// because baked theme art in flash is stored that way) and the CO5300 takes high byte
// first. Arduino_GFX swapped on the way out; esp_lvgl_port's LVGL 8 flush does not, so the
// swap happens here, in place, before its flush hands the strip to DMA. In place is safe:
// LVGL redraws the whole buffer before it next uses it.
static void swap_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px) {
    const int64_t t0 = esp_timer_get_time();
    const size_t n = (size_t)(area->x2 - area->x1 + 1) * (size_t)(area->y2 - area->y1 + 1);
    uint16_t *p = (uint16_t *)px;
    for (size_t i = 0; i < n; ++i) p[i] = (uint16_t)((p[i] << 8) | (p[i] >> 8));
    s_flushedPx += n;
    if (lv_disp_flush_is_last(drv)) ++s_frames;
    s_portFlush(drv, area, px);
    s_flushUs += (uint32_t)(esp_timer_get_time() - t0);
}

bool display::begin() {
    const size_t bufPx = (size_t)BOARD_LCD_H_RES * DRAW_BUF_LINES;

    ESP_LOGI(TAG, "CO5300 over QSPI at %d MHz", PANEL_QSPI_HZ / 1000000);
    // Field by field rather than through CO5300_PANEL_BUS_QSPI_CONFIG and
    // CO5300_PANEL_IO_QSPI_CONFIG: those are C designated initializers in an order C++
    // rejects. Same values as the macros in esp_lcd_co5300 2.2.0, except that the unused
    // data4..7 lines are -1 ("not connected") instead of the macro's implicit 0, which is
    // GPIO0, the BOOT button.
    spi_bus_config_t bus = {};
    bus.sclk_io_num     = BOARD_LCD_PCLK;
    bus.data0_io_num    = BOARD_LCD_D0;
    bus.data1_io_num    = BOARD_LCD_D1;
    bus.data2_io_num    = BOARD_LCD_D2;
    bus.data3_io_num    = BOARD_LCD_D3;
    bus.data4_io_num    = -1;
    bus.data5_io_num    = -1;
    bus.data6_io_num    = -1;
    bus.data7_io_num    = -1;
    bus.max_transfer_sz = bufPx * sizeof(uint16_t);
    TRY(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), "SPI bus");

    esp_lcd_panel_io_handle_t io = nullptr;
    esp_lcd_panel_io_spi_config_t ioCfg = {};
    ioCfg.cs_gpio_num       = BOARD_LCD_CS;
    ioCfg.dc_gpio_num       = -1;
    ioCfg.spi_mode          = 0;
    ioCfg.pclk_hz           = PANEL_QSPI_HZ;
    ioCfg.trans_queue_depth = 10;
    ioCfg.lcd_cmd_bits      = 32;   // QSPI: opcode, then the command in the middle byte
    ioCfg.lcd_param_bits    = 8;
    ioCfg.flags.quad_mode   = 1;
    TRY(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &ioCfg, &io), "panel IO");

    co5300_vendor_config_t vendor = {};
    vendor.init_cmds      = kInitCmds;
    vendor.init_cmds_size = sizeof(kInitCmds) / sizeof(kInitCmds[0]);
    vendor.flags.use_qspi_interface = 1;
    esp_lcd_panel_dev_config_t panelCfg = {};
    panelCfg.reset_gpio_num = BOARD_LCD_RST;
    panelCfg.rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB;
    panelCfg.bits_per_pixel = 16;
    panelCfg.vendor_config  = &vendor;
    TRY(esp_lcd_new_panel_co5300(io, &panelCfg, &s_panel), "panel");
    TRY(esp_lcd_panel_reset(s_panel), "reset");
    TRY(esp_lcd_panel_init(s_panel), "init");
    TRY(esp_lcd_panel_set_gap(s_panel, BOARD_LCD_COL_OFFSET, BOARD_LCD_ROW_OFFSET), "gap");
    TRY(esp_lcd_panel_disp_on_off(s_panel, true), "on");

    // LVGL on core 1, as on Arduino: core 0 belongs to WiFi and the network task.
    lvgl_port_cfg_t portCfg = ESP_LVGL_PORT_INIT_CONFIG();
    portCfg.task_affinity = 1;
    // LVGL's clock (lv_conf.h leaves the tick to the port here). 1 ms, as millis() was on
    // Arduino, rather than the port's default 5: shared code times animations off it.
    portCfg.timer_period_ms = 1;
    TRY(lvgl_port_init(&portCfg), "LVGL port");

    lvgl_port_display_cfg_t dispCfg = {};
    dispCfg.io_handle     = io;
    dispCfg.panel_handle  = s_panel;
    dispCfg.buffer_size   = bufPx;
    dispCfg.double_buffer = false;
    dispCfg.hres          = BOARD_LCD_H_RES;
    dispCfg.vres          = BOARD_LCD_V_RES;
    dispCfg.rounder_cb    = rounder;
    dispCfg.flags.buff_dma = 1;
    if (!lvgl_port_lock(0)) return false;
    s_disp = lvgl_port_add_disp(&dispCfg);
    if (s_disp) {
        s_drv = s_disp->driver;
        s_portFlush = s_drv->flush_cb;
        s_drv->flush_cb = swap_flush;
    }
    lvgl_port_unlock();
    if (!s_disp) {
        ESP_LOGE(TAG, "LVGL display registration failed");
        return false;
    }
    ESP_LOGI(TAG, "up: %dx%d, draw buffer %u lines (%u B internal)",
             BOARD_LCD_H_RES, BOARD_LCD_V_RES, (unsigned)DRAW_BUF_LINES,
             (unsigned)(bufPx * sizeof(uint16_t)));
    return true;
}

// LVGL renders in esp_lvgl_port's task here, during the main loop's sleep (see
// display_idf.h), so there is nothing to pump. Kept so shared code that calls it links.
void display::loop() {}

void display::markInput(uint32_t) {}

void display::setBrightness(uint8_t level) {
    if (!s_panel) return;
    // The driver takes a percentage and scales it back to 0..255 itself, so round to the
    // nearest percent; the idle level of 25/255 becomes 10% and lands on 25 again.
    const uint8_t pct = (uint8_t)(((unsigned)level * 100 + 127) / 255);
    esp_lcd_panel_co5300_set_brightness(s_panel, pct);
}

uint32_t display::inactiveMs() { return s_disp ? lv_disp_get_inactive_time(s_disp) : 0; }
void display::noteActivity() { if (s_disp) lv_disp_trig_activity(s_disp); }

// Arbitrary rotation needs src/display.cpp's PSRAM framebuffer path, which is not ported
// yet. Every Orb so far runs at 0; anything else is refused out loud rather than drawn wrong.
void display::setRotation(uint16_t degrees) {
    degrees %= 360;
    if (degrees != 0) {
        ESP_LOGW(TAG, "rotation %u° requested; only 0° is ported so far, staying at 0°", degrees);
        return;
    }
    s_rotation = 0;
}
uint16_t display::rotation() { return s_rotation; }

uint32_t display_frames()     { return s_frames; }
uint32_t display_lvgl_us()    { return 0; }   // LVGL runs in esp_lvgl_port's task; not measured yet
uint32_t display_flush_us()   { return s_flushUs; }
uint32_t display_flushed_px() { return s_flushedPx; }
uint32_t display_pushed_px()  { return s_flushedPx; }
uint32_t display_te_wait_us() { return 0; }
// The staged flush and the TE wait are Arduino_GFX-era fixes for tearing. Not ported yet:
// both report off, and asking for them changes nothing.
void display_set_stage(bool) {}
bool display_stage(void) { return false; }
void display_set_te(bool) {}
bool display_te(void) { return false; }

void orb_log_set_quiet(bool quiet) { s_quiet = quiet; }
bool orb_log_quiet() { return s_quiet; }

bool display_idf::lock(uint32_t timeoutMs) { return lvgl_port_lock(timeoutMs); }
void display_idf::unlock() { lvgl_port_unlock(); }
