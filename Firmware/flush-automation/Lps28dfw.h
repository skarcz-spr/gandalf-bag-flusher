#ifndef LPS28DFW_H
#define LPS28DFW_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Writes one byte to a pressure sensor register over the I2C bus.
// Arguments: device address, register address and value. Returns: true on ACK.
bool lps28dfw_bus_write_register(uint8_t address, uint8_t reg, uint8_t value);
// Reads consecutive sensor registers (with address auto-increment) using a repeated START.
// Arguments: device address, first register address, buffer and byte count.
// Returns: true when all bytes were received.
bool lps28dfw_bus_read_registers(uint8_t address, uint8_t reg, uint8_t *buffer,
                                 uint8_t length);
// Checks whether a device responds at the given I2C address.
// Arguments: device address. Returns: true when an ACK was received.
bool lps28dfw_bus_probe(uint8_t address);
// Waits the given number of milliseconds (used only during sensor initialisation).
// Arguments: time in ms. Returns: nothing.
void lps28dfw_bus_delay_ms(uint32_t milliseconds);

// Looks for the LPS28DFW at address 0x5C or 0x5D (SA0), checks WHO_AM_I and configures
// continuous measurement (25 Hz, 16-sample averaging, 1260 hPa full scale).
// Arguments: none. Returns: true when the sensor responds and has been configured.
bool lps28dfw_begin(void);
// Returns the address of the configured sensor, or 0 when no communication was established.
// Arguments: none.
uint8_t lps28dfw_address(void);
// Reads the absolute pressure in hPa. After a communication error the sensor is
// initialised again on the next call.
// Arguments: pointer to the result in hPa. Returns: true when the reading is valid and within
// the sensor's operating range (a sample that is not ready is rejected); stores it in *hpa.
bool lps28dfw_read_pressure_hpa(float *hpa);

#ifdef __cplusplus
}
#endif

#endif
