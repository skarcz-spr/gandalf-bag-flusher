#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>

#include "UIBitmaps.h"
#include "HomeV2MinutesBitmaps.h"
#include "StatusBitmaps.h"
#include "SettingsBitmaps.h"
#include "SettingsV2Bitmaps.h"
#include "ConfigStore.h"
#include "Lps28dfw.h"
#include "Ui.h"
#include "Procedure.h"

namespace {

constexpr uint8_t kOledAddress = 0x3C;
constexpr uint8_t kSdaPin = 4;
constexpr uint8_t kSclPin = 5;
constexpr uint8_t kEncoderClockPin = 3;
constexpr uint8_t kEncoderDataPin = 6;
constexpr uint8_t kEncoderButtonPin = 7;
constexpr uint8_t kStartButtonPin = 1;
constexpr uint8_t kStopButtonPin = 2;
constexpr uint8_t kFillButtonPin = 17;
constexpr uint8_t kEmptyButtonPin = 16;
constexpr int16_t kScreenWidth = 128;
constexpr int16_t kScreenHeight = 64;
constexpr uint32_t kButtonDebounceMs = 25;
constexpr uint32_t kMinimumRedrawIntervalMs = 16;
constexpr uint8_t kTestPageManual = 0;
constexpr uint8_t kTestPagePressure = 1;
constexpr uint8_t kTestPageParams = 2;
constexpr uint8_t kTestPageCount = 3;
// Cutoff parameter editing: page browsing, field selection, value change.
constexpr uint8_t kParamModeBrowse = 0;
constexpr uint8_t kParamModeSelect = 1;
constexpr uint8_t kParamModeEdit = 2;
constexpr uint8_t kParamRowPressure = 0;
constexpr uint8_t kParamRowSamples = 1;
constexpr uint8_t kParamRowCount = 2;
constexpr uint32_t kParamFastTurnMs = 80;

// Settings menu entries (settingsPosition).
constexpr uint8_t kMenuBack = 0;
constexpr uint8_t kMenuMaxFillTime = 1;
constexpr uint8_t kMenuMaxEmptyTime = 2;
constexpr uint8_t kMenuRestart = 3;
constexpr uint8_t kMenuCount = 4;

Adafruit_SSD1306 display(kScreenWidth, kScreenHeight, &Wire, -1);

int cursorPosition = 0;
int selectedOption = -1;
// Empty time edit field: 0 = minutes, 1 = seconds. Used only when selectedOption == 2.
uint8_t emptyTimeField = 0;
int8_t encoderAccumulator = 0;
uint8_t previousEncoderState = 0;
bool lastButtonReading = HIGH;
bool stableButtonState = HIGH;
uint32_t buttonChangedAt = 0;
uint32_t lastScreenUpdate = 0;
bool screenDirty = true;
bool displayReady = false;
bool hasDrawnScreen = false;
StatusPhase statusPhase = STATUS_EMPTY;
uint8_t statusCurrentCycle = 1;
uint8_t statusAllCycles = 0;
uint32_t statusElapsedSeconds = 0;
uint32_t statusMaximumSeconds = 0;
uint8_t settingsPosition = 0;
uint8_t confirmSelection = 0;
uint32_t numberEditValue = 1;
// Maximum empty time edit field: 0 = minutes, 1 = seconds.
uint8_t numberEditField = 0;
LastRunResult lastRunResult = LAST_RUN_PENDING;
uint8_t homeManualOutputMode = 0;
bool homeManualLockout = false;
bool testModeActive = false;
uint8_t testPage = kTestPageManual;
uint8_t paramMode = kParamModeBrowse;
uint8_t paramCursor = 0;
int32_t paramEditValue = 0;
uint32_t lastParamTurnAt = 0;
PressureTestInfo testInfo = {};
uint8_t testInfoAddress = 0;

struct DebouncedButton {
  uint8_t pin;
  bool lastReading;
  bool stableState;
  uint32_t changedAt;
};

DebouncedButton startButton = {kStartButtonPin, HIGH, HIGH, 0};
DebouncedButton stopButton = {kStopButtonPin, HIGH, HIGH, 0};

const int8_t kEncoderTransitions[16] = {
  0, -1, 1, 0,
  1, 0, 0, -1,
  -1, 0, 0, 1,
  0, 1, -1, 0
};

// Reads the state of the two encoder channels as a two-bit value.
// Arguments: none. Returns: the current CLK and DT state; does not change the UI state.
uint8_t readEncoderState() {
  return (static_cast<uint8_t>(digitalRead(kEncoderClockPin)) << 1) |
         static_cast<uint8_t>(digitalRead(kEncoderDataPin));
}

// Marks the screen as needing a redraw.
// Arguments: none. Returns: nothing; sets the screenDirty flag.
void markScreenDirty() {
  screenDirty = true;
}

// Picks the home screen bitmap for the selected field or cursor position.
// Arguments: none. Returns: pointer to the bitmap; does not change the UI state.
const uint8_t *homeBackground() {
  if (selectedOption == 0) {
    return HOME_V2_SELECTED_0;
  }
  if (selectedOption == 1) {
    return HOME_V2_SELECTED_1;
  }
  if (selectedOption == 2) {
    return emptyTimeField == 0 ? HOME_V2_SELECTED_2 : HOME_V2_SELECTED_3;
  }

  switch (cursorPosition) {
    case 1:
      return HOME_V2_CURSOR_1;
    case 2:
      return HOME_V2_CURSOR_2;
    case 3:
      return HOME_V2_CURSOR_3;
    default:
      return HOME_V2_CURSOR_0;
  }
}

// Draws one digit on the OLED at the given position, optionally with inverted colours.
// Arguments: digit (0-9), x/y position and invert flag. Returns: nothing;
// modifies the display buffer.
void drawDigit(uint8_t digit, int16_t x, int16_t y, bool inverted) {
  const uint16_t foreground = inverted ? SSD1306_BLACK : SSD1306_WHITE;
  const uint16_t background = inverted ? SSD1306_WHITE : SSD1306_BLACK;
  display.drawBitmap(x, y, FONT_DIGITS[digit], 8, 12, foreground, background);
}

// Letters "m" (5x4) and "s" (4x7) as on the home screen; the MSB is the left column.
constexpr uint8_t kMinuteGlyph[4] = {0x50, 0xA8, 0xA8, 0xA8};
constexpr uint8_t kSecondGlyph[7] = {0x60, 0x90, 0x80, 0x60, 0x10, 0x90, 0x60};

// Last-run result icons (bottom-right corner of the home screen), 16 bits per
// row, the MSB is the left column.
constexpr uint16_t kCheckIcon[12] = {0x0003, 0x0007, 0x000E, 0x001C,
                                     0x0038, 0x0070, 0xC0E0, 0xE1C0,
                                     0x7380, 0x3F00, 0x1E00, 0x0C00};
constexpr uint16_t kCrossIcon[12] = {0xC030, 0xE070, 0x70E0, 0x39C0,
                                     0x1F80, 0x0F00, 0x0F00, 0x1F80,
                                     0x39C0, 0x70E0, 0xE070, 0xC030};
constexpr uint16_t kWarningIcon[12] = {0x00C0, 0x01E0, 0x0120, 0x02D0,
                                       0x02D0, 0x04C8, 0x04C8, 0x08C4,
                                       0x0804, 0x10C2, 0x10C2, 0x3FFC};
constexpr uint8_t kResultIconHeight = 12;
constexpr int16_t kResultIconRight = 125;   // x just past the icon
constexpr int16_t kResultIconBottom = 62;   // y just past the icon

// Draws a letter from a bit pattern in the given colour; cleared bits are left unchanged.
// Arguments: rows, width, height, x/y position and colour. Returns: nothing.
void drawGlyph(const uint8_t *rows, uint8_t width, uint8_t height, int16_t x,
               int16_t y, uint16_t color) {
  for (uint8_t row = 0; row < height; ++row) {
    for (uint8_t column = 0; column < width; ++column) {
      if (rows[row] & (0x80 >> column)) {
        display.drawPixel(x + column, y + row, color);
      }
    }
  }
}

// Draws the last-run result icon (check mark, cross or warning triangle) in
// the bottom-right corner of the home screen.
// Arguments: none. Returns: nothing; modifies the display buffer.
void drawLastRunIcon() {
  const uint16_t *rows = nullptr;
  uint8_t width = 0;
  if (lastRunResult == LAST_RUN_DONE) {
    rows = kCheckIcon;
    width = 16;
  } else if (lastRunResult == LAST_RUN_STOPPED) {
    rows = kCrossIcon;
    width = 12;
  } else {
    rows = kWarningIcon;
    width = 16;
  }

  const int16_t x = kResultIconRight - width;
  const int16_t y = kResultIconBottom - kResultIconHeight;
  for (uint8_t row = 0; row < kResultIconHeight; ++row) {
    for (uint8_t column = 0; column < width; ++column) {
      if (rows[row] & (0x8000 >> column)) {
        display.drawPixel(x + column, y + row, SSD1306_WHITE);
      }
    }
  }
}

// Draws the home screen with the configuration and the active field highlighted.
// Arguments: none. Returns: nothing; reads config and modifies the OLED buffer.
void drawHomeScreen() {
  display.drawBitmap(0, 0, homeBackground(), kScreenWidth, kScreenHeight,
                     SSD1306_WHITE);
  drawLastRunIcon();

  const bool cyclesInverted = selectedOption == 0;
  drawDigit(config.cycles / 10, 13, 2, cyclesInverted);
  drawDigit(config.cycles % 10, 23, 2, cyclesInverted);

  const bool fillTimeInverted = selectedOption == 1;
  drawDigit(config.fill_time / 100, 13, 17, fillTimeInverted);
  drawDigit((config.fill_time / 10) % 10, 23, 17, fillTimeInverted);
  drawDigit(config.fill_time % 10, 37, 17, fillTimeInverted);

  const uint8_t emptyMinutes = (config.empty_time / 60) % 100;
  const uint8_t emptySeconds = config.empty_time % 60;
  const bool emptyMinutesInverted = selectedOption == 2 && emptyTimeField == 0;
  const bool emptySecondsInverted = selectedOption == 2 && emptyTimeField == 1;
  drawDigit(emptyMinutes / 10, 13, 34, emptyMinutesInverted);
  drawDigit(emptyMinutes % 10, 23, 34, emptyMinutesInverted);
  drawDigit(emptySeconds / 10, 39, 34, emptySecondsInverted);
  drawDigit(emptySeconds % 10, 49, 34, emptySecondsInverted);
}

// Picks the status screen background for the current procedure phase.
// Arguments: none. Returns: pointer to the bitmap; does not change the UI state.
const uint8_t *statusBackground() {
  switch (statusPhase) {
    case STATUS_FILL:
      return STATUS_FILL_BITMAP;
    default:
      return STATUS_EMPTY_BITMAP;
  }
}

// Draws a colon at the given position on the status screen.
// Arguments: x/y coordinates. Returns: nothing; modifies the display buffer.
void drawTimeSeparator(int16_t x, int16_t y) {
  display.drawPixel(x, y + 3, SSD1306_WHITE);
  display.drawPixel(x, y + 8, SSD1306_WHITE);
}

// Draws the slash separating the times on the status screen.
// Arguments: x/y coordinates. Returns: nothing; modifies the display buffer.
void drawTimeSlash(int16_t x, int16_t y) {
  display.drawLine(x, y + 10, x + 5, y + 2, SSD1306_WHITE);
}

// Draws a time as MM:SS or H:MM:SS.
// Arguments: time in seconds, start x, y and whether to use the hour format.
// Returns: nothing; modifies the display buffer.
void drawTimeDigits(uint32_t seconds, int16_t firstX, int16_t y,
                    bool includeHours) {
  if (includeHours) {
    const uint32_t hours = seconds / 3600;
    const uint32_t minutes = (seconds / 60) % 60;
    const uint32_t remainingSeconds = seconds % 60;
    drawDigit(hours % 10, firstX, y, false);
    drawTimeSeparator(firstX + 9, y);
    drawDigit(minutes / 10, firstX + 13, y, false);
    drawDigit(minutes % 10, firstX + 23, y, false);
    drawTimeSeparator(firstX + 32, y);
    drawDigit(remainingSeconds / 10, firstX + 36, y, false);
    drawDigit(remainingSeconds % 10, firstX + 46, y, false);
    return;
  }

  const uint32_t minutes = seconds / 60;
  const uint32_t remainingSeconds = seconds % 60;
  drawDigit((minutes / 10) % 10, firstX, y, false);
  drawDigit(minutes % 10, firstX + 10, y, false);
  drawDigit(remainingSeconds / 10, firstX + 23, y, false);
  drawDigit(remainingSeconds % 10, firstX + 33, y, false);
}

// Draws the status screen: phase, cycle number, times and progress bar.
// Arguments: none. Returns: nothing; reads the status state and modifies the OLED buffer.
void drawStatusScreen() {
  display.drawBitmap(0, 0, statusBackground(), kScreenWidth, kScreenHeight,
                     SSD1306_WHITE);

  drawDigit(statusCurrentCycle / 10, 41, 9, false);
  drawDigit(statusCurrentCycle % 10, 51, 9, false);
  drawDigit(statusAllCycles / 10, 69, 9, false);
  drawDigit(statusAllCycles % 10, 79, 9, false);

  const bool includeHours = statusMaximumSeconds >= 3600;
  if (includeHours) {
    display.fillRect(16, 35, 112, 14, SSD1306_BLACK);
    drawTimeDigits(statusElapsedSeconds, 10, 36, true);
    drawTimeSlash(66, 36);
    drawTimeDigits(statusMaximumSeconds, 73, 36, true);
  } else {
    drawTimeDigits(statusElapsedSeconds, 18, 36, false);
    drawTimeDigits(statusMaximumSeconds, 69, 36, false);
  }

  constexpr int16_t kProgressX = 3;
  constexpr int16_t kProgressY = 54;
  constexpr int16_t kProgressWidth = 122;
  constexpr int16_t kProgressHeight = 7;
  display.fillRect(kProgressX, kProgressY, kProgressWidth, kProgressHeight,
                   SSD1306_BLACK);

  if (statusMaximumSeconds > 0) {
    const uint32_t boundedElapsed =
        statusElapsedSeconds < statusMaximumSeconds
            ? statusElapsedSeconds
            : statusMaximumSeconds;
    const int16_t filledWidth = static_cast<int16_t>(
        (static_cast<uint64_t>(boundedElapsed) * kProgressWidth) /
        statusMaximumSeconds);
    display.fillRect(kProgressX, kProgressY, filledWidth, kProgressHeight,
                     SSD1306_WHITE);
  }
}

// Draws a line of text in the default 6x8 font.
// Arguments: text, position and colours. Returns: nothing; modifies the display buffer.
void drawSettingsLine(const char *text, int16_t x, int16_t y,
                      uint16_t foreground, uint16_t background) {
  display.setTextSize(1);
  display.setTextColor(foreground, background);
  display.setCursor(x, y);
  display.print(text);
}

// Draws the settings menu page selected by settingsPosition.
// Arguments: none. Returns: nothing; modifies the display buffer.
void drawSettingsMenu() {
  static const uint8_t *const pages[kMenuCount] = {
      SETTINGS_V2_PAGE_0, SETTINGS_V2_PAGE_1, SETTINGS_V2_PAGE_2,
      SETTINGS_V2_PAGE_3,
  };
  display.drawBitmap(0, 0, pages[settingsPosition], kScreenWidth,
                     kScreenHeight, SSD1306_WHITE);
}

// Returns the bitmap for the currently selected answer in the restart dialog.
// Arguments: none. Returns: pointer to the "no" or "yes" bitmap.
const uint8_t *confirmBackground() {
  return confirmSelection == 0 ? CONFIRM_REBOOT_NO : CONFIRM_REBOOT_YES;
}

// Draws the maximum empty time editor in the 00m 00s format used on the home screen.
// Arguments: none. Returns: nothing; reads numberEditValue and numberEditField, modifies the OLED buffer.
void drawMaxEmptyTimeEditor() {
  display.fillRect(36, 33, 56, 18, SSD1306_BLACK);
  const uint32_t minutes = (numberEditValue / 60) % 100;
  const uint32_t seconds = numberEditValue % 60;
  const bool minutesSelected = numberEditField == 0;
  const bool secondsSelected = numberEditField == 1;
  if (minutesSelected) {
    display.fillRoundRect(36, 35, 27, 17, 3, SSD1306_WHITE);
  }
  if (secondsSelected) {
    display.fillRoundRect(65, 35, 26, 17, 3, SSD1306_WHITE);
  }

  drawDigit(minutes / 10, 38, 37, minutesSelected);
  drawDigit(minutes % 10, 48, 37, minutesSelected);
  drawGlyph(kMinuteGlyph, 5, 4, 57, 45,
            minutesSelected ? SSD1306_BLACK : SSD1306_WHITE);
  drawDigit(seconds / 10, 67, 37, secondsSelected);
  drawDigit(seconds % 10, 77, 37, secondsSelected);
  drawGlyph(kSecondGlyph, 4, 7, 86, 42,
            secondsSelected ? SSD1306_BLACK : SSD1306_WHITE);
}

// Draws the editor screen for the maximum fill time or the maximum empty time.
// Arguments: none. Returns: nothing; reads the edit state and modifies the OLED buffer.
void drawNumberEditScreen() {
  display.drawBitmap(0, 0,
                     settingsPosition == kMenuMaxFillTime ? NUMBER_MAX_FILL : NUMBER_MAX_EMPTY,
                     kScreenWidth, kScreenHeight, SSD1306_WHITE);

  if (settingsPosition == kMenuMaxFillTime) {
    drawDigit((numberEditValue / 100) % 10, 50, 37, false);
    drawDigit((numberEditValue / 10) % 10, 60, 37, false);
    drawDigit(numberEditValue % 10, 70, 37, false);
  } else {
    drawMaxEmptyTimeEditor();
  }
}

// Formats a value in tenths as the text "123.4" with an optional plus sign.
// Arguments: buffer, size, value and force-sign flag. Returns: nothing.
void formatTenths(char *out, size_t size, int32_t tenths, bool forceSign) {
  const int32_t magnitude = tenths < 0 ? -tenths : tenths;
  snprintf(out, size, "%s%ld.%ld",
           tenths < 0 ? "-" : (forceSign ? "+" : ""),
           static_cast<long>(magnitude / 10),
           static_cast<long>(magnitude % 10));
}

// Draws the test page header with the title and page number.
// Arguments: title. Returns: nothing; modifies the display buffer.
void drawTestHeader(const char *title) {
  display.fillRect(0, 0, kScreenWidth, 11, SSD1306_WHITE);
  drawSettingsLine(title, 2, 2, SSD1306_BLACK, SSD1306_WHITE);
  char page[8];
  snprintf(page, sizeof(page), "%u/%u", static_cast<unsigned>(testPage + 1),
           static_cast<unsigned>(kTestPageCount));
  drawSettingsLine(page, 104, 2, SSD1306_BLACK, SSD1306_WHITE);
}

// Draws one line of text on a test page.
// Arguments: y position and text (max. 21 characters). Returns: nothing.
void drawTestLine(int16_t y, const char *text) {
  drawSettingsLine(text, 1, y, SSD1306_WHITE, SSD1306_BLACK);
}

// Draws the bottom bar of a test page, optionally inverted.
// Arguments: text and invert flag. Returns: nothing; modifies the display buffer.
void drawTestFooter(const char *text, bool inverted) {
  if (inverted) {
    display.fillRect(0, 53, kScreenWidth, 11, SSD1306_WHITE);
    drawSettingsLine(text, 1, 55, SSD1306_BLACK, SSD1306_WHITE);
  } else {
    drawSettingsLine(text, 1, 55, SSD1306_WHITE, SSD1306_BLACK);
  }
}

// Draws test mode page 1: large pressure readout and manual control state.
// Arguments: none. Returns: nothing; modifies the display buffer.
void drawTestManualPage() {
  drawTestHeader("TEST MODE");

  if (lps28dfw_address() != 0) {
    char status[24];
    snprintf(status, sizeof(status), "LPS28DFW 0x%02X OK", lps28dfw_address());
    drawSettingsLine(status, 12, 15, SSD1306_WHITE, SSD1306_BLACK);
  } else {
    drawSettingsLine("LPS28DFW: NO I2C", 16, 15, SSD1306_WHITE,
                     SSD1306_BLACK);
  }

  char text[20];
  if (!testInfo.hasPressure) {
    snprintf(text, sizeof(text), "...");
  } else if (!testInfo.pressureOk) {
    snprintf(text, sizeof(text), "I2C ERROR");
  } else {
    char value[16];
    formatTenths(value, sizeof(value), testInfo.pressureTenthsHpa, false);
    snprintf(text, sizeof(text), "%s", value);
  }
  const int16_t textWidth = static_cast<int16_t>(strlen(text)) * 12;
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
  display.setCursor((kScreenWidth - textWidth) / 2, 27);
  display.print(text);
  if (testInfo.hasPressure && testInfo.pressureOk) {
    drawSettingsLine("hPa", 55, 44, SSD1306_WHITE, SSD1306_BLACK);
  }

  if (homeManualOutputMode == 1) {
    drawSettingsLine("FILLING", 43, 54, SSD1306_WHITE, SSD1306_BLACK);
  } else if (homeManualOutputMode == 2) {
    drawSettingsLine("EMPTYING", 40, 54, SSD1306_WHITE, SSD1306_BLACK);
  } else {
    drawSettingsLine("---", 55, 54, SSD1306_WHITE, SSD1306_BLACK);
  }
}

// Draws test mode page 2: LPS28DFW pressure, threshold and pressure cutoff
// state.
// Arguments: none. Returns: nothing; modifies the display buffer.
void drawTestPressurePage() {
  drawTestHeader("PRESSURE");

  char line[28];
  char value[16];
  if (lps28dfw_address() != 0) {
    snprintf(line, sizeof(line), "LPS28DFW 0x%02X OK", lps28dfw_address());
  } else {
    snprintf(line, sizeof(line), "LPS28DFW: NO I2C");
  }
  drawTestLine(13, line);

  if (testInfo.hasPressure && testInfo.pressureOk) {
    formatTenths(value, sizeof(value), testInfo.pressureTenthsHpa, false);
    const int16_t width = static_cast<int16_t>(strlen(value)) * 12;
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
    display.setCursor(4, 23);
    display.print(value);
    drawSettingsLine("hPa", 4 + width + 4, 30, SSD1306_WHITE, SSD1306_BLACK);
  } else {
    drawSettingsLine(testInfo.hasPressure ? "READ ERROR" : "...", 4, 27,
                     SSD1306_WHITE, SSD1306_BLACK);
  }

  snprintf(line, sizeof(line), "THRESHOLD: %ld hPa",
           static_cast<long>(testInfo.pressureThresholdHpa));
  drawTestLine(39, line);
  snprintf(line, sizeof(line), "BELOW: %u/%u SAMPLES",
           static_cast<unsigned>(testInfo.pressureLowSamples),
           static_cast<unsigned>(testInfo.confirmSamples));
  drawTestLine(46, line);

  if (testInfo.pressureCutoffDetected) {
    drawTestFooter("CUTOFF: DETECTED!", true);
  } else {
    drawTestFooter("CUTOFF: NO", false);
  }
}

// Returns the stored parameter in edit units: field 0 is the pressure threshold (hPa),
// field 1 is the number of cutoff confirmation samples.
// Arguments: field number. Returns: the value from the configuration.
int32_t paramStoredValue(uint8_t row) {
  return row == kParamRowPressure ? config.pressure_cutoff_hpa
                                  : config.cutoff_confirm_samples;
}

// Lower edit limit of a field.
// Arguments: field number. Returns: the smallest allowed value.
int32_t paramMinValue(uint8_t row) {
  return row == kParamRowPressure ? PRESSURE_CUTOFF_MIN_HPA : 1;
}

// Upper edit limit of a field.
// Arguments: field number. Returns: the largest allowed value.
int32_t paramMaxValue(uint8_t row) {
  return row == kParamRowPressure ? PRESSURE_CUTOFF_MAX_HPA
                                  : MAX_CUTOFF_CONFIRM_SAMPLES;
}

// Step by which an encoder turn changes the field value.
// Arguments: field number and fast-turn flag. Returns: the step (pressure: 1 or 10 hPa,
// number of samples: 1).
int32_t paramStep(uint8_t row, bool fast) {
  return row == kParamRowPressure && fast ? 10 : 1;
}

// Stores a parameter in the configuration.
// Arguments: field number and value in edit units. Returns: nothing; requests an EEPROM save
// and resets the cutoff detection state.
void paramCommit(uint8_t row, int32_t value) {
  if (row == kParamRowPressure) {
    config.pressure_cutoff_hpa = static_cast<uint16_t>(value);
  } else {
    config.cutoff_confirm_samples = static_cast<uint8_t>(value);
  }
  configStoreRequestSave();
  procedureApplyCutoffConfig();
}

// Draws test mode page 3: manual editing of the pressure threshold and the number of
// cutoff confirmation samples.
// Arguments: none. Returns: nothing; modifies the display buffer.
void drawTestParamsPage() {
  static const char *const labels[kParamRowCount] = {"THRESHOLD[hPa]:",
                                                     "CUTOFF SAMPLES:"};
  drawTestHeader("PARAMETERS");

  for (uint8_t row = 0; row < kParamRowCount; ++row) {
    const int16_t y = 16 + row * 10;
    const bool selected = paramMode != kParamModeBrowse && paramCursor == row;
    const bool editing = paramMode == kParamModeEdit && selected;
    const uint16_t foreground = editing ? SSD1306_BLACK : SSD1306_WHITE;
    const uint16_t background = editing ? SSD1306_WHITE : SSD1306_BLACK;

    char value[16];
    const int32_t shown = editing ? paramEditValue : paramStoredValue(row);
    snprintf(value, sizeof(value), "%ld", static_cast<long>(shown));
    if (editing) {
      display.fillRect(7, y - 1, 121, 9, SSD1306_WHITE);
    }
    if (selected) {
      drawSettingsLine(">", 1, y, SSD1306_WHITE, SSD1306_BLACK);
    }
    drawSettingsLine(labels[row], 8, y, foreground, background);
    drawSettingsLine(value,
                     126 - static_cast<int16_t>(strlen(value)) * 6, y,
                     foreground, background);
  }

  // Bottom line: a hint while editing and on the BACK entry, otherwise
  // the current pressure.
  if (paramMode == kParamModeEdit) {
    drawTestFooter("TURN / PRESS: SAVE", true);
  } else if (paramMode == kParamModeSelect && paramCursor >= kParamRowCount) {
    drawTestFooter("> BACK", true);
  } else {
    char line[28];
    char value[16];
    if (testInfo.hasPressure && testInfo.pressureOk) {
      formatTenths(value, sizeof(value), testInfo.pressureTenthsHpa, false);
      snprintf(line, sizeof(line), "P:%s hPa", value);
    } else {
      snprintf(line, sizeof(line), "P: NO DATA");
    }
    drawTestLine(54, line);
  }
}

// Draws the current test mode page.
// Arguments: none. Returns: nothing; modifies the display buffer.
void drawTestScreen() {
  if (testPage == kTestPagePressure) {
    drawTestPressurePage();
  } else if (testPage == kTestPageParams) {
    drawTestParamsPage();
  } else {
    drawTestManualPage();
  }
}

// Reads a button with contact debouncing and detects the press edge.
// Arguments: button state to update and current time in ms.
// Returns: true only for a confirmed press; modifies the given state.
bool pollDebouncedButton(DebouncedButton &button, uint32_t now) {
  const bool reading = digitalRead(button.pin);
  if (reading != button.lastReading) {
    button.lastReading = reading;
    button.changedAt = now;
  }

  if (now - button.changedAt < kButtonDebounceMs ||
      reading == button.stableState) {
    return false;
  }

  button.stableState = reading;
  return button.stableState == LOW;
}

// Returns to the settings menu and forces a screen redraw.
// Arguments: none. Returns: nothing; changes currentScreen and screenDirty.
void returnToSettings() {
  currentScreen = SCREEN_SETTINGS;
  screenDirty = true;
}

// Runs the action assigned to the current settings menu entry.
// Arguments: none. Returns: nothing; changes the screen.
void selectSettingsItem() {
  switch (settingsPosition) {
    case kMenuBack:
      returnHomeScreen();
      return;
    case kMenuMaxFillTime:
      numberEditValue = config.max_fill_time;
      currentScreen = SCREEN_SETTINGS_NUMBER_EDIT;
      break;
    case kMenuMaxEmptyTime:
      numberEditValue = config.max_empty_time;
      numberEditField = 0;
      currentScreen = SCREEN_SETTINGS_NUMBER_EDIT;
      break;
    case kMenuRestart:
      confirmSelection = 0;
      currentScreen = SCREEN_SETTINGS_CONFIRM;
      break;
  }
  markScreenDirty();
}

enum EncoderRotationResult {
  ENCODER_ACTION_IGNORED,
  ENCODER_ACTION_CHANGED,
  ENCODER_ACTION_LIMIT
};

// Applies an encoder turn to the active menu, field or configuration value.
// Arguments: turn direction (+1/-1). Returns: ignored, changed or limit
// reached; may modify the cursor, selection, numberEditValue or config.
EncoderRotationResult handleRotation(int8_t direction) {
  if (currentScreen == SCREEN_STATUS) {
    return ENCODER_ACTION_IGNORED;
  }

  if (currentScreen == SCREEN_TEST) {
    if (testPage == kTestPageParams && paramMode == kParamModeSelect) {
      const int nextCursor = static_cast<int>(paramCursor) + direction;
      if (nextCursor < 0 || nextCursor > kParamRowCount) {
        return ENCODER_ACTION_LIMIT;
      }
      paramCursor = static_cast<uint8_t>(nextCursor);
    } else if (testPage == kTestPageParams && paramMode == kParamModeEdit) {
      // Pressure: a fast turn changes the value by 10 hPa, a slow one by 1 hPa.
      const uint32_t now = millis();
      const int32_t step =
          paramStep(paramCursor, now - lastParamTurnAt < kParamFastTurnMs);
      lastParamTurnAt = now;
      int32_t nextValue = paramEditValue + direction * step;
      const int32_t minValue = paramMinValue(paramCursor);
      const int32_t maxValue = paramMaxValue(paramCursor);
      nextValue = nextValue < minValue
                      ? minValue
                      : (nextValue > maxValue ? maxValue : nextValue);
      if (nextValue == paramEditValue) {
        return ENCODER_ACTION_LIMIT;
      }
      paramEditValue = nextValue;
    } else {
      const int nextPage = static_cast<int>(testPage) + direction;
      if (nextPage < 0 || nextPage >= kTestPageCount) {
        return ENCODER_ACTION_LIMIT;
      }
      testPage = static_cast<uint8_t>(nextPage);
    }
  } else if (currentScreen == SCREEN_SETTINGS) {
    const int nextPosition = static_cast<int>(settingsPosition) + direction;
    if (nextPosition < 0 || nextPosition >= kMenuCount) {
      return ENCODER_ACTION_LIMIT;
    }
    settingsPosition = static_cast<uint8_t>(nextPosition);
  } else if (currentScreen == SCREEN_SETTINGS_NUMBER_EDIT) {
    if (settingsPosition == kMenuMaxFillTime) {
      const int64_t nextValue =
          static_cast<int64_t>(numberEditValue) + direction;
      if (nextValue < 1 || nextValue > 100) {
        return ENCODER_ACTION_LIMIT;
      }
      numberEditValue = static_cast<uint32_t>(nextValue);
    } else {
      // As on the home screen: minutes change the value by 60 s, seconds by 1 s.
      const int64_t step = numberEditField == 0 ? 60 : 1;
      const int64_t nextValue =
          static_cast<int64_t>(numberEditValue) + direction * step;
      if (nextValue < 1 || nextValue > MAX_EMPTY_TIME_SECONDS) {
        return ENCODER_ACTION_LIMIT;
      }
      numberEditValue = static_cast<uint32_t>(nextValue);
    }
  } else if (currentScreen == SCREEN_SETTINGS_CONFIRM) {
    const int nextSelection = static_cast<int>(confirmSelection) + direction;
    if (nextSelection < 0 || nextSelection > 1) {
      return ENCODER_ACTION_LIMIT;
    }
    confirmSelection = static_cast<uint8_t>(nextSelection);
  } else if (selectedOption == 0) {
    const int16_t nextValue = static_cast<int16_t>(config.cycles) + direction;
    if (nextValue < 1 || nextValue > 99) {
      return ENCODER_ACTION_LIMIT;
    }
    config.cycles = static_cast<uint8_t>(nextValue);
  } else if (selectedOption == 1) {
    const int32_t nextValue = static_cast<int32_t>(config.fill_time) + direction;
    const uint32_t maximum =
        static_cast<uint32_t>(config.max_fill_time) * 10;
    const uint32_t boundedMaximum = maximum < 999 ? maximum : 999;
    if (nextValue < 1 || nextValue > static_cast<int32_t>(boundedMaximum)) {
      return ENCODER_ACTION_LIMIT;
    }
    config.fill_time = static_cast<uint16_t>(nextValue);
  } else if (selectedOption == 2) {
    // Minutes change the value by 60 s, seconds by 1 s; the range is 1 s..max_empty_time.
    const int32_t step = emptyTimeField == 0 ? 60 : 1;
    const int32_t nextValue =
        static_cast<int32_t>(config.empty_time) + direction * step;
    if (nextValue < 1 || nextValue > config.max_empty_time) {
      return ENCODER_ACTION_LIMIT;
    }
    config.empty_time = static_cast<uint16_t>(nextValue);
  } else {
    cursorPosition = (cursorPosition + direction + 4) % 4;
  }

  markScreenDirty();
  return ENCODER_ACTION_CHANGED;
}

// Handles an encoder press depending on the active screen.
// Arguments: none. Returns: whether the press was handled; may change the screen,
// the configuration or request a reset.
bool handleButtonPress() {
  if (currentScreen == SCREEN_STATUS) {
    return false;
  }

  if (currentScreen == SCREEN_TEST) {
    if (testPage == kTestPageParams) {
      if (paramMode == kParamModeBrowse) {
        paramMode = kParamModeSelect;
        paramCursor = 0;
      } else if (paramMode == kParamModeSelect) {
        if (paramCursor >= kParamRowCount) {
          paramMode = kParamModeBrowse;
        } else {
          paramEditValue = paramStoredValue(paramCursor);
          lastParamTurnAt = millis();
          paramMode = kParamModeEdit;
        }
      } else {
        paramCommit(paramCursor, paramEditValue);
        paramMode = kParamModeSelect;
      }
      markScreenDirty();
      return true;
    }
    return false;
  }

  if (currentScreen == SCREEN_SETTINGS) {
    selectSettingsItem();
    return true;
  }

  if (currentScreen == SCREEN_SETTINGS_NUMBER_EDIT) {
    if (settingsPosition == kMenuMaxFillTime) {
      config.max_fill_time = static_cast<uint16_t>(numberEditValue);
      const uint16_t maximum =
          static_cast<uint16_t>(config.max_fill_time * 10UL);
      if (config.fill_time > maximum) {
        config.fill_time = maximum > 999 ? 999 : maximum;
      }
    } else if (numberEditField == 0) {
      numberEditField = 1;
      markScreenDirty();
      return true;
    } else {
      config.max_empty_time = static_cast<uint16_t>(numberEditValue);
      if (config.empty_time > config.max_empty_time) {
        config.empty_time = config.max_empty_time;
      }
    }
    configStoreRequestSave();
    returnToSettings();
    return true;
  }

  if (currentScreen == SCREEN_SETTINGS_CONFIRM) {
    if (confirmSelection == 0) {
      returnToSettings();
      return true;
    }

    procedureRequestSoftwareReset();
    return false;
  }

  if (currentScreen == SCREEN_HOME) {
    if (selectedOption == 2 && emptyTimeField == 0) {
      emptyTimeField = 1;
    } else if (selectedOption >= 0) {
      selectedOption = -1;
      emptyTimeField = 0;
      configStoreRequestSave();
    } else if (cursorPosition == 3) {
      currentScreen = SCREEN_SETTINGS;
    } else {
      selectedOption = cursorPosition;
      emptyTimeField = 0;
    }
    markScreenDirty();
    return true;
  }

  return false;
}

// Reads the encoder channels, decodes a full step and plays its sound.
// Arguments: none. Returns: nothing; updates the transition accumulator and the UI state.
void pollEncoder() {
  if (currentScreen == SCREEN_STATUS) {
    previousEncoderState = readEncoderState();
    encoderAccumulator = 0;
    return;
  }

  const uint8_t currentState = readEncoderState();
  const uint8_t transition = (previousEncoderState << 2) | currentState;
  previousEncoderState = currentState;
  encoderAccumulator += kEncoderTransitions[transition];

  if (encoderAccumulator >= 4) {
    encoderAccumulator = 0;
    const EncoderRotationResult result = handleRotation(1);
    if (result == ENCODER_ACTION_CHANGED) {
      procedureEncoderTurnBeep();
    } else if (result == ENCODER_ACTION_LIMIT) {
      procedureEncoderErrorBeep();
    }
  } else if (encoderAccumulator <= -4) {
    encoderAccumulator = 0;
    const EncoderRotationResult result = handleRotation(-1);
    if (result == ENCODER_ACTION_CHANGED) {
      procedureEncoderTurnBeep();
    } else if (result == ENCODER_ACTION_LIMIT) {
      procedureEncoderErrorBeep();
    }
  }
}

// Reads the encoder button with debouncing and runs its action.
// Arguments: none. Returns: nothing; updates the button state and may change the UI.
void pollEncoderButton() {
  const uint32_t now = millis();
  const bool reading = digitalRead(kEncoderButtonPin);

  if (reading != lastButtonReading) {
    lastButtonReading = reading;
    buttonChangedAt = now;
  }

  if (now - buttonChangedAt < kButtonDebounceMs ||
      reading == stableButtonState) {
    return;
  }

  stableButtonState = reading;
  if (stableButtonState == LOW) {
    if (handleButtonPress()) {
      procedureEncoderPressBeep();
    }
  }
}

// Checks whether the manual Z1/Z2 buttons may drive the outputs on the current screen.
// Arguments: none. Returns: true on the home screen and on all test mode pages.
bool manualControlsAllowed() {
  return currentScreen == SCREEN_HOME || currentScreen == SCREEN_TEST;
}

// Handles the manual Z1/Z2 buttons on the home screen and the lockout after a screen change.
// Arguments: none. Returns: nothing; may change the procedure outputs and the manual mode.
void updateHomeManualControls() {
  const bool fillPressed = digitalRead(kFillButtonPin) == LOW;
  const bool emptyPressed = digitalRead(kEmptyButtonPin) == LOW;

  if (!manualControlsAllowed()) {
    if (homeManualOutputMode != 0) {
      procedureSetManualOutputs(false, false);
      homeManualOutputMode = 0;
    }
    homeManualLockout = true;
    return;
  }

  if (homeManualLockout) {
    if (!fillPressed && !emptyPressed) {
      homeManualLockout = false;
    } else {
      return;
    }
  }

  const uint8_t requestedMode =
      fillPressed == emptyPressed ? 0 : (fillPressed ? 1 : 2);
  if (requestedMode == homeManualOutputMode) {
    return;
  }

  procedureSetManualOutputs(requestedMode == 1, requestedMode == 2);
  homeManualOutputMode = requestedMode;
  if (requestedMode != 0) {
    // Manual pumping means the bags may have been changed: the last result
    // no longer applies to what is connected.
    setLastRunResult(LAST_RUN_PENDING);
  }
  if (currentScreen == SCREEN_TEST) {
    markScreenDirty();
  }
}

// Handles test mode: pressure sampling, the manual Z1/Z2 buttons and screen refresh.
// Arguments: none. Returns: nothing; marks the screen for redraw when the data changed.
void updateTestMode() {
  procedureTestUpdate();
  updateHomeManualControls();

  PressureTestInfo info;
  procedureTestGetInfo(&info);
  const uint8_t address = lps28dfw_address();
  if (memcmp(&info, &testInfo, sizeof(info)) != 0 ||
      address != testInfoAddress) {
    testInfo = info;
    testInfoAddress = address;
    markScreenDirty();
  }
}

}  // namespace

