#pragma once

#include <cstdint>
#include "pico/stdlib.h"
#include "logic/settings_types.h"    // pure flash enums (host-testable)
#include "logic/settings_storage.h"  // UserSettings layout + CRC/validation/migration (host-testable)

// ============================================================================
// User Settings Manager
// ============================================================================
// Manages user-configurable settings for the power supply.
// Settings are stored in RP2040 flash for persistence across power cycles.
//
// Flash wear reduction: Uses debounced saves (2 second delay after last change)
// Call update() in main loop to process pending saves.
// ============================================================================

// SETTINGS_MAGIC, SETTINGS_VERSION and the UserSettings flash layout now live in
// logic/settings_storage.h (included above) alongside the pure CRC / validation
// / migration logic, so all of it is host-testable.
//
// SavedStartupContractType, StartupContractMode, CurrentLimitMode and their
// helpers live in logic/settings_types.h (included above) so pure logic can
// depend on them without the Pico SDK.

// Debounce delay for flash writes (reduces wear)
constexpr uint32_t SETTINGS_SAVE_DEBOUNCE_MS = 2000;

class Settings {
public:
    Settings();

    // Initialize settings (load defaults or from flash)
    void init();

    // Get current settings (read-only)
    const UserSettings& get() const { return _settings; }

    // Modify settings
    void setCurrentLimit(uint32_t limit_ma);
    void setLastPdoIndex(int8_t index);
    void setLcdBrightness(uint8_t brightness);
    void setSoundsEnabled(bool enabled);
    void setAutoPpsEnabled(bool enabled);
    void setAutoAvsEnabled(bool enabled);
    void setAutoDimMinutes(uint8_t minutes);
    void setStartupMelody(uint8_t melody);
    void setAutoOutput(bool enabled);
    void setLastContractType(SavedStartupContractType type);
    void setLastRequestedVoltageMv(uint32_t voltage_mv);
    void setLastContractRange(uint32_t min_voltage_mv, uint32_t max_voltage_mv);
    void setStartupNegotiation(uint8_t mode);
    void setEnergyDisplayMode(uint8_t mode);
    void setCurrentLimitMode(CurrentLimitMode mode);
    void setCcModeEnabled(bool enabled);

    // Accessors
    uint32_t getCurrentLimit() const { return _settings.current_limit_ma; }
    int8_t getLastPdoIndex() const { return _settings.last_pdo_index; }
    uint8_t getLcdBrightness() const { return _settings.lcd_brightness; }
    bool isSoundsEnabled() const { return _settings.sounds_enabled; }
    bool isAutoPpsEnabled() const { return _settings.auto_pps_enabled; }
    bool isAutoAvsEnabled() const { return _settings.auto_avs_enabled; }
    uint8_t getAutoDimMinutes() const { return _settings.auto_dim_minutes; }
    uint8_t getStartupMelody() const { return _settings.startup_melody; }
    bool isAutoOutput() const { return _settings.auto_output; }
    SavedStartupContractType getLastContractType() const {
        return static_cast<SavedStartupContractType>(_settings.last_contract_type);
    }
    uint32_t getLastRequestedVoltageMv() const { return _settings.last_requested_voltage_mv; }
    uint32_t getLastContractMinVoltageMv() const { return _settings.last_contract_min_voltage_mv; }
    uint32_t getLastContractMaxVoltageMv() const { return _settings.last_contract_max_voltage_mv; }
    uint8_t getStartupNegotiation() const { return _settings.startup_negotiation; }
    StartupContractMode getStartupNegotiationMode() const {
        return static_cast<StartupContractMode>(_settings.startup_negotiation);
    }
    uint8_t getEnergyDisplayMode() const { return _settings.energy_display_mode; }
    CurrentLimitMode getCurrentLimitMode() const {
        return static_cast<CurrentLimitMode>(_settings.current_limit_mode);
    }
    bool isCcModeEnabled() const { return getCurrentLimitMode() == CurrentLimitMode::CC; }

    // Persistence
    void requestSave();      // Request a debounced save (will save after 2s delay)
    void update();           // Call in main loop to process pending saves
    bool saveToFlash();      // Force immediate save (bypasses debounce)
    bool loadFromFlash();
    void resetToDefaults();

private:
    UserSettings _settings;
    bool _dirty;                        // True if settings changed since last save
    bool _save_pending;                 // True if a debounced save is scheduled
    absolute_time_t _save_scheduled;    // When to execute the pending save
    
    // CRC32 calculation for data integrity
    uint32_t calculateCrc32() const;
};

// Global instance
extern Settings settings;
