# GANDALF – Firmware

Firmware of GANDALF (Gas-bag Automated Nitrogen Displacement And Lab Flushing).

Original author: **Kamil Widzyk**, student at AGH University of Krakow.

Firmware for the Raspberry Pi Pico (RP2040). It drives the solenoid valves and
the pump, runs fill/empty cycles, detects the end of emptying from the bag
pressure (LPS28DFW), handles the OLED display, rotary encoder and buttons, and
stores its settings in emulated EEPROM (flash).

## Architecture

| File | Responsibility |
| --- | --- |
| [flush-automation.ino](./flush-automation.ino) | Arduino entry point. `setup()` initialises the config store, the UI, the pressure sensor and the procedure controller. `loop()` handles inputs, redraws the screen, advances the procedure state machine and services deferred saves. It also provides the I2C register access used by the LPS28DFW driver. |
| [Ui.cpp](./Ui.cpp), [Ui.h](./Ui.h) | OLED, inputs, screens, user-editable configuration and test mode. Defines the `Config` structure shared by all modules and its defaults. |
| [Procedure.cpp](./Procedure.cpp), [Procedure.h](./Procedure.h) | Non-blocking procedure state machine, output control, pressure-based cutoff detection, buzzer sequences and software reset. |
| [ConfigStore.cpp](./ConfigStore.cpp), [ConfigStore.h](./ConfigStore.h) | Reading, validating, migrating and writing the configuration in EEPROM. |
| [Lps28dfw.c](./Lps28dfw.c), [Lps28dfw.h](./Lps28dfw.h) | LPS28DFW pressure sensor driver: address detection, configuration and pressure readout in hPa. |
| `*Bitmaps.h` | Screen and digit bitmaps used by the UI. |
| `UI/` | PNG previews of the screen bitmaps (128×64) and the digit font. |

See also the diagrams in [`docs/`](../../docs): system overview, pneumatics and
output timing.

Modules communicate through the interfaces in their header files. The UI
modifies the shared configuration and forwards events to the procedure; the
procedure in turn updates the status screen. The main Arduino loop calls the
modules continuously, so stage timings and buzzer sequences run without
blocking delays.

## Startup and main loop

1. `procedureOutputsOff()` drives Z1, Z2 and the pump outputs low before
   anything else, so the transistors stay off after any reset, including a
   watchdog reset.
2. `configStoreBegin()` initialises EEPROM emulation and loads the
   configuration (migrating an older record, or falling back to defaults).
3. `uiBegin()` starts I2C, configures the inputs with pull-ups and initialises
   the SSD1306 128×64 display at address `0x3C`. STOP held at this point
   enables test mode.
4. `lps28dfw_begin()` detects and configures the pressure sensor.
5. `procedureBegin()` configures the outputs and plays the startup melody.
6. The hardware watchdog is started with a 2 s timeout
   (`kWatchdogTimeoutMs` in `flush-automation.ino`).
7. On every `loop()` iteration:
   - `rp2040.wdt_reset()` feeds the watchdog; if the loop stalls for 2 s,
     the RP2040 resets and step 1 switches all outputs off again;
   - `processInput()` reads the buttons and the encoder and updates manual
     control;
   - `updateScreen()` redraws the screen if needed (at least 16 ms between
     redraws);
   - `procedureUpdate()` advances the state machine and drives the buzzer;
   - the screen is redrawn again after changes made by the procedure;
   - `configStoreService()` performs a pending save once 750 ms have passed
     since the last save request.

Buttons are debounced (25 ms). The procedure and the buzzer use
`millis()`-based deadlines, so the loop keeps servicing the UI and display.

## User interface

### Home screen

The encoder selects the number of cycles, the fill time, the empty time or
the settings entry. Pressing the encoder starts editing the selected value;
pressing again confirms it and requests a save. Turning past the allowed range
plays an error beep. The empty time is edited in two steps: minutes (±60 s),
then seconds (±1 s).

START begins the procedure. STOP aborts it, switches all outputs off and
returns to the home screen. The status screen shows the current phase, cycle
number, elapsed time and a progress bar relative to the computed maximum
duration.

The manual fill and empty buttons work on the home screen and in test mode.
Pressing both at once activates no output. After switching to another screen,
manual control is locked until the buttons are released, so a button left
pressed cannot activate an output on return.

The bottom-right corner shows the bag status icon (`LastRunResult`):
a warning triangle (`LAST_RUN_PENDING`: the initial state after power-up, set
again by any manual fill/empty and by `procedureStart()`), a check mark
(`LAST_RUN_DONE`, set by `finishProcedure()`) or a cross (`LAST_RUN_STOPPED`,
set by `procedureStop()` while a procedure is running). The icons are drawn
over the home bitmap by `drawLastRunIcon()` in `Ui.cpp`; the state lives in
RAM only.

### Settings

The settings menu offers: back to home, maximum fill time, maximum empty
time, and software reset (with confirmation).

## Procedure state machine

The procedure has two parts:

1. **Initial empty** (cycle 0): Z2 opens and the pump starts; after
   `actuation_time` cutoff monitoring begins and runs until a cutoff is
   detected or `empty_time` expires. Outputs are switched off and
   `actuation_time` elapses.
