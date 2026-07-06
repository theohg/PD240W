// Host unit tests for the pure settings-storage codec in
// src/logic/settings_storage.h: CRC32 integrity, magic/version validation,
// v4/v5 -> v6 migration, defaults, and the setter value clamps. These exercise
// the real production functions the firmware's Settings class delegates to.

#include <snitch/snitch.hpp>

#include <cstdint>
#include <cstring>

#include "config/app_config.h"
#include "logic/settings_storage.h"

using namespace SettingsStorage;

namespace {

// A valid current-version image: defaults with a freshly stamped CRC.
UserSettings validImage() {
    UserSettings s;
    applyDefaults(s);
    s.crc32 = calculateCrc(s);
    return s;
}

// Reinterpret any layout struct as the raw byte pointer loadFromBytes consumes.
template <typename T>
const uint8_t* asBytes(const T& s) {
    return reinterpret_cast<const uint8_t*>(&s);
}

}  // namespace

// ============================================================================
// calculateCrc — integrity round-trip
// ============================================================================
TEST_CASE("calculateCrc: stable and change-sensitive", "[settings]") {
    UserSettings a = validImage();
    UserSettings b = a;

    CHECK(calculateCrc(a) == calculateCrc(b));  // deterministic

    b.current_limit_ma += 1;
    CHECK(calculateCrc(a) != calculateCrc(b));  // any payload change flips CRC
}

TEST_CASE("calculateCrc: excludes the trailing crc32 field", "[settings]") {
    UserSettings a = validImage();
    UserSettings b = a;
    b.crc32 = a.crc32 ^ 0xDEADBEEF;  // scribble only the CRC slot
    CHECK(calculateCrc(a) == calculateCrc(b));
}

// ============================================================================
// loadFromBytes — current version
// ============================================================================
TEST_CASE("loadFromBytes: valid current-version image loads", "[settings]") {
    UserSettings img = validImage();
    img.current_limit_ma = 2500;
    img.lcd_brightness = 42;
    img.crc32 = calculateCrc(img);

    UserSettings out;
    memset(&out, 0, sizeof(out));
    auto status = loadFromBytes(asBytes(img), out);

    REQUIRE(status == LoadStatus::LOADED);
    CHECK(loadStatusOk(status));
    CHECK(out.current_limit_ma == 2500);
    CHECK(out.lcd_brightness == 42);
    CHECK(out.magic == SETTINGS_MAGIC);
    CHECK(out.version == SETTINGS_VERSION);
}

TEST_CASE("loadFromBytes: bad magic is rejected", "[settings]") {
    UserSettings img = validImage();
    img.magic = 0xBADBAD00;
    img.crc32 = calculateCrc(img);  // CRC is over payload incl. magic

    UserSettings out = validImage();  // seed with something recognizable
    auto status = loadFromBytes(asBytes(img), out);

    CHECK(status == LoadStatus::BAD_MAGIC);
    CHECK_FALSE(loadStatusOk(status));
}

TEST_CASE("loadFromBytes: corrupt CRC on current version is rejected", "[settings]") {
    UserSettings img = validImage();
    img.crc32 = img.crc32 ^ 0x1;  // flip one bit -> mismatch

    UserSettings out;
    memset(&out, 0, sizeof(out));
    auto status = loadFromBytes(asBytes(img), out);

    CHECK(status == LoadStatus::BAD_CRC);
}

TEST_CASE("loadFromBytes: unknown version (magic ok) is rejected", "[settings]") {
    UserSettings img = validImage();
    img.version = 99;
    img.crc32 = calculateCrc(img);

    UserSettings out;
    memset(&out, 0, sizeof(out));
    auto status = loadFromBytes(asBytes(img), out);

    CHECK(status == LoadStatus::BAD_VERSION);
}

TEST_CASE("loadFromBytes: garbage current_limit_mode normalizes to OCP", "[settings]") {
    UserSettings img = validImage();
    img.current_limit_mode = 200;  // not OFF/OCP/CC
    img.crc32 = calculateCrc(img);

    UserSettings out;
    auto status = loadFromBytes(asBytes(img), out);

    REQUIRE(status == LoadStatus::LOADED);
    CHECK(out.current_limit_mode == static_cast<uint8_t>(CurrentLimitMode::OCP));
}

TEST_CASE("loadFromBytes: blank (all-0xFF) flash sector is rejected", "[settings]") {
    // A never-written sector reads as all-ones; magic won't match.
    alignas(UserSettings) uint8_t blank[sizeof(UserSettings)];
    memset(blank, 0xFF, sizeof(blank));

    UserSettings out;
    auto status = loadFromBytes(blank, out);
    CHECK(status == LoadStatus::BAD_MAGIC);
}

