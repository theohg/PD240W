#include "settings.h"
#include "config/app_config.h"
#include "utils/logging.h"
#include <cstring>
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

// Global instance
Settings settings;

// Flash storage configuration
// Use the last 4KB sector of flash for settings
// RP2040 has 2MB flash, sectors are 4KB
static constexpr uint32_t FLASH_TARGET_OFFSET = (2 * 1024 * 1024) - FLASH_SECTOR_SIZE;  // Last sector
#define FLASH_TARGET_ADDR ((const uint8_t*)(XIP_BASE + FLASH_TARGET_OFFSET))

namespace {

struct UserSettingsV5 {
    uint32_t magic;
    uint8_t version;
    uint32_t current_limit_ma;
    int8_t last_pdo_index;
    bool load_switch_enabled;
    bool buck_17v_enabled;
    uint8_t lcd_brightness;
    bool sounds_enabled;
    bool auto_pps_enabled;
    uint8_t auto_dim_minutes;
    uint8_t startup_melody;
    bool auto_output;
    uint8_t last_contract_type;
    uint32_t last_requested_voltage_mv;
    uint32_t last_contract_min_voltage_mv;
    uint32_t last_contract_max_voltage_mv;
    uint8_t startup_negotiation;
    bool auto_avs_enabled;
    uint8_t energy_display_mode;
    bool cc_mode_enabled;
    uint32_t crc32;
};

struct UserSettingsV4 {
    uint32_t magic;
    uint8_t version;
    uint32_t current_limit_ma;
    int8_t last_pdo_index;
    bool load_switch_enabled;
    bool buck_17v_enabled;
    uint8_t lcd_brightness;
    bool sounds_enabled;
    bool auto_pps_enabled;
    uint8_t auto_dim_minutes;
    uint8_t startup_melody;
    bool auto_output;
    uint32_t last_pps_avs_voltage_mv;
    uint8_t startup_negotiation;
    bool auto_avs_enabled;
    uint8_t energy_display_mode;
    bool cc_mode_enabled;
    uint32_t crc32;
};

template <typename SettingsStruct>
uint32_t calculateSettingsCrc(const SettingsStruct& settings) {
    const uint8_t* data = reinterpret_cast<const uint8_t*>(&settings);
    size_t len = offsetof(SettingsStruct, crc32);

    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    return ~crc;
}

const char* savedStartupContractTypeName(SavedStartupContractType type) {
    switch (type) {
        case SavedStartupContractType::NONE: return "none";
        case SavedStartupContractType::UNKNOWN: return "legacy";
        case SavedStartupContractType::FIXED: return "fixed";
        case SavedStartupContractType::PPS: return "pps";
        case SavedStartupContractType::AVS: return "avs";
    }

    return "unknown";
}

CurrentLimitMode normalizeCurrentLimitMode(uint8_t raw_mode) {
    switch (static_cast<CurrentLimitMode>(raw_mode)) {
        case CurrentLimitMode::OFF:
        case CurrentLimitMode::OCP:
        case CurrentLimitMode::CC:
            return static_cast<CurrentLimitMode>(raw_mode);
    }

    return CurrentLimitMode::OCP;
}

CurrentLimitMode legacyCurrentLimitMode(bool cc_mode_enabled) {
    return cc_mode_enabled ? CurrentLimitMode::CC : CurrentLimitMode::OCP;
}

const char* currentLimitModeName(CurrentLimitMode mode) {
    switch (mode) {
        case CurrentLimitMode::OFF: return "OFF";
        case CurrentLimitMode::OCP: return "OCP";
        case CurrentLimitMode::CC: return "CC";
    }

    return "OCP";
}

}  // namespace

// ============================================================================
// Constructor
// ============================================================================

Settings::Settings()
    : _dirty(false)
    , _save_pending(false)
    , _save_scheduled(nil_time)
{
    // Zero-initialize settings struct
    memset(&_settings, 0, sizeof(_settings));
}

// ============================================================================
// CRC32 Calculation (simple implementation for data integrity)
// ============================================================================

uint32_t Settings::calculateCrc32() const {
    return calculateSettingsCrc(_settings);
}