const Config defaultConfig = {1, 50, 100, 30, 30, 500,
                              DEFAULT_CUTOFF_CONFIRM_SAMPLES,
                              DEFAULT_PRESSURE_CUTOFF_HPA};
Config config = defaultConfig;
int currentScreen = SCREEN_HOME;

// Initialises I2C, the user inputs, the debounce state and the OLED display.
// Arguments: none. Returns: nothing; configures the hardware and sets the UI flags.
void uiBegin() {
  Wire.setSDA(kSdaPin);
  Wire.setSCL(kSclPin);
  Wire.begin();

  pinMode(kEncoderClockPin, INPUT_PULLUP);
  pinMode(kEncoderDataPin, INPUT_PULLUP);
  pinMode(kEncoderButtonPin, INPUT_PULLUP);
  pinMode(kStartButtonPin, INPUT_PULLUP);
  pinMode(kStopButtonPin, INPUT_PULLUP);
  pinMode(kFillButtonPin, INPUT_PULLUP);
  pinMode(kEmptyButtonPin, INPUT_PULLUP);

  previousEncoderState = readEncoderState();
  lastButtonReading = digitalRead(kEncoderButtonPin);
  stableButtonState = lastButtonReading;
  buttonChangedAt = millis();
  startButton.lastReading = digitalRead(kStartButtonPin);
  startButton.stableState = startButton.lastReading;
  startButton.changedAt = millis();
  stopButton.lastReading = digitalRead(kStopButtonPin);
  stopButton.stableState = stopButton.lastReading;
  stopButton.changedAt = millis();

  displayReady = display.begin(SSD1306_SWITCHCAPVCC, kOledAddress);

  // STOP held at power-up enables test mode.
  delay(5);
  testModeActive = digitalRead(kStopButtonPin) == LOW;
  if (testModeActive) {
    currentScreen = SCREEN_TEST;
  }
  markScreenDirty();
}