2. **Cycles** (`cycles` times), each with two phases:
   - fill: Z1 opens, `actuation_time` elapses, then filling lasts
     `fill_time × 100` ms. Outputs switch off and `actuation_time` elapses;
   - empty: Z2 opens and the pump starts; after `actuation_time` cutoff
     monitoring runs until a cutoff is detected or `empty_time` expires.
     Outputs switch off and `actuation_time` elapses.

After the last cycle the procedure ends and plays the end sequence: three
2 kHz beeps (333 ms, 333 ms and 500 ms, 100 ms gaps). STOP immediately drives
all outputs low, aborts the state machine and plays a series of short beeps.

Outputs are set centrally by the machine state:

| State | Active outputs |
| --- | --- |
| Idle | none |
| Fill | Z1 |
| Empty | Z2 and pump |

### Cutoff detection (pressure sensor)

A cutoff ends the empty step early; `empty_time` is only the maximum empty
time. During emptying the bag pressure is sampled every 100 ms. A cutoff is
reported when, for `cutoff_confirm_samples` consecutive samples (default
10 = 1 s), the pressure is below `pressure_cutoff_hpa` (default 850 hPa, range
260–1260 hPa). A failed reading resets the counter. If the sensor does not
respond, no cutoff occurs and the step ends at the `empty_time` limit.

`Lps28dfw.c` drives the LPS28DFW on the shared I2C bus (GP4/GP5) at address
`0x5C` (SA0 = GND) or `0x5D` (SA0 = VDD). The address is detected
automatically and `WHO_AM_I` must read `0xB4`. The sensor runs in continuous
mode (25 Hz, 16-sample averaging, 1260 hPa full scale, 4096 LSB/hPa) and the
24-bit result is converted to hPa. Out-of-range readings (e.g. zero right after
power-up) are discarded. After a communication error the sensor is searched
for and configured again.

### Test mode

Power up with STOP held. The encoder switches pages; the manual fill/empty
buttons work on all pages.

1. `TEST MODE`: sensor status, large pressure readout and manual output
   state.
2. `PRESSURE`: pressure, threshold, below-threshold sample counter and cutoff
   state.
3. `PARAMETERS`: editing of `THRESHOLD[hPa]` and `CUTOFF SAMPLES`. A press
   enters field selection, turning moves the cursor (the last entry is
   `BACK`),
   a press selects a field. While editing, turning changes the value
   (pressure: 1 hPa slow / 10 hPa fast; samples: 1), a press saves it to
   EEPROM and applies it immediately, and STOP cancels the edit.

## Configuration

Defaults (defined in `Ui.cpp`, `defaultConfig`):

| Parameter | Default | Unit / meaning |
| --- | ---: | --- |
| Number of cycles | 1 | cycles |
| Fill time | 50 | 0.1 s (5 s) |
| Maximum fill time | 100 | s |
| Empty time | 30 | s |
| Maximum empty time | 30 | s |
| Actuation time | 500 | ms |
| Cutoff confirm samples | 10 | samples (100 ms each) |
| Pressure threshold | 850 | hPa |

The fill time (`fill_time`) is stored in tenths of a second; the empty time
(`empty_time`) in whole seconds (edited as mm:ss). `max_fill_time` and
`max_empty_time` are limits in seconds; the settings editor allows 1–100 s and
1–5999 s (99 min 59 s) respectively. The number of cycles is limited to 1–99.

A stored record is rejected at startup unless `empty_time ≤ max_empty_time`
and `fill_time ≤ max_fill_time × 10`, so the defaults must satisfy these
conditions as well. After loading defaults or migrating an older record, both
times are clamped to their maximums before saving.

EEPROM emulation uses a 256-byte buffer. The record contains a magic value,
version, size, the settings and a CRC-32. The current record version is **9**.
Version 8 and 7 records (from firmware that also used the pump-current
sensor) are migrated: cycles, times, actuation time, pressure threshold and
cutoff samples are kept; the current-sensor fields are dropped. Version 7
stored the empty time in tenths of a second, so it is reset to the default.
Older records are replaced with the defaults. Changes are saved with a delay
to limit flash writes.

## Hardware connections

The pin mapping below is taken from the firmware and matches the physical
device.

| Pico GPIO | Function |
| --- | --- |
| GP0 | Buzzer |
| GP1 | START |
| GP2 | STOP |
| GP3 | Encoder – CLK |
| GP4 | I2C SDA: OLED and LPS28DFW |
| GP5 | I2C SCL: OLED and LPS28DFW |
| GP6 | Encoder – DT |
| GP7 | Encoder push button |
| GP8 | Output Z1 (fill) |
| GP9 | Output Z2 (empty) |
| GP10 | Pump output |
| GP16 | Manual empty button |
| GP17 | Manual fill button |

All buttons use the internal pull-ups and are active low.

## Building

`libraries.txt` lists the Arduino libraries: Adafruit SSD1306 and Adafruit
GFX (plus Adafruit BusIO as a dependency). The sketch also uses the Arduino
RP2040 core API, `Wire`, `EEPROM` and the Pico hardware watchdog. Select the
Raspberry Pi Pico (RP2040) board, or use `install.bat` and `compile.bat`
(arduino-cli) on Windows.

`diagram.json` and `wokwi.toml` date from an early stage of the project, when
the device was tested in the Wokwi simulator. They do not fully describe the
physical device and are not needed to build the firmware.