// ============================================================================
// Initialization
// ============================================================================

void Settings::init() {
    // Try to load from flash first
    if (!loadFromFlash()) {
        // Flash load failed or no saved settings - use defaults
        resetToDefaults();
    }

    SavedStartupContractType saved_type = getLastContractType();
    if (saved_type == SavedStartupContractType::NONE) {
        LOG_INFO("Settings initialized: current_limit=%umA, saved_startup=none",
                 _settings.current_limit_ma);
        return;
    }

    if (getLastContractMinVoltageMv() > 0 || getLastContractMaxVoltageMv() > 0) {
        LOG_INFO("Settings initialized: current_limit=%umA, saved_startup=%s target=%umV range=%u-%umV hint_pdo=%d",
                 _settings.current_limit_ma,
                 savedStartupContractTypeName(saved_type),
                 getLastRequestedVoltageMv(),
                 getLastContractMinVoltageMv(),
                 getLastContractMaxVoltageMv(),
                 _settings.last_pdo_index);
    } else {
        LOG_INFO("Settings initialized: current_limit=%umA, saved_startup=%s target=%umV hint_pdo=%d",
                 _settings.current_limit_ma,
                 savedStartupContractTypeName(saved_type),
                 getLastRequestedVoltageMv(),
                 _settings.last_pdo_index);
    }
}

// ============================================================================
// Setters
// ============================================================================

void Settings::setCurrentLimit(uint32_t limit_ma) {
    // Clamp to valid range
    if (limit_ma < AppConfig::CURRENT_LIMIT_MIN_MA) {
        limit_ma = AppConfig::CURRENT_LIMIT_MIN_MA;
    }
    if (limit_ma > AppConfig::CURRENT_LIMIT_MAX_MA) {
        limit_ma = AppConfig::CURRENT_LIMIT_MAX_MA;
    }

    if (_settings.current_limit_ma != limit_ma) {
        _settings.current_limit_ma = limit_ma;
        _dirty = true;
        LOG_DEBUG("Current limit changed to %u mA", limit_ma);
    }
}

void Settings::setLastPdoIndex(int8_t index) {
    if (_settings.last_pdo_index != index) {
        _settings.last_pdo_index = index;
        _dirty = true;
    }
}

void Settings::setLoadSwitchEnabled(bool enabled) {
    if (_settings.load_switch_enabled != enabled) {
        _settings.load_switch_enabled = enabled;
        _dirty = true;
    }
}

void Settings::setBuck17vEnabled(bool enabled) {
    if (_settings.buck_17v_enabled != enabled) {
        _settings.buck_17v_enabled = enabled;
        _dirty = true;
    }
}

void Settings::setLcdBrightness(uint8_t brightness) {
    if (brightness > 100) brightness = 100;

    if (_settings.lcd_brightness != brightness) {
        _settings.lcd_brightness = brightness;
        _dirty = true;
    }
}

void Settings::setSoundsEnabled(bool enabled) {
    if (_settings.sounds_enabled != enabled) {
        _settings.sounds_enabled = enabled;
        _dirty = true;
        LOG_DEBUG("Sounds %s", enabled ? "enabled" : "disabled");
    }
}

void Settings::setAutoPpsEnabled(bool enabled) {
    if (_settings.auto_pps_enabled != enabled) {
        _settings.auto_pps_enabled = enabled;
        _dirty = true;
        LOG_DEBUG("Auto PPS %s", enabled ? "enabled" : "disabled");
    }
}

void Settings::setAutoAvsEnabled(bool enabled) {
    if (_settings.auto_avs_enabled != enabled) {
        _settings.auto_avs_enabled = enabled;
        _dirty = true;
        LOG_DEBUG("Auto AVS %s", enabled ? "enabled" : "disabled");
    }
}

void Settings::setAutoDimMinutes(uint8_t minutes) {
    if (minutes < AppConfig::AUTO_DIM_MIN_MINUTES) minutes = AppConfig::AUTO_DIM_MIN_MINUTES;
    if (minutes > AppConfig::AUTO_DIM_MAX_MINUTES) minutes = AppConfig::AUTO_DIM_MAX_MINUTES;
    if (_settings.auto_dim_minutes != minutes) {
        _settings.auto_dim_minutes = minutes;
        _dirty = true;
    }
}

