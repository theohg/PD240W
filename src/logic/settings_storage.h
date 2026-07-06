#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "config/app_config.h"
#include "logic/settings_types.h"

// ============================================================================
// Settings storage codec (pure, hardware-free)
// ============================================================================
// The persisted flash layout plus the pure logic that operates on it: CRC32
// integrity, magic/version validation, v4/v5 -> v6 migration, defaults, and the
// value clamps used by the setters. Split out of the Pico-SDK-coupled Settings
// class so this — the trickiest and most bug-prone part of settings — can be
// exercised by host unit tests (see test/test_settings_storage.cpp). This header
// includes only <cstdint>/<cstring>, app_config.h, and settings_types.h, so it
// is host-buildable.
//
// Settings (settings.cpp) owns the actual flash I/O, debounce timing, logging,
// and dirty-tracking; it delegates the byte-crunching here.
// ============================================================================

// Magic number to validate stored settings ("PD24").
constexpr uint32_t SETTINGS_MAGIC = 0x50443234;
// Current on-flash layout version. Bumping resets to defaults unless a migration
// path below understands the older layout.
constexpr uint8_t SETTINGS_VERSION = 6;

// Current persisted layout (v6). Kept a plain POD so it can be memcpy'd to/from
// flash and CRC'd byte-for-byte. Fields are never reordered without a version
// bump; new fields append before crc32.
struct UserSettings {
    // Magic number for validation
    uint32_t magic;

    // Settings version for future compatibility
    uint8_t version;

    // Current limit (mA)
    uint32_t current_limit_ma;

    // Saved startup-contract hint (PDO indices are charger-specific, so this is only a hint)
    int8_t last_pdo_index;