// Runs a single pass over the buttons, the encoder and the manual inputs.
// Arguments: none. Returns: nothing; may call the procedure control and change the UI.
void processInput() {
  const uint32_t now = millis();
  if (testModeActive) {
    // In test mode, STOP cancels parameter editing.
    if (pollDebouncedButton(stopButton, now)) {
      if (testPage == kTestPageParams && paramMode != kParamModeBrowse) {
        paramMode = paramMode == kParamModeEdit ? kParamModeSelect
                                                : kParamModeBrowse;
        markScreenDirty();
      }
    }
    pollEncoder();
    pollEncoderButton();
    updateTestMode();
    return;
  }

  const bool stopPressed = pollDebouncedButton(stopButton, now);
  const bool startPressed = pollDebouncedButton(startButton, now);

  if (stopPressed) {
    homeManualLockout = true;
    homeManualOutputMode = 0;
    procedureStop();
  } else if (startPressed) {
    procedureStart();
  }

  if (!stopPressed) {
    pollEncoder();
    pollEncoderButton();
  }
  updateHomeManualControls();
}

// Switches the UI to the status screen and initialises its data.
// Arguments: phase, current cycle number, number of cycles, elapsed time and maximum (s).
// Returns: nothing; changes the screen and the status data.
void showStatusScreen(StatusPhase phase, uint8_t currentCycle,
                      uint8_t allCycles, uint32_t elapsedSeconds,
                      uint32_t maximumSeconds) {
  currentScreen = SCREEN_STATUS;
  hasDrawnScreen = false;
  updateStatusScreen(phase, currentCycle, allCycles, elapsedSeconds,
                     maximumSeconds);
  screenDirty = true;
}