void Settings::setStartupMelody(uint8_t melody) {
    if (melody > 3) melody = 3;
    if (_settings.startup_melody != melody) {
        _settings.startup_melody = melody;
        _dirty = true;
    }
}

void Settings::setAutoOutput(bool enabled) {
    if (_settings.auto_output != enabled) {
        _settings.auto_output = enabled;
        _dirty = true;
    }
}

void Settings::setLastContractType(SavedStartupContractType type) {
    uint8_t raw_type = static_cast<uint8_t>(type);
    if (_settings.last_contract_type != raw_type) {
        _settings.last_contract_type = raw_type;
        _dirty = true;
    }
}

void Settings::setLastRequestedVoltageMv(uint32_t voltage_mv) {
    if (_settings.last_requested_voltage_mv != voltage_mv) {
        _settings.last_requested_voltage_mv = voltage_mv;
        _dirty = true;
    }
}

void Settings::setLastContractRange(uint32_t min_voltage_mv, uint32_t max_voltage_mv) {
    if (_settings.last_contract_min_voltage_mv != min_voltage_mv ||
        _settings.last_contract_max_voltage_mv != max_voltage_mv) {
        _settings.last_contract_min_voltage_mv = min_voltage_mv;
        _settings.last_contract_max_voltage_mv = max_voltage_mv;
        _dirty = true;
    }
}

void Settings::setStartupNegotiation(uint8_t mode) {
    if (mode > 2) mode = 2;  // Clamp to valid range (0-2)
    if (_settings.startup_negotiation != mode) {
        _settings.startup_negotiation = mode;
        _dirty = true;
    }
}

void Settings::setEnergyDisplayMode(uint8_t mode) {
    if (mode > 1) mode = 1;
    if (_settings.energy_display_mode != mode) {
        _settings.energy_display_mode = mode;
        _dirty = true;
    }
}

void Settings::setCurrentLimitMode(CurrentLimitMode mode) {
    uint8_t raw_mode = static_cast<uint8_t>(normalizeCurrentLimitMode(static_cast<uint8_t>(mode)));
    if (_settings.current_limit_mode != raw_mode) {
        _settings.current_limit_mode = raw_mode;
        _dirty = true;
        LOG_DEBUG("Current limit mode %s", currentLimitModeName(static_cast<CurrentLimitMode>(raw_mode)));
    }
}

void Settings::setCcModeEnabled(bool enabled) {
    setCurrentLimitMode(enabled ? CurrentLimitMode::CC : CurrentLimitMode::OCP);
}

// ============================================================================
// Persistence
// ============================================================================

void Settings::requestSave() {
    // Schedule a save after debounce delay
    // Each call resets the timer, so rapid changes only result in one flash write
    _save_pending = true;
    _save_scheduled = make_timeout_time_ms(SETTINGS_SAVE_DEBOUNCE_MS);
}

void Settings::update() {
    // Check if a debounced save is due
    if (_save_pending && absolute_time_diff_us(_save_scheduled, get_absolute_time()) >= 0) {
        _save_pending = false;
        if (_dirty) {
            saveToFlash();
        }
    }
}

bool Settings::saveToFlash() {
    // Cancel any pending debounced save
    _save_pending = false;
    
    // Update CRC before saving
    _settings.crc32 = calculateCrc32();
    
    // Prepare data aligned to 256 bytes (minimum write size)
    uint8_t buffer[FLASH_PAGE_SIZE];
    memset(buffer, 0xFF, sizeof(buffer));  // Fill with 0xFF (erased state)
    memcpy(buffer, &_settings, sizeof(_settings));
    
    // Disable interrupts during flash operations
    uint32_t interrupts = save_and_disable_interrupts();
    
    // Erase the sector (4KB)
    flash_range_erase(FLASH_TARGET_OFFSET, FLASH_SECTOR_SIZE);
    
    // Write the settings (256 bytes minimum)
    flash_range_program(FLASH_TARGET_OFFSET, buffer, FLASH_PAGE_SIZE);
    
    // Restore interrupts
    restore_interrupts(interrupts);
    
    _dirty = false;
    LOG_INFO("Settings saved to flash");
    return true;
}

