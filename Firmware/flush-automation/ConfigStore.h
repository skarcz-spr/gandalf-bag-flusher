#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

// Initialises EEPROM and loads the current record, migrates a v8/v7 record or applies the defaults.
// Arguments/return value: none; sets the global configuration and may schedule a save.
void configStoreBegin();
// Requests a deferred save of the current configuration.
// Arguments/return value: none; sets the save flag and the debounce deadline.
void configStoreRequestSave();
// Performs a pending save once the deadline has passed and commits it to EEPROM.
// Arguments/return value: none; may clear the pending-save flag.
void configStoreService();

#endif