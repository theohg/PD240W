#include "settings.h"
#include "app_config.h"
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
    // Calculate CRC32 over settings data (excluding the crc32 field itself)
    const uint8_t* data = reinterpret_cast<const uint8_t*>(&_settings);
    size_t len = offsetof(UserSettings, crc32);  // Don't include CRC in calculation
    
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    return ~crc;
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

    LOG_INFO("Settings initialized: current_limit=%umA, last_pdo=%d",
             _settings.current_limit_ma, _settings.last_pdo_index);
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

void Settings::setAutoDimMinutes(uint8_t minutes) {
    if (minutes < 1) minutes = 1;
    if (minutes > 10) minutes = 10;
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

void Settings::setLastPpsVoltageMv(uint32_t voltage_mv) {
    if (_settings.last_pps_voltage_mv != voltage_mv) {
        _settings.last_pps_voltage_mv = voltage_mv;
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
    // Read settings from flash
    const UserSettings* flash_settings = reinterpret_cast<const UserSettings*>(FLASH_TARGET_ADDR);
    
    // Validate magic number
    if (flash_settings->magic != SETTINGS_MAGIC) {
        LOG_DEBUG("Settings: Invalid magic (0x%08X), using defaults", flash_settings->magic);
        return false;
    }
    
    // Validate version
    if (flash_settings->version != SETTINGS_VERSION) {
        LOG_DEBUG("Settings: Version mismatch (%d vs %d), using defaults", 
                  flash_settings->version, SETTINGS_VERSION);
        return false;
    }
    
    // Copy to RAM
    memcpy(&_settings, flash_settings, sizeof(_settings));
    
    // Validate CRC
    uint32_t expected_crc = _settings.crc32;
    if (calculateCrc32() != expected_crc) {
        LOG_WARN("Settings: CRC mismatch, using defaults");
        return false;
    }
    
    _dirty = false;
    LOG_INFO("Settings loaded from flash: brightness=%d, sounds=%d, auto_pps=%d",
             _settings.lcd_brightness, _settings.sounds_enabled, _settings.auto_pps_enabled);
    return true;
}

void Settings::resetToDefaults() {
    _settings.magic = SETTINGS_MAGIC;
    _settings.version = SETTINGS_VERSION;
    _settings.current_limit_ma = AppConfig::CURRENT_LIMIT_DEFAULT_MA;
    _settings.last_pdo_index = 0;
    _settings.load_switch_enabled = false;  // Output disabled by default
    _settings.buck_17v_enabled = false;
    _settings.lcd_brightness = AppConfig::LCD_BRIGHTNESS_DEFAULT;
    _settings.sounds_enabled = true;        // Sounds ON by default
    _settings.auto_pps_enabled = false;     // Auto PPS OFF by default
    _settings.auto_dim_minutes = 1;         // 1 minute dim timeout
    _settings.startup_melody = 1;           // Mario Power-Up by default
    _settings.auto_output = false;          // Output disabled by default
    _settings.last_pps_voltage_mv = 0;      // No saved PPS voltage
    _settings.startup_negotiation = 2;      // Last used (remember last contract)

    memset(_settings.reserved, 0, sizeof(_settings.reserved));
    _settings.crc32 = 0;  // Will be calculated on save

    _dirty = false;

    LOG_INFO("Settings reset to defaults");
}
