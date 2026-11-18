#ifndef PROCEDURE_H
#define PROCEDURE_H

#include <stdint.h>

// Test mode diagnostic snapshot (LPS28DFW pressure sensor).
struct PressureTestInfo {
  int32_t pressureTenthsHpa;
  int32_t pressureThresholdHpa;
  uint8_t confirmSamples;
  uint8_t pressureLowSamples;
  bool hasPressure;
  bool pressureOk;
  bool pressureCutoffDetected;
  bool pumpOn;
};

// Initialises the GPIOs and plays the startup sound.
// Arguments/return value: none; prepares the procedure control state.
void procedureBegin();

// Sets the Z1, Z2 and pump pins as outputs driven low (everything off).
void procedureOutputsOff();
// Starts the procedure unless it is already running.
// Arguments/return value: none; initialises the state machine, timer and status screen.
void procedureStart();
// Stops the procedure, switches the outputs off and returns to the home screen.
// Arguments/return value: none; deactivates the state machine and signals the stop.
void procedureStop();
// Sets the manual fill/empty outputs; conflicting requests switch the outputs off.
// Arguments: fill and empty flags. Return value: none; changes the GPIOs and machine state.
void procedureSetManualOutputs(bool filling, bool emptying);
// Updates the buzzer and advances the active procedure state machine.
// Arguments/return value: none; may change stages, outputs and the status screen.
void procedureUpdate();
// Plays the short encoder-turn sound.
// Arguments/return value: none; replaces the current buzzer sequence.
void procedureEncoderTurnBeep();
// Plays the encoder-press sound.
// Arguments/return value: none; replaces the current buzzer sequence.
void procedureEncoderPressBeep();
// Plays the out-of-range input error sound.
// Arguments/return value: none; replaces the current buzzer sequence.
void procedureEncoderErrorBeep();
// Requests a software reset after the confirmation sound has played.
// Arguments/return value: none; sets the reset flag in the sound sequence.
void procedureRequestSoftwareReset();
// Samples the pressure in test mode (same logic as during emptying).
// Arguments/return value: none; called on every test mode loop iteration.
void procedureTestUpdate();
// Fills the diagnostic snapshot: pressure, threshold and below-threshold sample counter.
// Arguments: pointer to the result. Return value: none.
void procedureTestGetInfo(PressureTestInfo *info);
// Resets the cutoff detection state after the parameters were changed manually.
// Arguments/return value: none.
void procedureApplyCutoffConfig();

#endif