bool Settings::loadFromFlash() {
    const UserSettings* flash_settings = reinterpret_cast<const UserSettings*>(FLASH_TARGET_ADDR);
    
    // Validate magic number
    if (flash_settings->magic != SETTINGS_MAGIC) {
        LOG_DEBUG("Settings: Invalid magic (0x%08X), using defaults", flash_settings->magic);
        return false;
    }
    
    if (flash_settings->version == SETTINGS_VERSION) {
        memcpy(&_settings, flash_settings, sizeof(_settings));

        uint32_t expected_crc = _settings.crc32;
        if (calculateCrc32() != expected_crc) {
            LOG_WARN("Settings: CRC mismatch, using defaults");
            return false;
        }

        _settings.current_limit_mode = static_cast<uint8_t>(normalizeCurrentLimitMode(_settings.current_limit_mode));

        _dirty = false;
        LOG_INFO("Settings loaded from flash: brightness=%d, sounds=%d, auto_pps=%d, auto_avs=%d, auto_out_en=%d",
                 _settings.lcd_brightness, _settings.sounds_enabled, _settings.auto_pps_enabled, _settings.auto_avs_enabled, _settings.auto_output);
        return true;
    }

    if (flash_settings->version == 5) {
        const UserSettingsV5* legacy_settings = reinterpret_cast<const UserSettingsV5*>(FLASH_TARGET_ADDR);
        uint32_t expected_crc = legacy_settings->crc32;
        if (calculateSettingsCrc(*legacy_settings) != expected_crc) {
            LOG_WARN("Settings: Legacy CRC mismatch, using defaults");
            return false;
        }

        memset(&_settings, 0, sizeof(_settings));
        _settings.magic = SETTINGS_MAGIC;
        _settings.version = SETTINGS_VERSION;
        _settings.current_limit_ma = legacy_settings->current_limit_ma;
        _settings.last_pdo_index = legacy_settings->last_pdo_index;
        _settings.load_switch_enabled = legacy_settings->load_switch_enabled;
        _settings.buck_17v_enabled = legacy_settings->buck_17v_enabled;
        _settings.lcd_brightness = legacy_settings->lcd_brightness;
        _settings.sounds_enabled = legacy_settings->sounds_enabled;
        _settings.auto_pps_enabled = legacy_settings->auto_pps_enabled;
        _settings.auto_dim_minutes = legacy_settings->auto_dim_minutes;
        _settings.startup_melody = legacy_settings->startup_melody;
        _settings.auto_output = legacy_settings->auto_output;
        _settings.last_contract_type = legacy_settings->last_contract_type;
        _settings.last_requested_voltage_mv = legacy_settings->last_requested_voltage_mv;
        _settings.last_contract_min_voltage_mv = legacy_settings->last_contract_min_voltage_mv;
        _settings.last_contract_max_voltage_mv = legacy_settings->last_contract_max_voltage_mv;
        _settings.startup_negotiation = legacy_settings->startup_negotiation;
        _settings.auto_avs_enabled = legacy_settings->auto_avs_enabled;
        _settings.energy_display_mode = legacy_settings->energy_display_mode;
        _settings.current_limit_mode = static_cast<uint8_t>(legacyCurrentLimitMode(legacy_settings->cc_mode_enabled));
        _settings.crc32 = 0;

        _dirty = false;
        LOG_INFO("Settings migrated from v5: current limit mode=%s", currentLimitModeName(getCurrentLimitMode()));
        return true;
    }

    if (flash_settings->version == 4) {
        const UserSettingsV4* legacy_settings = reinterpret_cast<const UserSettingsV4*>(FLASH_TARGET_ADDR);
        uint32_t expected_crc = legacy_settings->crc32;
        if (calculateSettingsCrc(*legacy_settings) != expected_crc) {
            LOG_WARN("Settings: Legacy CRC mismatch, using defaults");
            return false;
        }

        memset(&_settings, 0, sizeof(_settings));
        _settings.magic = SETTINGS_MAGIC;
        _settings.version = SETTINGS_VERSION;
        _settings.current_limit_ma = legacy_settings->current_limit_ma;
        _settings.last_pdo_index = legacy_settings->last_pdo_index;
        _settings.load_switch_enabled = legacy_settings->load_switch_enabled;
        _settings.buck_17v_enabled = legacy_settings->buck_17v_enabled;
        _settings.lcd_brightness = legacy_settings->lcd_brightness;
        _settings.sounds_enabled = legacy_settings->sounds_enabled;
        _settings.auto_pps_enabled = legacy_settings->auto_pps_enabled;
        _settings.auto_dim_minutes = legacy_settings->auto_dim_minutes;
        _settings.startup_melody = legacy_settings->startup_melody;
        _settings.auto_output = legacy_settings->auto_output;
        _settings.startup_negotiation = legacy_settings->startup_negotiation;
        _settings.auto_avs_enabled = legacy_settings->auto_avs_enabled;
        _settings.energy_display_mode = legacy_settings->energy_display_mode;
        _settings.current_limit_mode = static_cast<uint8_t>(legacyCurrentLimitMode(legacy_settings->cc_mode_enabled));

        bool has_legacy_snapshot = (legacy_settings->last_pdo_index >= 0) ||
                                   (legacy_settings->last_pps_avs_voltage_mv > 0);
        if (has_legacy_snapshot) {
            _settings.last_contract_type = static_cast<uint8_t>(SavedStartupContractType::UNKNOWN);
            _settings.last_requested_voltage_mv = legacy_settings->last_pps_avs_voltage_mv;
        } else {
            _settings.last_pdo_index = -1;
            _settings.last_contract_type = static_cast<uint8_t>(SavedStartupContractType::NONE);
            _settings.last_requested_voltage_mv = 0;
        }
        _settings.last_contract_min_voltage_mv = 0;
        _settings.last_contract_max_voltage_mv = 0;
        _settings.crc32 = 0;

        _dirty = false;
        LOG_INFO("Settings migrated from v4: startup contract snapshot marked as legacy hint");
        return true;
    }

    LOG_DEBUG("Settings: Version mismatch (%d vs %d), using defaults",
              flash_settings->version, SETTINGS_VERSION);
    return false;
}

