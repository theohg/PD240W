#pragma once

#include <cstdint>

// ============================================================================
// User Settings Manager
// ============================================================================
// Manages user-configurable settings for the power supply.
// Settings are stored in RP2040 flash for persistence across power cycles.
// ============================================================================

// Magic number to validate stored settings
constexpr uint32_t SETTINGS_MAGIC = 0x50443234;  // "PD24"
constexpr uint8_t SETTINGS_VERSION = 1;

struct UserSettings {
    // Magic number for validation
    uint32_t magic;
    
    // Settings version for future compatibility
    uint8_t version;

    // Current limit (mA)
    uint32_t current_limit_ma;

    // Last selected PDO index
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

    // Reserved for future use
    uint8_t reserved[12];
    
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

    // Accessors
    uint32_t getCurrentLimit() const { return _settings.current_limit_ma; }
    int8_t getLastPdoIndex() const { return _settings.last_pdo_index; }
    bool isLoadSwitchEnabled() const { return _settings.load_switch_enabled; }
    bool isBuck17vEnabled() const { return _settings.buck_17v_enabled; }
    uint8_t getLcdBrightness() const { return _settings.lcd_brightness; }
    bool isSoundsEnabled() const { return _settings.sounds_enabled; }
    bool isAutoPpsEnabled() const { return _settings.auto_pps_enabled; }

    // Persistence
    bool saveToFlash();
    bool loadFromFlash();
    void resetToDefaults();

private:
    UserSettings _settings;
    bool _dirty;  // True if settings changed since last save
    
    // CRC32 calculation for data integrity
    uint32_t calculateCrc32() const;
};

// Global instance
extern Settings settings;
