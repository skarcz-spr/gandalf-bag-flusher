#include "Lps28dfw.h"

#define LPS28DFW_ADDRESS_LOW 0x5C
#define LPS28DFW_ADDRESS_HIGH 0x5D
#define LPS28DFW_WHO_AM_I_REGISTER 0x0F
#define LPS28DFW_WHO_AM_I_VALUE 0xB4
#define LPS28DFW_CTRL_REG1 0x10
#define LPS28DFW_CTRL_REG2 0x11
#define LPS28DFW_CTRL_REG3 0x12
#define LPS28DFW_PRESS_OUT_XL 0x28
#define LPS28DFW_CTRL2_SWRESET 0x04
// Block data update: the 24-bit result stays consistent during a multi-byte read.
#define LPS28DFW_CTRL2_BDU 0x08
#define LPS28DFW_CTRL3_IF_ADD_INC 0x01
// ODR 25 Hz (bits 6:3 = 0100), 16-sample averaging (bits 2:0 = 010).
#define LPS28DFW_CTRL1_VALUE ((4u << 3) | 2u)
// 1260 hPa full scale (FS_MODE = 0): 4096 LSB/hPa.
#define LPS28DFW_LSB_PER_HPA 4096.0f
#define LPS28DFW_MIN_VALID_HPA 200.0f
#define LPS28DFW_MAX_VALID_HPA 1300.0f
#define LPS28DFW_RESET_ATTEMPTS 10

static uint8_t lps28dfw_active_address = 0;

// Resets the sensor, then writes the configuration and reads it back.
// Arguments: device address. Returns: true when the registers hold the written values.
static bool lps28dfw_configure(uint8_t address) {
  uint8_t value = 0;
  uint8_t attempt;
  bool reset_done = false;

  if (!lps28dfw_bus_write_register(address, LPS28DFW_CTRL_REG2,
                                   LPS28DFW_CTRL2_SWRESET)) {
    return false;
  }

  for (attempt = 0; attempt < LPS28DFW_RESET_ATTEMPTS; ++attempt) {
    lps28dfw_bus_delay_ms(1);
    if (lps28dfw_bus_read_registers(address, LPS28DFW_CTRL_REG2, &value, 1) &&
        (value & LPS28DFW_CTRL2_SWRESET) == 0) {
      reset_done = true;
      break;
    }
  }
  if (!reset_done) {
    return false;
  }

  if (!lps28dfw_bus_write_register(address, LPS28DFW_CTRL_REG2,
                                   LPS28DFW_CTRL2_BDU) ||
      !lps28dfw_bus_write_register(address, LPS28DFW_CTRL_REG3,
                                   LPS28DFW_CTRL3_IF_ADD_INC) ||
      !lps28dfw_bus_write_register(address, LPS28DFW_CTRL_REG1,
                                   LPS28DFW_CTRL1_VALUE)) {
    return false;
  }

  return lps28dfw_bus_read_registers(address, LPS28DFW_CTRL_REG1, &value, 1) &&
         value == LPS28DFW_CTRL1_VALUE;
}

// Checks the chip ID at the given address.
// Arguments: device address. Returns: true when WHO_AM_I holds the LPS28DFW value (0xB4).
static bool lps28dfw_identify(uint8_t address) {
  uint8_t id = 0;

  return lps28dfw_bus_probe(address) &&
         lps28dfw_bus_read_registers(address, LPS28DFW_WHO_AM_I_REGISTER, &id,
                                     1) &&
         id == LPS28DFW_WHO_AM_I_VALUE;
}

// Looks for the sensor at addresses 0x5C and 0x5D and configures the first chip found.
// Arguments: none. Returns: true when the sensor responds and has been configured.
bool lps28dfw_begin(void) {
  uint8_t address;

  lps28dfw_active_address = 0;
  for (address = LPS28DFW_ADDRESS_LOW; address <= LPS28DFW_ADDRESS_HIGH;
       ++address) {
    if (lps28dfw_identify(address) && lps28dfw_configure(address)) {
      lps28dfw_active_address = address;
      return true;
    }
  }

  return false;
}

// Returns the address of the configured sensor, or 0 when no communication was established.
// Arguments: none.
uint8_t lps28dfw_address(void) {
  return lps28dfw_active_address;
}

// Reads the 24-bit pressure (two's complement) and converts it to hPa.
// Arguments: pointer to the result in hPa. Returns: true for a valid reading; a value outside
// the operating range (e.g. 0 right after power-up) is rejected as a sample that is not ready.
// A communication error causes the sensor to be searched for and configured again.
bool lps28dfw_read_pressure_hpa(float *hpa) {
  uint8_t data[3];
  int32_t raw;
  float value;

  if (lps28dfw_active_address == 0 && !lps28dfw_begin()) {
    return false;
  }

  if (!lps28dfw_bus_read_registers(lps28dfw_active_address,
                                   LPS28DFW_PRESS_OUT_XL, data, 3)) {
    lps28dfw_active_address = 0;
    return false;
  }

  raw = (int32_t)((uint32_t)data[0] | ((uint32_t)data[1] << 8) |
                  ((uint32_t)data[2] << 16));
  if ((raw & 0x800000) != 0) {
    raw -= 0x1000000;
  }

  value = (float)raw / LPS28DFW_LSB_PER_HPA;
  if (value < LPS28DFW_MIN_VALID_HPA || value > LPS28DFW_MAX_VALID_HPA) {
    return false;
  }

  *hpa = value;
  return true;
}