// ============================================================================
// loadFromBytes — v5 migration
// ============================================================================
TEST_CASE("loadFromBytes: v5 image migrates and maps fields", "[settings]") {
    UserSettingsV5 v5;
    memset(&v5, 0, sizeof(v5));
    v5.magic = SETTINGS_MAGIC;
    v5.version = 5;
    v5.current_limit_ma = 3300;
    v5.last_pdo_index = 2;
    v5.lcd_brightness = 60;
    v5.sounds_enabled = true;
    v5.auto_pps_enabled = true;
    v5.auto_dim_minutes = 3;
    v5.startup_melody = 2;
    v5.auto_output = true;
    v5.last_contract_type = static_cast<uint8_t>(SavedStartupContractType::PPS);
    v5.last_requested_voltage_mv = 9000;
    v5.last_contract_min_voltage_mv = 3300;
    v5.last_contract_max_voltage_mv = 21000;
    v5.startup_negotiation = 1;
    v5.auto_avs_enabled = true;
    v5.energy_display_mode = 1;
    v5.cc_mode_enabled = true;  // -> CurrentLimitMode::CC
    v5.crc32 = calculateCrc(v5);

    UserSettings out;
    auto status = loadFromBytes(asBytes(v5), out);

    REQUIRE(status == LoadStatus::MIGRATED_V5);
    CHECK(out.version == SETTINGS_VERSION);
    CHECK(out.current_limit_ma == 3300);
    CHECK(out.last_pdo_index == 2);
    CHECK(out.lcd_brightness == 60);
    CHECK(out.startup_melody == 2);
    CHECK(out.last_contract_type == static_cast<uint8_t>(SavedStartupContractType::PPS));
    CHECK(out.last_requested_voltage_mv == 9000);
    CHECK(out.last_contract_max_voltage_mv == 21000);
    CHECK(out.energy_display_mode == 1);
    CHECK(out.current_limit_mode == static_cast<uint8_t>(CurrentLimitMode::CC));
    CHECK(out.crc32 == 0);  // recomputed on next save
}

TEST_CASE("loadFromBytes: v5 cc_mode_enabled=false maps to OCP", "[settings]") {
    UserSettingsV5 v5;
    memset(&v5, 0, sizeof(v5));
    v5.magic = SETTINGS_MAGIC;
    v5.version = 5;
    v5.cc_mode_enabled = false;
    v5.crc32 = calculateCrc(v5);

    UserSettings out;
    auto status = loadFromBytes(asBytes(v5), out);
    REQUIRE(status == LoadStatus::MIGRATED_V5);
    CHECK(out.current_limit_mode == static_cast<uint8_t>(CurrentLimitMode::OCP));
}

TEST_CASE("loadFromBytes: v5 with bad CRC is rejected", "[settings]") {
    UserSettingsV5 v5;
    memset(&v5, 0, sizeof(v5));
    v5.magic = SETTINGS_MAGIC;
    v5.version = 5;
    v5.crc32 = calculateCrc(v5) ^ 0x1;

    UserSettings out;
    auto status = loadFromBytes(asBytes(v5), out);
    CHECK(status == LoadStatus::BAD_CRC);
}

// ============================================================================
// loadFromBytes — v4 migration
// ============================================================================
TEST_CASE("loadFromBytes: v4 with legacy snapshot -> UNKNOWN hint", "[settings]") {
    UserSettingsV4 v4;
    memset(&v4, 0, sizeof(v4));
    v4.magic = SETTINGS_MAGIC;
    v4.version = 4;
    v4.current_limit_ma = 1500;
    v4.last_pdo_index = 3;              // >= 0 -> legacy snapshot present
    v4.last_pps_avs_voltage_mv = 12000;
    v4.cc_mode_enabled = false;
    v4.crc32 = calculateCrc(v4);

    UserSettings out;
    auto status = loadFromBytes(asBytes(v4), out);

    REQUIRE(status == LoadStatus::MIGRATED_V4);
    CHECK(out.current_limit_ma == 1500);
    CHECK(out.last_contract_type == static_cast<uint8_t>(SavedStartupContractType::UNKNOWN));
    CHECK(out.last_requested_voltage_mv == 12000);
    CHECK(out.last_contract_min_voltage_mv == 0);
    CHECK(out.last_contract_max_voltage_mv == 0);
    CHECK(out.current_limit_mode == static_cast<uint8_t>(CurrentLimitMode::OCP));
    CHECK(out.crc32 == 0);
}

