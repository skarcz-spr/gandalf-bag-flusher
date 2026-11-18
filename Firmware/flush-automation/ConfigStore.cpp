#include <Arduino.h>
#include <EEPROM.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ConfigStore.h"
#include "Ui.h"

namespace {

constexpr uint32_t kRecordMagic = 0x43464731;
// v9: the current sensor (INA219) fields were removed; only the pressure sensor detects the cutoff.
constexpr uint16_t kRecordVersion = 9;
constexpr uint16_t kVersion8RecordVersion = 8;
constexpr uint16_t kVersion7RecordVersion = 7;
constexpr uint32_t kSaveDebounceMs = 750;
constexpr size_t kEepromBufferSize = 256;

struct ConfigRecord {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint8_t cycles;
  uint16_t fillTime;
  uint16_t maxFillTime;
  uint16_t emptyTime;
  uint16_t maxEmptyTime;
  uint32_t actuationTime;
  uint8_t cutoffConfirmSamples;
  uint16_t pressureCutoffHpa;
  uint32_t crc;
};

// Layout of v8 and v7 records (with current sensor fields). In v8 the empty time is in seconds,
// in v7 in tenths of a second. Records older than v7 are not migrated.
struct LegacyCurrentConfigRecord {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint8_t cycles;
  uint16_t fillTime;
  uint16_t maxFillTime;
  uint16_t emptyTime;
  uint16_t maxEmptyTime;
  uint32_t actuationTime;
  uint16_t pumpCurrentBaselineTenthsMa;
  uint8_t pumpCurrentBaselineValid;
  uint16_t pumpCurrentBlockedTenthsMa;
  uint16_t pumpCurrentRiseTenthsMa;
  uint8_t currentFilterSamples;
  uint8_t cutoffConfirmSamples;
  uint8_t currentFilterType;
  uint16_t pressureCutoffHpa;
  uint8_t cutoffMethod;
  uint32_t crc;
};

static_assert(sizeof(ConfigRecord) <= kEepromBufferSize,
              "Config record must fit in EEPROM emulation buffer");
static_assert(sizeof(LegacyCurrentConfigRecord) <= kEepromBufferSize,
              "Legacy record must fit in EEPROM emulation buffer");

bool storageReady = false;
bool savePending = false;
uint32_t saveNotBefore = 0;

// Calculates the CRC-32 of the given byte range.
// Arguments: data buffer and its length. Returns: the calculated checksum.
uint32_t calculateCrc32(const uint8_t *data, size_t length) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t index = 0; index < length; ++index) {
    crc ^= data[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
  }
  return ~crc;
}

// Verifies the record CRC calculated over the bytes before the crc field.
// Arguments: a record of any supported format. Returns: whether the CRC matches.
template <typename Record>
bool recordCrcValid(const Record &record) {
  const uint32_t expectedCrc = calculateCrc32(
      reinterpret_cast<const uint8_t *>(&record),
      offsetof(Record, crc));
  return record.crc == expectedCrc;
}

// Checks the value ranges of a current-version configuration record.
// Arguments: record to validate. Returns: true when all fields are in range.
bool configValuesValid(const ConfigRecord &record) {
  return record.cycles >= 1 && record.cycles <= 99 &&
         record.fillTime >= 1 && record.fillTime <= 999 &&
         record.maxFillTime >= 1 && record.maxFillTime <= 100 &&
         record.emptyTime >= 1 && record.emptyTime <= MAX_EMPTY_TIME_SECONDS &&
         record.maxEmptyTime >= 1 &&
         record.maxEmptyTime <= MAX_EMPTY_TIME_SECONDS &&
         record.emptyTime <= record.maxEmptyTime &&
         record.fillTime <= record.maxFillTime * 10UL &&
         record.actuationTime >= 1 && record.actuationTime <= 60000 &&
         record.cutoffConfirmSamples >= 1 &&
         record.cutoffConfirmSamples <= MAX_CUTOFF_CONFIRM_SAMPLES &&
         record.pressureCutoffHpa >= PRESSURE_CUTOFF_MIN_HPA &&
         record.pressureCutoffHpa <= PRESSURE_CUTOFF_MAX_HPA;
}

// Verifies the header, size, CRC and values of a current-version record.
// Arguments: record in the current format. Returns: true when the record is valid.
bool recordValid(const ConfigRecord &record) {
  return record.magic == kRecordMagic &&
         record.version == kRecordVersion &&
         record.size == sizeof(ConfigRecord) &&
         recordCrcValid(record) &&
         configValuesValid(record);
}

// Verifies a v8 or v7 record. Only the fields carried over to v9 are checked; the relation
// between the empty time and its maximum is not required, because the time is clamped after
// migration (the v8 defaults broke that relation and the record was rejected at every boot).
// Arguments: record and expected version. Returns: true when the record can be migrated.
bool legacyRecordValid(const LegacyCurrentConfigRecord &record,
                       uint16_t version) {
  return record.magic == kRecordMagic && record.version == version &&
         record.size == sizeof(LegacyCurrentConfigRecord) &&
         recordCrcValid(record) &&
         record.cycles >= 1 && record.cycles <= 99 &&
         record.fillTime >= 1 && record.fillTime <= 999 &&
         record.maxFillTime >= 1 && record.maxFillTime <= 100 &&
         record.emptyTime >= 1 &&
         record.maxEmptyTime >= 1 &&
         record.maxEmptyTime <= MAX_EMPTY_TIME_SECONDS &&
         record.actuationTime >= 1 && record.actuationTime <= 60000 &&
         record.cutoffConfirmSamples >= 1 &&
         record.cutoffConfirmSamples <= MAX_CUTOFF_CONFIRM_SAMPLES &&
         record.pressureCutoffHpa >= PRESSURE_CUTOFF_MIN_HPA &&
         record.pressureCutoffHpa <= PRESSURE_CUTOFF_MAX_HPA;
}