    // Output states (unused: accessors were removed as dead code). Retained only
    // to keep the flash layout stable; delete on the next SETTINGS_VERSION bump.
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

namespace SettingsStorage {

// Legacy on-flash layouts still understood for migration. Never edited — they
// are the exact byte layouts written by firmware v5 / v4.
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

// CRC32 (reflected, poly 0xEDB88320) over every byte preceding the trailing
// crc32 field. Templated so the legacy structs are validated with their own
// layout.
template <typename SettingsStruct>
uint32_t calculateCrc(const SettingsStruct& settings) {
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

// Outcome of interpreting a raw flash image. The three "loaded" results populate
// `out`; the three rejections leave it untouched (caller falls back to defaults).
enum class LoadStatus {
    LOADED,       // current-version image, CRC valid
    MIGRATED_V5,  // v5 image migrated into `out`
    MIGRATED_V4,  // v4 image migrated into `out`
    BAD_MAGIC,    // magic mismatch (blank/foreign sector)
    BAD_CRC,      // magic+version matched but CRC failed (corrupt)
    BAD_VERSION,  // magic ok, version neither current nor migratable
};

inline bool loadStatusOk(LoadStatus s) {
    return s == LoadStatus::LOADED || s == LoadStatus::MIGRATED_V5 ||
           s == LoadStatus::MIGRATED_V4;
}

inline CurrentLimitMode legacyCurrentLimitMode(bool cc_mode_enabled) {
    return cc_mode_enabled ? CurrentLimitMode::CC : CurrentLimitMode::OCP;
}

// Interpret a raw flash image and, on success, populate `out`. `flash_bytes`
// must point to at least sizeof(UserSettings) readable bytes (the largest
// candidate layout). Pure: no flash I/O, no logging, no globals.
inline LoadStatus loadFromBytes(const uint8_t* flash_bytes, UserSettings& out) {
    // magic/version share the same offset in every layout, so read them through
    // the current struct before committing to a specific version.
    const UserSettings* current = reinterpret_cast<const UserSettings*>(flash_bytes);

    if (current->magic != SETTINGS_MAGIC) {
        return LoadStatus::BAD_MAGIC;
    }

    if (current->version == SETTINGS_VERSION) {
        UserSettings candidate;
        memcpy(&candidate, current, sizeof(candidate));
        if (calculateCrc(candidate) != candidate.crc32) {
            return LoadStatus::BAD_CRC;
        }
        candidate.current_limit_mode =
            static_cast<uint8_t>(normalizeCurrentLimitMode(candidate.current_limit_mode));
        out = candidate;
        return LoadStatus::LOADED;
    }

    if (current->version == 5) {
        const UserSettingsV5* v5 = reinterpret_cast<const UserSettingsV5*>(flash_bytes);
        if (calculateCrc(*v5) != v5->crc32) {
            return LoadStatus::BAD_CRC;
        }

        memset(&out, 0, sizeof(out));
        out.magic = SETTINGS_MAGIC;
        out.version = SETTINGS_VERSION;
        out.current_limit_ma = v5->current_limit_ma;
        out.last_pdo_index = v5->last_pdo_index;
        out.load_switch_enabled = v5->load_switch_enabled;
        out.buck_17v_enabled = v5->buck_17v_enabled;
        out.lcd_brightness = v5->lcd_brightness;
        out.sounds_enabled = v5->sounds_enabled;
        out.auto_pps_enabled = v5->auto_pps_enabled;
        out.auto_dim_minutes = v5->auto_dim_minutes;
        out.startup_melody = v5->startup_melody;
        out.auto_output = v5->auto_output;
        out.last_contract_type = v5->last_contract_type;
        out.last_requested_voltage_mv = v5->last_requested_voltage_mv;
        out.last_contract_min_voltage_mv = v5->last_contract_min_voltage_mv;
        out.last_contract_max_voltage_mv = v5->last_contract_max_voltage_mv;
        out.startup_negotiation = v5->startup_negotiation;
        out.auto_avs_enabled = v5->auto_avs_enabled;
        out.energy_display_mode = v5->energy_display_mode;
        out.current_limit_mode = static_cast<uint8_t>(legacyCurrentLimitMode(v5->cc_mode_enabled));
        out.crc32 = 0;  // recomputed on next save
        return LoadStatus::MIGRATED_V5;
    }

    if (current->version == 4) {
        const UserSettingsV4* v4 = reinterpret_cast<const UserSettingsV4*>(flash_bytes);
        if (calculateCrc(*v4) != v4->crc32) {
            return LoadStatus::BAD_CRC;
        }

        memset(&out, 0, sizeof(out));
        out.magic = SETTINGS_MAGIC;
        out.version = SETTINGS_VERSION;
        out.current_limit_ma = v4->current_limit_ma;
        out.last_pdo_index = v4->last_pdo_index;
        out.load_switch_enabled = v4->load_switch_enabled;
        out.buck_17v_enabled = v4->buck_17v_enabled;
        out.lcd_brightness = v4->lcd_brightness;
        out.sounds_enabled = v4->sounds_enabled;
        out.auto_pps_enabled = v4->auto_pps_enabled;
        out.auto_dim_minutes = v4->auto_dim_minutes;
        out.startup_melody = v4->startup_melody;
        out.auto_output = v4->auto_output;
        out.startup_negotiation = v4->startup_negotiation;
        out.auto_avs_enabled = v4->auto_avs_enabled;
        out.energy_display_mode = v4->energy_display_mode;
        out.current_limit_mode = static_cast<uint8_t>(legacyCurrentLimitMode(v4->cc_mode_enabled));

        // v4 had no explicit startup-contract type; infer a legacy hint from the
        // old PDO index / PPS-AVS voltage fields.
        bool has_legacy_snapshot =
            (v4->last_pdo_index >= 0) || (v4->last_pps_avs_voltage_mv > 0);
        if (has_legacy_snapshot) {
            out.last_contract_type = static_cast<uint8_t>(SavedStartupContractType::UNKNOWN);
            out.last_requested_voltage_mv = v4->last_pps_avs_voltage_mv;
        } else {
            out.last_pdo_index = -1;
            out.last_contract_type = static_cast<uint8_t>(SavedStartupContractType::NONE);
            out.last_requested_voltage_mv = 0;
        }
        out.last_contract_min_voltage_mv = 0;
        out.last_contract_max_voltage_mv = 0;
        out.crc32 = 0;  // recomputed on next save
        return LoadStatus::MIGRATED_V4;
    }

    return LoadStatus::BAD_VERSION;
}

// Populate `out` with the factory defaults (matches Settings::resetToDefaults).
inline void applyDefaults(UserSettings& out) {
    out.magic = SETTINGS_MAGIC;
    out.version = SETTINGS_VERSION;
    out.current_limit_ma = AppConfig::CURRENT_LIMIT_DEFAULT_MA;
    out.last_pdo_index = -1;
    out.load_switch_enabled = false;
    out.buck_17v_enabled = false;
    out.lcd_brightness = AppConfig::LCD_BRIGHTNESS_DEFAULT;
    out.sounds_enabled = true;
    out.auto_pps_enabled = true;
    out.auto_avs_enabled = true;
    out.auto_dim_minutes = 1;
    out.startup_melody = 1;
    out.auto_output = false;
    out.last_contract_type = static_cast<uint8_t>(SavedStartupContractType::NONE);
    out.last_requested_voltage_mv = 0;
    out.last_contract_min_voltage_mv = 0;
    out.last_contract_max_voltage_mv = 0;
    out.startup_negotiation = 2;  // Last used
    out.energy_display_mode = 0;  // mAh
    out.current_limit_mode = static_cast<uint8_t>(CurrentLimitMode::OCP);
    out.crc32 = 0;  // computed on save
}

// ---------------------------------------------------------------------------
// Value clamps — the single source of truth for the Settings setters. Pure so
// tests can pin the accepted ranges without a Settings instance.
// ---------------------------------------------------------------------------
inline uint32_t clampCurrentLimit(uint32_t limit_ma) {
    if (limit_ma < AppConfig::CURRENT_LIMIT_MIN_MA) return AppConfig::CURRENT_LIMIT_MIN_MA;
    if (limit_ma > AppConfig::CURRENT_LIMIT_MAX_MA) return AppConfig::CURRENT_LIMIT_MAX_MA;
    return limit_ma;
}

inline uint8_t clampBrightness(uint8_t brightness) {
    return brightness > 100 ? 100 : brightness;
}

inline uint8_t clampAutoDimMinutes(uint8_t minutes) {
    if (minutes < AppConfig::AUTO_DIM_MIN_MINUTES) return AppConfig::AUTO_DIM_MIN_MINUTES;
    if (minutes > AppConfig::AUTO_DIM_MAX_MINUTES) return AppConfig::AUTO_DIM_MAX_MINUTES;
    return minutes;
}

inline uint8_t clampStartupMelody(uint8_t melody) {
    return melody > 3 ? 3 : melody;
}

inline uint8_t clampStartupNegotiation(uint8_t mode) {
    return mode > 2 ? 2 : mode;  // 0=Lowest, 1=Highest, 2=Last used
}

inline uint8_t clampEnergyDisplayMode(uint8_t mode) {
    return mode > 1 ? 1 : mode;  // 0=mAh, 1=mWh
}

}  // namespace SettingsStorage
