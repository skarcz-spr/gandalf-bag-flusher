#include <Arduino.h>
#include <hardware/watchdog.h>
#include <math.h>
#include <string.h>

#include "Lps28dfw.h"
#include "ConfigStore.h"
#include "Procedure.h"
#include "Ui.h"

namespace {

constexpr uint8_t kBuzzerPin = 0;
constexpr uint8_t kZ1Pin = 8;
constexpr uint8_t kZ2Pin = 9;
constexpr uint8_t kPumpPin = 10;
// The pump cutoff (end of emptying) is detected by the LPS28DFW pressure sensor: the bag pressure
// drops below config.pressure_cutoff_hpa. The pressure is sampled every 100 ms.
constexpr uint32_t kPressureSampleIntervalMs = 100;

// The number of consecutive samples that must meet the cutoff condition comes from
// config.cutoff_confirm_samples.
// Returns the current number of cutoff confirmation samples, limited to the allowed range.
// Arguments: none. Returns: the number of samples (1 - MAX_CUTOFF_CONFIRM_SAMPLES).
uint8_t confirmSamples() {
  const uint8_t samples = config.cutoff_confirm_samples;
  if (samples < 1) {
    return 1;
  }
  return samples > MAX_CUTOFF_CONFIRM_SAMPLES ? MAX_CUTOFF_CONFIRM_SAMPLES
                                              : samples;
}
constexpr uint16_t kShortBeepMs = 100;
constexpr uint16_t kFastBeepMs = 80;
constexpr uint16_t kFastBeepGapMs = 80;
constexpr uint8_t kMaxBeepSteps = 8;

enum MachineState : uint8_t {
  MACHINE_IDLE = 0,
  MACHINE_FILL = 1,
  MACHINE_EMPTY = 2
};

enum ProcedureStage : uint8_t {
  STAGE_IDLE,
  STAGE_START_PENDING,
  STAGE_FILL_ACTUATION,
  STAGE_FILL_DURATION,
  STAGE_FILL_IDLE_SETTLE,
  STAGE_CYCLE_EMPTY_ACTUATION,
  STAGE_CYCLE_EMPTY_MONITOR,
  STAGE_CYCLE_IDLE_SETTLE
};

struct BeepStep {
  uint16_t frequency;
  uint16_t durationMs;
  uint16_t gapMs;
};

constexpr BeepStep kEndSequence[] = {
  {2000, 333, 100},
  {2000, 333, 100},
  {2000, 500, 0},
};

struct Beeper {
  BeepStep steps[kMaxBeepSteps];
  uint8_t stepCount;
  uint8_t stepIndex;
  uint32_t nextChangeMs;
  bool active;
  bool sounding;
  bool rebootAfterSequence;
};

MachineState machineState = MACHINE_IDLE;
ProcedureStage stage = STAGE_IDLE;
Beeper beeper = {};
bool procedureActive = false;
uint32_t procedureStartedAt = 0;
uint32_t maximumProcedureMs = 0;
uint32_t stageDeadline = 0;
uint32_t emptyStartedAt = 0;
uint32_t lastDisplayedSecond = UINT32_MAX;
uint8_t currentCycle = 0;
StatusPhase displayedPhase = STATUS_EMPTY;
bool hasPressureSample = false;
bool lastPressureOk = false;
float lastPressureHpa = 0.0f;
uint8_t pressureLowCount = 0;
uint32_t lastPressureSampleAt = 0;

// Checks whether a deadline has been reached, taking millis() overflow into account.
// Arguments: current time and deadline in ms. Returns: true when the deadline has passed.
bool deadlineReached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

// Rounds a number of milliseconds up to whole seconds.
// Arguments: time in ms. Returns: the rounded time in seconds.
uint32_t roundedSeconds(uint32_t milliseconds) {
  return (milliseconds + 999) / 1000;
}

// Calculates the time since the current procedure started.
// Arguments: current time in ms. Returns: the elapsed time in whole seconds.
uint32_t procedureElapsedSeconds(uint32_t now) {
  return (now - procedureStartedAt) / 1000;
}

// Converts the maximum procedure duration to seconds for display.
// Arguments: none. Returns: the rounded time limit in seconds.
uint32_t statusMaximumSeconds() {
  return roundedSeconds(maximumProcedureMs);
}

// Passes the phase, cycle and time to the UI when the second changed or an update is forced.
// Arguments: current time in ms and an optional force flag. Returns: nothing; may
// update lastDisplayedSecond and the status screen data.
void publishStatus(uint32_t now, bool force = false) {
  if (!procedureActive) {
    return;
  }

  const uint32_t elapsed = procedureElapsedSeconds(now);
  if (force || elapsed != lastDisplayedSecond) {
    lastDisplayedSecond = elapsed;
    updateStatusScreen(displayedPhase, currentCycle > 0 ? currentCycle : 1,
                       config.cycles, elapsed, statusMaximumSeconds());
  }
}

// Switches all outputs off, sets the outputs for the new state and publishes the phase.
// Arguments: new machine state and current time in ms. Returns: nothing; changes the GPIOs,
// machineState, displayedPhase and the status screen state.
void setMachineState(MachineState nextState, uint32_t now) {
  digitalWrite(kPumpPin, LOW);
  digitalWrite(kZ1Pin, LOW);
  digitalWrite(kZ2Pin, LOW);
  machineState = nextState;

  switch (machineState) {
    case MACHINE_FILL:
      digitalWrite(kZ1Pin, HIGH);
      displayedPhase = STATUS_FILL;
      break;
    case MACHINE_EMPTY:
      digitalWrite(kZ2Pin, HIGH);
      digitalWrite(kPumpPin, HIGH);
      displayedPhase = STATUS_EMPTY;
      break;
    case MACHINE_IDLE:
    default:
      break;
  }

  publishStatus(now, true);
}

// Copies a limited number of sound steps and starts the sequence without blocking.
// Arguments: array of steps, their count and the current time in ms.
// Returns: nothing; replaces the current sequence and resets the buzzer state.
void startBeepSequence(const BeepStep *steps, uint8_t count, uint32_t now) {
  noTone(kBuzzerPin);
  beeper.stepCount = count > kMaxBeepSteps ? kMaxBeepSteps : count;
  beeper.stepIndex = 0;
  for (uint8_t i = 0; i < beeper.stepCount; ++i) {
    beeper.steps[i] = steps[i];
  }
  beeper.nextChangeMs = now;
  beeper.active = beeper.stepCount > 0;
  beeper.sounding = false;
  beeper.rebootAfterSequence = false;
}

// Builds a repeated 1 kHz beep pattern and hands it over for playback.
// Arguments: number of beeps, beep length, gap (ms) and current time (ms).
// Returns: nothing; replaces the current buzzer sequence.
void startBeepPattern(uint8_t count, uint16_t onTimeMs,
                      uint16_t offTimeMs, uint32_t now) {
  BeepStep steps[kMaxBeepSteps];
  const uint8_t stepCount = count > kMaxBeepSteps ? kMaxBeepSteps : count;
  for (uint8_t i = 0; i < stepCount; ++i) {
    steps[i] = {1000, onTimeMs, offTimeMs};
  }
  startBeepSequence(steps, stepCount, now);
}

// Switches the buzzer and outputs off, then resets the RP2040 through the watchdog.
// Arguments: none. Does not return; if the watchdog call returns, it stays in a loop.
[[noreturn]] void performSoftwareReset() {
  noTone(kBuzzerPin);
  digitalWrite(kPumpPin, LOW);
  digitalWrite(kZ1Pin, LOW);
  digitalWrite(kZ2Pin, LOW);
  watchdog_reboot(0, 0, 0);
  while (true) {
  }
}

// Performs a due buzzer sequence state change without blocking.
// Arguments: current time in ms. Returns: nothing; changes the buzzer and sequence state,
// and after a reset sequence it may reset the device.
void updateBeeper(uint32_t now) {
  if (!beeper.active || !deadlineReached(now, beeper.nextChangeMs)) {
    return;
  }

  if (!beeper.sounding) {
    tone(kBuzzerPin, beeper.steps[beeper.stepIndex].frequency);
    beeper.sounding = true;
    beeper.nextChangeMs =
        now + beeper.steps[beeper.stepIndex].durationMs;
    return;
  }

  noTone(kBuzzerPin);
  beeper.sounding = false;
  const uint16_t gapMs = beeper.steps[beeper.stepIndex].gapMs;
  ++beeper.stepIndex;
  if (beeper.stepIndex >= beeper.stepCount) {
    beeper.active = false;
    if (beeper.rebootAfterSequence) {
      performSoftwareReset();
    }
  } else {
    beeper.nextChangeMs = now + gapMs;
  }
}

// Estimates the maximum procedure duration (initial empty + all cycles).
// Arguments: none. Returns: the time in ms limited to the uint32_t range.
uint32_t calculateMaximumProcedureMs() {
  const uint64_t actuation = config.actuation_time;
  const uint64_t emptyDuration =
      static_cast<uint64_t>(config.empty_time) * 1000;
  const uint64_t fillDuration =
      static_cast<uint64_t>(config.fill_time) * 100;
  const uint64_t initialEmpty = 2 * actuation + emptyDuration;
  const uint64_t total = initialEmpty + static_cast<uint64_t>(config.cycles) *
                             (4 * actuation + fillDuration + emptyDuration);

  if (total > UINT32_MAX) {
    return UINT32_MAX;
  }
  return static_cast<uint32_t>(total);
}

// Starts the next cycle: increments the counter, signals the start and begins filling.
// Arguments: current time in ms. Returns: nothing; changes the cycle, outputs, stage and deadline.
void beginCycleFill(uint32_t now) {
  if (currentCycle < config.cycles) {
    ++currentCycle;
  }
  startBeepPattern(1, kShortBeepMs, 0, now);
  setMachineState(MACHINE_FILL, now);
  stage = STAGE_FILL_ACTUATION;
  stageDeadline = now + config.actuation_time;
}

// Starts timing the empty step and clears the below-threshold pressure sample counter.
// Arguments: current time in ms. Returns: nothing; changes the stage.
void beginPumpMonitor(uint32_t now) {
  emptyStartedAt = now;
  lastPressureSampleAt = now - kPressureSampleIntervalMs;
  pressureLowCount = 0;
  stage = STAGE_CYCLE_EMPTY_MONITOR;
}

// Ends emptying and starts the pause before the next cycle or the end.
// Arguments: current time in ms. Returns: nothing; switches the outputs off and sets the stage/deadline.
void completeCycleEmpty(uint32_t now) {
  setMachineState(MACHINE_IDLE, now);
  stage = STAGE_CYCLE_IDLE_SETTLE;
  stageDeadline = now + config.actuation_time;
}

// Ends the procedure, switches the outputs off, signals success and returns to the home screen.
// Arguments: current time in ms. Returns: nothing; deactivates the procedure and resets the stage.
void finishProcedure(uint32_t now) {
  setMachineState(MACHINE_IDLE, now);
  procedureActive = false;
  stage = STAGE_IDLE;
  startBeepSequence(kEndSequence,
                    static_cast<uint8_t>(sizeof(kEndSequence) / sizeof(kEndSequence[0])),
                    now);
  setLastRunResult(LAST_RUN_DONE);
  returnHomeScreen();
}

// Returns the cutoff pressure threshold from the configuration, limited to the sensor's range.
// Arguments: none. Returns: the threshold in hPa.
uint16_t pressureThresholdHpa() {
  const uint16_t threshold = config.pressure_cutoff_hpa;
  if (threshold < PRESSURE_CUTOFF_MIN_HPA) {
    return PRESSURE_CUTOFF_MIN_HPA;
  }
  return threshold > PRESSURE_CUTOFF_MAX_HPA ? PRESSURE_CUTOFF_MAX_HPA
                                              : threshold;
}

// Samples the bag pressure every 100 ms and counts consecutive samples below the threshold.
// Arguments: current time in ms. Returns: true when the number of consecutive below-threshold
// samples has reached the cutoff confirmation count; a failed reading clears the counter.
bool pollPressure(uint32_t now) {
  if (now - lastPressureSampleAt >= kPressureSampleIntervalMs) {
    lastPressureSampleAt = now;

    float hpa;
    lastPressureOk = lps28dfw_read_pressure_hpa(&hpa);
    hasPressureSample = true;
    if (!lastPressureOk) {
      pressureLowCount = 0;
    } else {
      lastPressureHpa = hpa;
      if (hpa < static_cast<float>(pressureThresholdHpa())) {
        if (pressureLowCount < confirmSamples()) {
          ++pressureLowCount;
        }
      } else {
        pressureLowCount = 0;
      }
    }
  }

  return pressureLowCount >= confirmSamples();
}

// Ends emptying when a cutoff is detected (bag pressure below the threshold)
// or when empty_time expires, which is only the maximum empty time.
// Arguments: current time in ms. Returns: nothing; switches the stage when the condition is met.
void updatePumpMonitor(uint32_t now) {
  const bool cutoffDetected = pollPressure(now);
  const uint32_t limitMs = config.empty_time * 1000UL;
  const uint32_t usedMs = now - emptyStartedAt;
  if (!cutoffDetected && usedMs < limitMs) {
    return;
  }

  // An early cutoff shortens the estimated total time by the unused limit.
  if (usedMs < limitMs) {
    const uint32_t savedMs = limitMs - usedMs;
    maximumProcedureMs =
        maximumProcedureMs > savedMs ? maximumProcedureMs - savedMs : 0;
  }

  completeCycleEmpty(now);
}

// Performs the procedure state machine transition for the current stage and deadlines.
// Arguments: current time in ms. Returns: nothing; updates the stage, outputs, cycles and deadlines.
void updateProcedureStage(uint32_t now) {
  switch (stage) {
    case STAGE_START_PENDING:
      // The procedure starts with emptying (cycle 0), then n x (fill + empty).
      setMachineState(MACHINE_EMPTY, now);
      stage = STAGE_CYCLE_EMPTY_ACTUATION;
      stageDeadline = now + config.actuation_time;
      break;

    case STAGE_FILL_ACTUATION:
      if (deadlineReached(now, stageDeadline)) {
        stage = STAGE_FILL_DURATION;
        stageDeadline = now + static_cast<uint32_t>(config.fill_time) * 100;
      }
      break;

    case STAGE_FILL_DURATION:
      if (deadlineReached(now, stageDeadline)) {
        setMachineState(MACHINE_IDLE, now);
        stage = STAGE_FILL_IDLE_SETTLE;
        stageDeadline = now + config.actuation_time;
      }
      break;

    case STAGE_FILL_IDLE_SETTLE:
      if (deadlineReached(now, stageDeadline)) {
        setMachineState(MACHINE_EMPTY, now);
        stage = STAGE_CYCLE_EMPTY_ACTUATION;
        stageDeadline = now + config.actuation_time;
      }
      break;

    case STAGE_CYCLE_EMPTY_ACTUATION:
      if (deadlineReached(now, stageDeadline)) {
        beginPumpMonitor(now);
      }
      break;

    case STAGE_CYCLE_EMPTY_MONITOR:
      updatePumpMonitor(now);
      break;

    case STAGE_CYCLE_IDLE_SETTLE:
      if (deadlineReached(now, stageDeadline)) {
        if (currentCycle >= config.cycles) {
          finishProcedure(now);
        } else {
          beginCycleFill(now);
        }
      }
      break;

    case STAGE_IDLE:
    default:
      break;
  }
}

}  // namespace

