// Bag Flushing Automation – firmware for the Raspberry Pi Pico (RP2040).
// Original author: Kamil Widzyk, student at AGH University of Krakow.

#include <Wire.h>

#include "ConfigStore.h"
#include "Lps28dfw.h"
#include "Procedure.h"
#include "Ui.h"

// Writes one byte to an LPS28DFW pressure sensor register over I2C.
// Arguments: device address, register number and value. Returns: true on ACK.
extern "C" bool lps28dfw_bus_write_register(
    uint8_t address,
    uint8_t reg,
    uint8_t value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

// Reads consecutive LPS28DFW registers (address auto-increment) using a repeated START.
// Arguments: device address, first register, buffer and byte count (1-8).
// Returns: true when all bytes were received.
extern "C" bool lps28dfw_bus_read_registers(
    uint8_t address,
    uint8_t reg,
    uint8_t *buffer,
    uint8_t length) {
  Wire.beginTransmission(address);
  Wire.write(reg);

  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  if (Wire.requestFrom(address, length) != length) {
    while (Wire.available()) {
      Wire.read();
    }
    return false;
  }

  for (uint8_t index = 0; index < length; ++index) {
    buffer[index] = Wire.read();
  }
  return true;
}

// Checks whether a device responds at the given I2C address.
// Arguments: device address. Returns: true when an ACK was received.
extern "C" bool lps28dfw_bus_probe(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

// Waits the given number of milliseconds during pressure sensor initialisation.
// Arguments: time in ms. Returns: nothing.
extern "C" void lps28dfw_bus_delay_ms(uint32_t milliseconds) {
  delay(milliseconds);
}

// Time without feeding after which the hardware watchdog resets the RP2040 (max approx. 8.3 s).
// Must be longer than the longest blocking operation in loop(): a flash write
// (EEPROM.commit) and an OLED refresh over I2C normally take less than 0.5 s.
constexpr uint32_t kWatchdogTimeoutMs = 2000;

// Initialises the settings store, the user interface and the procedure controller.
// Arguments: none. Returns: nothing; prepares the peripherals and firmware state.
void setup() {
  // Switch the valves and the pump off first: after any reset (including a watchdog
  // reset) the transistors must stay off before the rest of the initialisation runs.
  procedureOutputsOff();

  configStoreBegin();
  uiBegin();
  lps28dfw_begin();
  procedureBegin();

  // The watchdog starts after initialisation because sensor detection uses delay().
  rp2040.wdt_begin(kWatchdogTimeoutMs);
}

// Runs one non-blocking pass over inputs, UI, procedure and settings save.
// Arguments: none. Returns: nothing; each module updates its own state.
void loop() {
  // Feed the watchdog. If the loop hangs, the RP2040 is reset
  // and setup() switches all outputs off again.
  rp2040.wdt_reset();

  processInput();
  updateScreen();
  procedureUpdate();
  updateScreen();
  configStoreService();
}