TEST_CASE("loadFromBytes: v4 with no snapshot -> NONE", "[settings]") {
    UserSettingsV4 v4;
    memset(&v4, 0, sizeof(v4));
    v4.magic = SETTINGS_MAGIC;
    v4.version = 4;
    v4.last_pdo_index = -1;            // no snapshot
    v4.last_pps_avs_voltage_mv = 0;
    v4.cc_mode_enabled = true;         // -> CC
    v4.crc32 = calculateCrc(v4);

    UserSettings out;
    auto status = loadFromBytes(asBytes(v4), out);

    REQUIRE(status == LoadStatus::MIGRATED_V4);
    CHECK(out.last_pdo_index == -1);
    CHECK(out.last_contract_type == static_cast<uint8_t>(SavedStartupContractType::NONE));
    CHECK(out.last_requested_voltage_mv == 0);
    CHECK(out.current_limit_mode == static_cast<uint8_t>(CurrentLimitMode::CC));
}

// ============================================================================
// applyDefaults
// ============================================================================
TEST_CASE("applyDefaults: expected factory values", "[settings]") {
    UserSettings s;
    memset(&s, 0x5A, sizeof(s));  // poison to prove every field is set
    applyDefaults(s);

    CHECK(s.magic == SETTINGS_MAGIC);
    CHECK(s.version == SETTINGS_VERSION);
    CHECK(s.current_limit_ma == AppConfig::CURRENT_LIMIT_DEFAULT_MA);
    CHECK(s.last_pdo_index == -1);
    CHECK(s.lcd_brightness == AppConfig::LCD_BRIGHTNESS_DEFAULT);
    CHECK(s.sounds_enabled == true);
    CHECK(s.auto_pps_enabled == true);
    CHECK(s.auto_avs_enabled == true);
    CHECK(s.auto_dim_minutes == 1);
    CHECK(s.startup_melody == 1);
    CHECK(s.auto_output == false);
    CHECK(s.last_contract_type == static_cast<uint8_t>(SavedStartupContractType::NONE));
    CHECK(s.startup_negotiation == 2);
    CHECK(s.energy_display_mode == 0);
    CHECK(s.current_limit_mode == static_cast<uint8_t>(CurrentLimitMode::OCP));
}

TEST_CASE("applyDefaults then stamp CRC round-trips through loadFromBytes", "[settings]") {
    UserSettings img;
    applyDefaults(img);
    img.crc32 = calculateCrc(img);

    UserSettings out;
    memset(&out, 0, sizeof(out));
    auto status = loadFromBytes(asBytes(img), out);

    REQUIRE(status == LoadStatus::LOADED);
    CHECK(memcmp(&img, &out, sizeof(img)) == 0);
}

// ============================================================================
// Clamps
// ============================================================================
TEST_CASE("clampCurrentLimit: pins to [MIN, MAX]", "[settings]") {
    CHECK(clampCurrentLimit(0) == AppConfig::CURRENT_LIMIT_MIN_MA);
    CHECK(clampCurrentLimit(AppConfig::CURRENT_LIMIT_MIN_MA - 1) == AppConfig::CURRENT_LIMIT_MIN_MA);
    CHECK(clampCurrentLimit(1000) == 1000);
    CHECK(clampCurrentLimit(AppConfig::CURRENT_LIMIT_MAX_MA) == AppConfig::CURRENT_LIMIT_MAX_MA);
    CHECK(clampCurrentLimit(AppConfig::CURRENT_LIMIT_MAX_MA + 5000) == AppConfig::CURRENT_LIMIT_MAX_MA);
}

TEST_CASE("clampBrightness: caps at 100", "[settings]") {
    CHECK(clampBrightness(0) == 0);
    CHECK(clampBrightness(55) == 55);
    CHECK(clampBrightness(100) == 100);
    CHECK(clampBrightness(200) == 100);
}

TEST_CASE("clampAutoDimMinutes: pins to [MIN, MAX]", "[settings]") {
    CHECK(clampAutoDimMinutes(0) == AppConfig::AUTO_DIM_MIN_MINUTES);
    CHECK(clampAutoDimMinutes(5) == 5);
    CHECK(clampAutoDimMinutes(AppConfig::AUTO_DIM_MAX_MINUTES) == AppConfig::AUTO_DIM_MAX_MINUTES);
    CHECK(clampAutoDimMinutes(250) == AppConfig::AUTO_DIM_MAX_MINUTES);
}

TEST_CASE("clampStartupMelody: caps at 3", "[settings]") {
    CHECK(clampStartupMelody(0) == 0);
    CHECK(clampStartupMelody(3) == 3);
    CHECK(clampStartupMelody(4) == 3);
    CHECK(clampStartupMelody(255) == 3);
}

TEST_CASE("clampStartupNegotiation: caps at 2", "[settings]") {
    CHECK(clampStartupNegotiation(0) == 0);
    CHECK(clampStartupNegotiation(2) == 2);
    CHECK(clampStartupNegotiation(3) == 2);
}

TEST_CASE("clampEnergyDisplayMode: caps at 1", "[settings]") {
    CHECK(clampEnergyDisplayMode(0) == 0);
    CHECK(clampEnergyDisplayMode(1) == 1);
    CHECK(clampEnergyDisplayMode(9) == 1);
}