// Updates the status data and marks the screen for redraw if anything changed.
// Arguments: phase, current cycle number, number of cycles, elapsed time and maximum (s).
// Returns: nothing; modifies the status state and, if needed, screenDirty.
void updateStatusScreen(StatusPhase phase, uint8_t currentCycle,
                        uint8_t allCycles, uint32_t elapsedSeconds,
                        uint32_t maximumSeconds) {
  if (statusPhase != phase || statusCurrentCycle != currentCycle ||
      statusAllCycles != allCycles ||
      statusElapsedSeconds != elapsedSeconds ||
      statusMaximumSeconds != maximumSeconds) {
    statusPhase = phase;
    statusCurrentCycle = currentCycle;
    statusAllCycles = allCycles;
    statusElapsedSeconds = elapsedSeconds;
    statusMaximumSeconds = maximumSeconds;
    screenDirty = true;
  }
}

// Sets the last-run result icon shown on the home screen.
// Arguments: the result. Returns: nothing; marks the screen for redraw.
void setLastRunResult(LastRunResult result) {
  lastRunResult = result;
  screenDirty = true;
}

// Returns to the home screen and clears the selection.
// Arguments: none. Returns: nothing; changes the screen, cursor and redraw flag.
void returnHomeScreen() {
  currentScreen = SCREEN_HOME;
  selectedOption = -1;
  cursorPosition = 0;
  screenDirty = true;
}

// Draws the current screen only when it is ready, has changed and the refresh interval has passed.
// Arguments: none. Returns: nothing; updates the OLED buffer and the screen sync state.
void updateScreen() {
  if (!displayReady || !screenDirty) {
    return;
  }

  const uint32_t now = millis();
  if (hasDrawnScreen && now - lastScreenUpdate < kMinimumRedrawIntervalMs) {
    return;
  }

  display.clearDisplay();
  if (currentScreen == SCREEN_SETTINGS) {
    drawSettingsMenu();
  } else if (currentScreen == SCREEN_SETTINGS_NUMBER_EDIT) {
    drawNumberEditScreen();
  } else if (currentScreen == SCREEN_SETTINGS_CONFIRM) {
    display.drawBitmap(0, 0, confirmBackground(), kScreenWidth, kScreenHeight,
                       SSD1306_WHITE);
  } else if (currentScreen == SCREEN_STATUS) {
    drawStatusScreen();
  } else if (currentScreen == SCREEN_TEST) {
    drawTestScreen();
  } else {
    drawHomeScreen();
  }
  display.display();

  screenDirty = false;
  hasDrawnScreen = true;
  lastScreenUpdate = now;
}