// Sets the Z1, Z2 and pump pins as outputs driven low (transistors off).
// Called first in setup(), including after a watchdog reset.
// Arguments: none. Returns: nothing; changes only the output GPIOs.
void procedureOutputsOff() {
  pinMode(kZ1Pin, OUTPUT);
  pinMode(kZ2Pin, OUTPUT);
  pinMode(kPumpPin, OUTPUT);
  digitalWrite(kPumpPin, LOW);
  digitalWrite(kZ1Pin, LOW);
  digitalWrite(kZ2Pin, LOW);
}

// Initialises the procedure GPIOs and starts the startup melody.
// Arguments: none. Returns: nothing; configures the hardware and the internal procedure state.
void procedureBegin() {
  pinMode(kBuzzerPin, OUTPUT);
  procedureOutputsOff();
  setMachineState(MACHINE_IDLE, millis());

  const uint32_t now = millis();
  const BeepStep bootMelody[] = {
      {800, 80, 80},
      {800, 80, 80},
      {1400, 100, 80},
      {1000, 450, 0},
  };
  startBeepSequence(bootMelody, 4, now);
}

// Starts the procedure unless it is already running and switches the UI to the status screen.
// Arguments: none. Returns: nothing; sets the time, limit, cycle counter and start stage.
void procedureStart() {
  if (procedureActive) {
    return;
  }

  const uint32_t now = millis();
  procedureActive = true;
  procedureStartedAt = now;
  maximumProcedureMs = calculateMaximumProcedureMs();
  currentCycle = 0;
  lastDisplayedSecond = UINT32_MAX;
  startBeepPattern(1, kShortBeepMs, 0, now);
  setLastRunResult(LAST_RUN_PENDING);

  displayedPhase = STATUS_EMPTY;
  showStatusScreen(STATUS_EMPTY, 1, config.cycles, 0,
                   statusMaximumSeconds());
  stage = STAGE_START_PENDING;
}