void Settings::resetToDefaults() {
    _settings.magic = SETTINGS_MAGIC;
    _settings.version = SETTINGS_VERSION;
    _settings.current_limit_ma = AppConfig::CURRENT_LIMIT_DEFAULT_MA;
    _settings.last_pdo_index = -1;
    _settings.load_switch_enabled = false;                          // Output disabled by default
    _settings.buck_17v_enabled = false;                             // Buck 17V disabled by default
    _settings.lcd_brightness = AppConfig::LCD_BRIGHTNESS_DEFAULT;   // Default brightness
    _settings.sounds_enabled = true;                                // Sounds ON by default
    _settings.auto_pps_enabled = true;                              // Auto PPS ON by default
    _settings.auto_avs_enabled = true;                              // Auto AVS ON by default
    _settings.auto_dim_minutes = 1;                                 // 1 minute dim timeout
    _settings.startup_melody = 1;                                   // Mario Power-Up by default
    _settings.auto_output = false;                                  // Output disabled by default
    _settings.last_contract_type = static_cast<uint8_t>(SavedStartupContractType::NONE);
    _settings.last_requested_voltage_mv = 0;                        // No saved startup contract target
    _settings.last_contract_min_voltage_mv = 0;
    _settings.last_contract_max_voltage_mv = 0;
    _settings.startup_negotiation = 2;                              // Last used (remember last contract)
    _settings.energy_display_mode = 0;                              // mAh by default
    _settings.current_limit_mode = static_cast<uint8_t>(CurrentLimitMode::OCP);
    _settings.crc32 = 0;                                            // Will be calculated on save

    _dirty = false;

    LOG_INFO("Settings reset to defaults");
}
