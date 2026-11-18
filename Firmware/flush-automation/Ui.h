#ifndef UI_H
#define UI_H

#include <stdint.h>

#define SCREEN_HOME 0
#define SCREEN_SETTINGS 1
#define SCREEN_STATUS 2
#define SCREEN_SETTINGS_NUMBER_EDIT 3
#define SCREEN_SETTINGS_CONFIRM 4
#define SCREEN_TEST 5
#define MAX_CUTOFF_CONFIRM_SAMPLES 50
#define DEFAULT_CUTOFF_CONFIRM_SAMPLES 10
// Bag pressure threshold (hPa) below which the pump is cut off.
#define PRESSURE_CUTOFF_MIN_HPA 260
#define PRESSURE_CUTOFF_MAX_HPA 1260
#define DEFAULT_PRESSURE_CUTOFF_HPA 850
#define MAX_EMPTY_TIME_SECONDS 5999

enum StatusPhase : uint8_t {
  STATUS_EMPTY,
  STATUS_FILL
};

// Result of the last procedure, shown as an icon on the home screen.
enum LastRunResult : uint8_t {
  LAST_RUN_PENDING,  // bags need flushing: after power-up, a manual fill/empty
                     // or while a procedure runs (warning triangle)
  LAST_RUN_DONE,     // all cycles completed (check mark)
  LAST_RUN_STOPPED   // aborted with STOP (cross)
};

struct Config {
  uint8_t cycles;
  // fill_time is tenths of a second; max_fill_time is seconds.
  uint16_t fill_time;
  uint16_t max_fill_time;
  // empty_time and max_empty_time are whole seconds (max 99m59s).
  uint16_t empty_time;
  uint16_t max_empty_time;
  uint32_t actuation_time;
  // Number of consecutive below-threshold pressure samples needed to confirm a cutoff.
  uint8_t cutoff_confirm_samples;
  // Cutoff pressure threshold in hPa (cutoff when the pressure is below the threshold).
  uint16_t pressure_cutoff_hpa;
};

extern Config config;
extern const Config defaultConfig;
extern int currentScreen;

// Initialises I2C, the user inputs and the OLED display.
// Arguments/return value: none; configures the hardware and the UI state.
void uiBegin();
// Runs one pass over the buttons, the encoder and the manual buttons.
// Arguments/return value: none; may change the screen, configuration and outputs.
void processInput();
// Redraws a changed screen if the OLED is ready and the minimum interval has passed.
// Arguments/return value: none; updates the display and the redraw flags.
void updateScreen();
// Switches the UI to the status screen and sets the initial procedure data.
// Arguments: phase, current cycle number, number of cycles, elapsed time and maximum in seconds.
// Return value: none; changes the screen and the status data.
void showStatusScreen(StatusPhase phase, uint8_t currentCycle,
                      uint8_t allCycles, uint32_t elapsedSeconds,
                      uint32_t maximumSeconds);
// Updates the status data and marks the screen for redraw when something changed.
// Arguments: phase, current cycle number, number of cycles, elapsed time and maximum in seconds.
// Return value: none; modifies the status state and, if needed, the redraw flag.
void updateStatusScreen(StatusPhase phase, uint8_t currentCycle,
                        uint8_t allCycles, uint32_t elapsedSeconds,
                        uint32_t maximumSeconds);
// Returns to the home screen and clears the selection.
// Arguments/return value: none; changes the screen and UI state.
void returnHomeScreen();

// Sets the result icon shown on the home screen. Kept in RAM only, so a power
// cycle or reset brings back LAST_RUN_PENDING.
// Arguments: the result. Return value: none; marks the screen for redraw.
void setLastRunResult(LastRunResult result);

#endif