// Plays a short beep confirming an encoder turn.
// Arguments: none. Returns: nothing; replaces the current buzzer sequence.
void procedureEncoderTurnBeep() {
  const BeepStep steps[] = {{2000, 30, 0}};
  startBeepSequence(steps, 1, millis());
}

// Plays a beep confirming an encoder press.
// Arguments: none. Returns: nothing; replaces the current buzzer sequence.
void procedureEncoderPressBeep() {
  const BeepStep steps[] = {{2000, 80, 0}};
  startBeepSequence(steps, 1, millis());
}

// Plays a two-tone error beep, e.g. for an attempt to go outside the allowed range.
// Arguments: none. Returns: nothing; replaces the current buzzer sequence.
void procedureEncoderErrorBeep() {
  const BeepStep steps[] = {
      {800, 35, 45},
      {800, 35, 0},
  };
  startBeepSequence(steps, 2, millis());
}

// Requests a software reset after a short confirmation beep has finished.
// Arguments: none. Returns: nothing; sets the reset flag in the buzzer sequence.
void procedureRequestSoftwareReset() {
  procedureEncoderPressBeep();
  beeper.rebootAfterSequence = true;
}

// Stops the procedure, switches the outputs off and returns to the home screen.
// Arguments: none. Returns: nothing; deactivates the state machine and signals STOP if a cycle was running.
void procedureStop() {
  const uint32_t now = millis();
  if (!procedureActive) {
    setMachineState(MACHINE_IDLE, now);
    returnHomeScreen();
    return;
  }

  procedureActive = false;
  stage = STAGE_IDLE;
  setMachineState(MACHINE_IDLE, now);
  startBeepPattern(5, kFastBeepMs, kFastBeepGapMs, now);
  setLastRunResult(LAST_RUN_STOPPED);
  returnHomeScreen();
}

