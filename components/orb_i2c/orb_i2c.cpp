// The Orb OS — the board's one I2C bus. See orb_i2c.h.
#include "orb_i2c.h"
#include "board.h"
#include "esp_log.h"

static const char *TAG = "i2c";

// 400 kHz, as Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000) set it (src/imu_qmi8658.cpp).
static constexpr uint32_t I2C_HZ = 400000;
// A chip that does not answer within this is treated as absent rather than waited on.
static constexpr int I2C_TIMEOUT_MS = 50;

static i2c_master_bus_handle_t s_bus = nullptr;

i2c_master_bus_handle_t orb_i2c::bus() {
    if (s_bus) return s_bus;
    i2c_master_bus_config_t cfg = {};
    cfg.i2c_port          = I2C_NUM_0;
    cfg.sda_io_num        = BOARD_I2C_SDA;
    cfg.scl_io_num        = BOARD_I2C_SCL;
    cfg.clk_source        = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    cfg.flags.enable_internal_pullup = true;
    const esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bus on SDA %d / SCL %d: %s", BOARD_I2C_SDA, BOARD_I2C_SCL, esp_err_to_name(err));
        s_bus = nullptr;
    }
    return s_bus;
}

i2c_master_dev_handle_t orb_i2c::device(uint8_t addr) {
    if (!bus()) return nullptr;
    i2c_device_config_t dev = {};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address  = addr;
    dev.scl_speed_hz    = I2C_HZ;
    i2c_master_dev_handle_t h = nullptr;
    if (i2c_master_bus_add_device(s_bus, &dev, &h) != ESP_OK) return nullptr;
    return h;
}

bool orb_i2c::present(uint8_t addr) {
    return bus() && i2c_master_probe(s_bus, addr, I2C_TIMEOUT_MS) == ESP_OK;
}