// Copies the fields of a current-version record into the global configuration.
// Arguments: source record. Returns: nothing; overwrites config.
void recordToConfig(const ConfigRecord &record) {
  config.cycles = record.cycles;
  config.fill_time = record.fillTime;
  config.max_fill_time = record.maxFillTime;
  config.empty_time = record.emptyTime;
  config.max_empty_time = record.maxEmptyTime;
  config.actuation_time = record.actuationTime;
  config.cutoff_confirm_samples = record.cutoffConfirmSamples;
  config.pressure_cutoff_hpa = record.pressureCutoffHpa;
}

// Migrates a v8/v7 record into config. The current sensor fields are dropped. A v7 record stored
// the empty time in tenths of a second, so it is reset to the default.
// Arguments: validated record. Returns: nothing; overwrites the global config.
void migrateLegacyRecord(const LegacyCurrentConfigRecord &record) {
  config = defaultConfig;
  config.cycles = record.cycles;
  config.fill_time = record.fillTime;
  config.max_fill_time = record.maxFillTime;
  config.max_empty_time = record.maxEmptyTime;
  config.actuation_time = record.actuationTime;
  config.cutoff_confirm_samples = record.cutoffConfirmSamples;
  config.pressure_cutoff_hpa = record.pressureCutoffHpa;
  if (record.version == kVersion8RecordVersion &&
      record.emptyTime <= MAX_EMPTY_TIME_SECONDS) {
    config.empty_time = record.emptyTime;
  }
}

// Makes the loaded configuration consistent: the fill time does not exceed
// max_fill_time*10 and the empty time does not exceed max_empty_time. Without this the saved
// record would fail validation at the next boot and the settings would be lost.
// Arguments: none. Returns: nothing; may reduce fill_time and empty_time.
void clampConfig() {
  const uint32_t fillMaximum = config.max_fill_time * 10UL;
  if (config.fill_time > fillMaximum) {
    config.fill_time = static_cast<uint16_t>(fillMaximum > 999 ? 999 : fillMaximum);
  }
  if (config.empty_time > config.max_empty_time) {
    config.empty_time = config.max_empty_time;
  }
}

// Builds the current EEPROM record from the global configuration and calculates its CRC.
// Arguments: none. Returns: the finished record; does not write it to EEPROM itself.
ConfigRecord configToRecord() {
  ConfigRecord record = {};
  record.magic = kRecordMagic;
  record.version = kRecordVersion;
  record.size = sizeof(ConfigRecord);
  record.cycles = config.cycles;
  record.fillTime = config.fill_time;
  record.maxFillTime = config.max_fill_time;
  record.emptyTime = config.empty_time;
  record.maxEmptyTime = config.max_empty_time;
  record.actuationTime = config.actuation_time;
  record.cutoffConfirmSamples = config.cutoff_confirm_samples;
  record.pressureCutoffHpa = config.pressure_cutoff_hpa;
  record.crc = calculateCrc32(
      reinterpret_cast<const uint8_t *>(&record),
      offsetof(ConfigRecord, crc));
  return record;
}

}  // namespace

// Starts EEPROM emulation, reads and validates the record, migrates v8/v7 or loads the defaults.
// Arguments: none. Returns: nothing; sets config and may request a save.
void configStoreBegin() {
  EEPROM.begin(kEepromBufferSize);
  storageReady = true;

  ConfigRecord record = {};
  EEPROM.get(0, record);
  if (recordValid(record)) {
    recordToConfig(record);
    return;
  }

  LegacyCurrentConfigRecord legacyRecord = {};
  EEPROM.get(0, legacyRecord);
  if (legacyRecordValid(legacyRecord, kVersion8RecordVersion) ||
      legacyRecordValid(legacyRecord, kVersion7RecordVersion)) {
    migrateLegacyRecord(legacyRecord);
  } else {
    config = defaultConfig;
  }
  clampConfig();
  configStoreRequestSave();
}

// Requests a configuration save after a period without further changes.
// Arguments: none. Returns: nothing; sets savePending and a 750 ms deadline if storage works.
void configStoreRequestSave() {
  if (!storageReady) {
    return;
  }
  savePending = true;
  saveNotBefore = millis() + kSaveDebounceMs;
}

// Saves the pending configuration after the debounce period and commits it to EEPROM.
// Arguments: none. Returns: nothing; may clear savePending after a successful commit.
void configStoreService() {
  if (!storageReady || !savePending ||
      static_cast<int32_t>(millis() - saveNotBefore) < 0) {
    return;
  }

  const ConfigRecord record = configToRecord();
  EEPROM.put(0, record);
  if (EEPROM.commit()) {
    savePending = false;
  }
}
