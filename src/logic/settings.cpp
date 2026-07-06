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
// Use the last 4KB sector of flash for settings.
// PICO_FLASH_SIZE_BYTES tracks the configured chip size (from the board config),
// so settings always land in the true last sector regardless of flash size.
static constexpr uint32_t FLASH_TARGET_OFFSET = PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE;  // Last sector
#define FLASH_TARGET_ADDR ((const uint8_t*)(XIP_BASE + FLASH_TARGET_OFFSET))

namespace {

using namespace SettingsStorage;

// UserSettings layout, legacy V4/V5 structs, the CRC template, defaults, the
// load/validate/migrate logic, and the value clamps now live in the pure,
// host-testable logic/settings_storage.h. This file keeps only the flash I/O,
// debounce timing, logging, and dirty-tracking.

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

// currentLimitModeName / normalizeCurrentLimitMode live in settings_types.h.

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
    return SettingsStorage::calculateCrc(_settings);
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
        LOG_INFO("Settings initialized: current_limit=%lumA, saved_startup=none",
                 _settings.current_limit_ma);
        return;
    }

    if (getLastContractMinVoltageMv() > 0 || getLastContractMaxVoltageMv() > 0) {
        LOG_INFO("Settings initialized: current_limit=%lumA, saved_startup=%s target=%lumV range=%lu-%lumV hint_pdo=%d",
                 _settings.current_limit_ma,
                 savedStartupContractTypeName(saved_type),
                 getLastRequestedVoltageMv(),
                 getLastContractMinVoltageMv(),
                 getLastContractMaxVoltageMv(),
                 _settings.last_pdo_index);
    } else {
        LOG_INFO("Settings initialized: current_limit=%lumA, saved_startup=%s target=%lumV hint_pdo=%d",
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
    limit_ma = SettingsStorage::clampCurrentLimit(limit_ma);

    if (_settings.current_limit_ma != limit_ma) {
        _settings.current_limit_ma = limit_ma;
        _dirty = true;
        LOG_DEBUG("Current limit changed to %lu mA", limit_ma);
    }
}

void Settings::setLastPdoIndex(int8_t index) {
    if (_settings.last_pdo_index != index) {
        _settings.last_pdo_index = index;
        _dirty = true;
    }
}

void Settings::setLcdBrightness(uint8_t brightness) {
    brightness = SettingsStorage::clampBrightness(brightness);

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
    minutes = SettingsStorage::clampAutoDimMinutes(minutes);
    if (_settings.auto_dim_minutes != minutes) {
        _settings.auto_dim_minutes = minutes;
        _dirty = true;
    }
}

void Settings::setStartupMelody(uint8_t melody) {
    melody = SettingsStorage::clampStartupMelody(melody);
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
    mode = SettingsStorage::clampStartupNegotiation(mode);
    if (_settings.startup_negotiation != mode) {
        _settings.startup_negotiation = mode;
        _dirty = true;
    }
}

void Settings::setEnergyDisplayMode(uint8_t mode) {
    mode = SettingsStorage::clampEnergyDisplayMode(mode);
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
    
    // Disable interrupts during flash operations.
    //
    // ACCEPTED OVERCURRENT-LATENCY BOUND: the 4 KB sector erase + page program
    // runs from RAM with XIP paused and ALL interrupts masked (on the order of
    // tens of milliseconds). During that window the overcurrent ISR
    // (interrupts.cpp isrOvercurrent) cannot run, so if the INA228 ALERT asserts
    // mid-write the load switch stays ON until the write completes. This is a
    // deliberate, bounded tradeoff rather than a bug:
    //   - The ALERT edge is not lost: the RP2040 GPIO controller latches the
    //     pending IRQ, so the ISR fires (and cuts power) the instant interrupts
    //     are restored below. Worst-case software cut latency == this flash op.
    //   - The TPS26750's own hardware overcurrent/OVP protection is the backstop
    //     that covers this window independently of the RP2040.
    //   - Saves are debounced (SETTINGS_SAVE_DEBOUNCE_MS), so this window is
    //     entered at most once per burst of settings changes, not per change.
    // If this bound ever becomes unacceptable, defer the save while the output is
    // enabled and drawing significant current (see CODE_REVIEW.md P2.3 option b).
    uint32_t interrupts = save_and_disable_interrupts();

    // Erase the sector (4KB)
    flash_range_erase(FLASH_TARGET_OFFSET, FLASH_SECTOR_SIZE);

    // Write the settings (256 bytes minimum)
    flash_range_program(FLASH_TARGET_OFFSET, buffer, FLASH_PAGE_SIZE);

    // Restore interrupts (fires any overcurrent IRQ latched during the write)
    restore_interrupts(interrupts);
    
    _dirty = false;
    LOG_INFO("Settings saved to flash");
    return true;
}

bool Settings::loadFromFlash() {
    // Byte-crunching (magic/version/CRC validation + v4/v5 migration) lives in
    // the pure logic/settings_storage.h; this wrapper adds flash access, logging,
    // and dirty-tracking.
    const uint8_t* flash_bytes = reinterpret_cast<const uint8_t*>(FLASH_TARGET_ADDR);
    UserSettings loaded;
    SettingsStorage::LoadStatus status = SettingsStorage::loadFromBytes(flash_bytes, loaded);

    switch (status) {
        case SettingsStorage::LoadStatus::LOADED:
            _settings = loaded;
            _dirty = false;
            LOG_INFO("Settings loaded from flash: brightness=%d, sounds=%d, auto_pps=%d, auto_avs=%d, auto_out_en=%d",
                     _settings.lcd_brightness, _settings.sounds_enabled, _settings.auto_pps_enabled, _settings.auto_avs_enabled, _settings.auto_output);
            return true;

        case SettingsStorage::LoadStatus::MIGRATED_V5:
            _settings = loaded;
            _dirty = false;
            LOG_INFO("Settings migrated from v5: current limit mode=%s", currentLimitModeName(getCurrentLimitMode()));
            return true;

        case SettingsStorage::LoadStatus::MIGRATED_V4:
            _settings = loaded;
            _dirty = false;
            LOG_INFO("Settings migrated from v4: startup contract snapshot marked as legacy hint");
            return true;

        case SettingsStorage::LoadStatus::BAD_MAGIC: {
            const UserSettings* flash_settings = reinterpret_cast<const UserSettings*>(FLASH_TARGET_ADDR);
            LOG_DEBUG("Settings: Invalid magic (0x%08lX), using defaults", flash_settings->magic);
            return false;
        }

        case SettingsStorage::LoadStatus::BAD_CRC:
            LOG_WARN("Settings: CRC mismatch, using defaults");
            return false;

        case SettingsStorage::LoadStatus::BAD_VERSION: {
            const UserSettings* flash_settings = reinterpret_cast<const UserSettings*>(FLASH_TARGET_ADDR);
            LOG_DEBUG("Settings: Version mismatch (%d vs %d), using defaults",
                      flash_settings->version, SETTINGS_VERSION);
            return false;
        }
    }

    return false;
}

void Settings::resetToDefaults() {
    SettingsStorage::applyDefaults(_settings);
    _dirty = false;
    LOG_INFO("Settings reset to defaults");
}
