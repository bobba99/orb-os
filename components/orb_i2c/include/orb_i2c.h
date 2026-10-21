// The Orb OS — the board's one I2C bus (RTC, IMU, PMIC, codec, touch), on ESP-IDF.
//
// One bus, brought up once, at the Arduino firmware's 400 kHz. Every chip driver asks this
// for its device handle instead of creating a bus of its own, which on ESP-IDF's i2c_master
// driver would fail for the second caller on the same pins.
#pragma once
#include <stdint.h>
#include "driver/i2c_master.h"

namespace orb_i2c {

// The bus, created on first use. Null if it could not be created (logged).
i2c_master_bus_handle_t bus();

// A device on the bus at `addr`, or null if the bus is down. Does not probe; see present().
i2c_master_dev_handle_t device(uint8_t addr);

// Does anything acknowledge `addr`? What Arduino's beginTransmission/endTransmission() == 0 was.
bool present(uint8_t addr);

}  // namespace orb_i2c
