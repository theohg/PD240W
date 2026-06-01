#pragma once

#include <cstdint>
#include "pico/stdlib.h"

// ============================================================================
// User Settings Manager
// ============================================================================
// Manages user-configurable settings for the power supply.
// Settings are stored in RP2040 flash for persistence across power cycles.
//
// Flash wear reduction: Uses debounced saves (2 second delay after last change)
// Call update() in main loop to process pending saves.
// ============================================================================

// Magic number to validate stored settings
constexpr uint32_t SETTINGS_MAGIC = 0x50443234;  // "PD24"
constexpr uint8_t SETTINGS_VERSION = 6;

enum class SavedStartupContractType : uint8_t {
    NONE = 0,
    UNKNOWN = 1,
    FIXED = 2,
    PPS = 3,
    AVS = 4,
};

// Startup contract negotiation modes
enum class StartupContractMode : uint8_t {
    LOWEST_VOLTAGE = 0,   // Negotiate lowest voltage available
    HIGHEST_VOLTAGE = 1,  // Negotiate highest voltage available
    LAST_USED = 2         // Restore last used contract (closest if unavailable)
};

// Current limit operating modes
enum class CurrentLimitMode : uint8_t {
    OFF = 0,
    OCP = 1,
    CC = 2,
};

// Debounce delay for flash writes (reduces wear)
constexpr uint32_t SETTINGS_SAVE_DEBOUNCE_MS = 2000;

struct UserSettings {
    // Magic number for validation
    uint32_t magic;
    
    // Settings version for future compatibility
    uint8_t version;

    // Current limit (mA)
    uint32_t current_limit_ma;

    // Saved startup-contract hint (PDO indices are charger-specific, so this is only a hint)
    int8_t last_pdo_index;

    // Output states (for restoration after power cycle - future use)
    bool load_switch_enabled;
    bool buck_17v_enabled;

    // Display settings
    uint8_t lcd_brightness;  // 0-100%

    // Sound settings
    bool sounds_enabled;     // ON/OFF for navigation buzzer sounds

    // Auto PPS tuning
    bool auto_pps_enabled;   // ON/OFF for automatic PPS voltage calibration

    // Auto-dim timeout (minutes)
    uint8_t auto_dim_minutes;  // 0-10, 0 = OFF, default 1

    // Startup melody selection
    uint8_t startup_melody;    // 0=Silent, 1=Mario, 2=Chime, 3=TwoTone

    // Auto output on boot
    bool auto_output;          // If true, enable output after boot completes

    // Saved startup contract snapshot for LAST_USED restore
    uint8_t last_contract_type;             // SavedStartupContractType
    uint32_t last_requested_voltage_mv;     // Requested fixed/PPS/AVS target [mV]
    uint32_t last_contract_min_voltage_mv;  // Advertised range min, or fixed voltage [mV]
    uint32_t last_contract_max_voltage_mv;  // Advertised range max, or fixed voltage [mV]

    // Startup contract negotiation mode
    uint8_t startup_negotiation;  // 0=Lowest voltage, 1=Highest voltage, 2=Last used

    // Auto AVS tuning
    bool auto_avs_enabled;   // ON/OFF for automatic AVS voltage calibration

    // Energy display mode: 0 = mAh, 1 = mWh
    uint8_t energy_display_mode;

    // Current limit operating mode (OFF / OCP / CC)
    uint8_t current_limit_mode;

    // CRC32 for data integrity
    uint32_t crc32;
};

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
    void setLoadSwitchEnabled(bool enabled);
    void setBuck17vEnabled(bool enabled);
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
    bool isLoadSwitchEnabled() const { return _settings.load_switch_enabled; }
    bool isBuck17vEnabled() const { return _settings.buck_17v_enabled; }
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