// Sets the manual output state: fill, empty or idle.
// Arguments: fill and empty requests. Returns: nothing; changes the GPIOs and machine state.
void procedureSetManualOutputs(bool filling, bool emptying) {
  if (filling && !emptying) {
    setMachineState(MACHINE_FILL, millis());
  } else if (emptying && !filling) {
    setMachineState(MACHINE_EMPTY, millis());
  } else {
    setMachineState(MACHINE_IDLE, millis());
  }
}

// Updates the buzzer and runs the current state machine stage if the procedure is active.
// Arguments: none. Returns: nothing; may change the outputs, stages and status screen data.
void procedureUpdate() {
  const uint32_t now = millis();
  updateBeeper(now);

  if (!procedureActive) {
    return;
  }

  updateProcedureStage(now);
  publishStatus(now);
}

// Runs one test mode step: pressure sampling with cutoff detection.
// Arguments: none. Returns: nothing.
void procedureTestUpdate() {
  if (procedureActive) {
    return;
  }
  pollPressure(millis());
}

// Resets the cutoff detection state after the parameters were changed manually.
// Arguments: none. Returns: nothing.
void procedureApplyCutoffConfig() {
  pressureLowCount = 0;
}

// Fills the test mode diagnostic snapshot (pressure in tenths of hPa).
// Arguments: pointer to the result. Returns: nothing; clears the structure before filling it.
void procedureTestGetInfo(PressureTestInfo *info) {
  memset(info, 0, sizeof(*info));
  info->hasPressure = hasPressureSample;
  info->pressureOk = lastPressureOk;
  info->pressureTenthsHpa =
      static_cast<int32_t>(lroundf(lastPressureHpa * 10.0f));
  info->pressureThresholdHpa = pressureThresholdHpa();
  info->confirmSamples = confirmSamples();
  info->pressureLowSamples = pressureLowCount;
  info->pressureCutoffDetected = pressureLowCount >= confirmSamples();
  info->pumpOn = machineState == MACHINE_EMPTY;
}
