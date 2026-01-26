#include "settings.h"
#include "app_config.h"
#include "utils/logging.h"
#include <cstring>

// Global instance
Settings settings;

// ============================================================================
// Constructor
// ============================================================================

Settings::Settings()
    : _dirty(false)
{
    // Zero-initialize settings struct
    memset(&_settings, 0, sizeof(_settings));
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

// ============================================================================
// Persistence
// ============================================================================

bool Settings::saveToFlash() {
    // TODO: Implement flash storage using RP2040 flash APIs
    // For now, just mark as saved
    _dirty = false;
    LOG_DEBUG("Settings save requested (not implemented)");
    return true;
}

bool Settings::loadFromFlash() {
    // TODO: Implement flash loading
    // For now, always return false to trigger defaults
    LOG_DEBUG("Settings load requested (not implemented, using defaults)");
    return false;
}

void Settings::resetToDefaults() {
    _settings.current_limit_ma = AppConfig::CURRENT_LIMIT_DEFAULT_MA;
    _settings.last_pdo_index = 0;
    _settings.load_switch_enabled = false;  // Output disabled by default
    _settings.buck_17v_enabled = false;
    _settings.lcd_brightness = AppConfig::LCD_BRIGHTNESS_DEFAULT;

    memset(_settings.reserved, 0, sizeof(_settings.reserved));

    _dirty = false;

    LOG_INFO("Settings reset to defaults");
}
