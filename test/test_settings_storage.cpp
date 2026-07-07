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

TEST_CASE("clampBrightness: pins to [MIN, MAX]", "[settings]") {
    CHECK(clampBrightness(0) == AppConfig::LCD_BRIGHTNESS_MIN);   // floor, not black screen
    CHECK(clampBrightness(AppConfig::LCD_BRIGHTNESS_MIN) == AppConfig::LCD_BRIGHTNESS_MIN);
    CHECK(clampBrightness(55) == 55);
    CHECK(clampBrightness(100) == 100);
    CHECK(clampBrightness(200) == AppConfig::LCD_BRIGHTNESS_MAX);
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

// ============================================================================
// Robustness fuzzing — loadFromBytes over raw/mutated flash images
// ============================================================================
// loadFromBytes consumes raw flash bytes and reinterpret_casts them through
// three POD layouts. These deterministic-PRNG loops feed it random and
// bit-mutated images: under ASan/UBSan they prove no aliasing/OOB/misaligned
// access, and the core safety property is asserted directly —
//   loadStatusOk(status)  =>  the claimed version's CRC actually validates
// so a corrupt image can never be mistaken for good settings.

namespace {

// xorshift32 — deterministic, reproducible (no time/entropy seeding). Seeded
// nonzero per test so CI failures replay identically.
uint32_t xorshift32(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

// Independent re-implementation of loadFromBytes's accept condition: magic must
// match and the CRC over the claimed version's layout must validate. Used as the
// oracle the fuzzers check loadFromBytes against.
bool crcValidFor(const uint8_t* bytes) {
    const UserSettings* cur = reinterpret_cast<const UserSettings*>(bytes);
    if (cur->magic != SETTINGS_MAGIC) return false;
    if (cur->version == SETTINGS_VERSION) {
        UserSettings v;
        memcpy(&v, bytes, sizeof(v));
        return calculateCrc(v) == v.crc32;
    }
    if (cur->version == 5) {
        UserSettingsV5 v;
        memcpy(&v, bytes, sizeof(v));
        return calculateCrc(v) == v.crc32;
    }
    if (cur->version == 4) {
        UserSettingsV4 v;
        memcpy(&v, bytes, sizeof(v));  // v4 is 36 bytes, read from the 44-byte buffer
        return calculateCrc(v) == v.crc32;
    }
    return false;
}

}  // namespace

TEST_CASE("fuzz: random flash images never crash and only load with valid CRC", "[settings][fuzz]") {
    uint32_t rng = 0xC0FFEE01u;
    alignas(UserSettings) uint8_t img[sizeof(UserSettings)];

    int loaded = 0, rejected = 0;
    for (int i = 0; i < 5000; i++) {
        for (size_t b = 0; b < sizeof(img); b += 4) {
            uint32_t r = xorshift32(rng);
            memcpy(img + b, &r, 4);
        }

        UserSettings out;
        memset(&out, 0, sizeof(out));
        auto status = loadFromBytes(img, out);

        // The safety invariant: never accept an image whose CRC doesn't validate.
        if (loadStatusOk(status)) {
            CHECK(crcValidFor(img));
            loaded++;
        } else {
            rejected++;
        }
    }
    // Sanity: random bytes essentially never forge a valid magic+CRC.
    CHECK(rejected == 5000);
    CHECK(loaded == 0);
}

TEST_CASE("fuzz: single-bit-flipped valid image is never accepted", "[settings][fuzz]") {
    uint32_t rng = 0xBADC0DEu;

    for (int i = 0; i < 4000; i++) {
        UserSettings base = validImage();
        alignas(UserSettings) uint8_t img[sizeof(UserSettings)];
        memcpy(img, &base, sizeof(img));

        // Flip exactly one bit anywhere in the image.
        uint32_t bit = xorshift32(rng) % (sizeof(img) * 8);
        img[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));

        UserSettings out;
        memset(&out, 0, sizeof(out));
        auto status = loadFromBytes(img, out);

        // A CRC32 always changes under a single bit flip, so a flipped image must
        // never be accepted, and the oracle must agree.
        CHECK_FALSE(loadStatusOk(status));
        CHECK_FALSE(crcValidFor(img));
    }
}

TEST_CASE("fuzz: magic-valid images load exactly when their CRC validates", "[settings][fuzz]") {
    // Force the magic so the migration/CRC branches are actually exercised, then
    // randomize version + payload and, half the time, stamp the correct CRC.
    uint32_t rng = 0x5EED1234u;
    const uint8_t versions[] = {4, 5, 6, 7};  // 7 is an unknown version (never loads)

    int accepted = 0;
    for (int i = 0; i < 5000; i++) {
        alignas(UserSettings) uint8_t img[sizeof(UserSettings)];
        for (size_t b = 0; b < sizeof(img); b += 4) {
            uint32_t r = xorshift32(rng);
            memcpy(img + b, &r, 4);
        }

        uint32_t magic = SETTINGS_MAGIC;
        memcpy(img + offsetof(UserSettings, magic), &magic, 4);
        uint8_t ver = versions[xorshift32(rng) % 4];
        img[offsetof(UserSettings, version)] = ver;

        // Half the time, make it genuinely valid by stamping the right CRC at the
        // right offset for that version's layout.
        if (xorshift32(rng) & 1) {
            if (ver == 6) {
                UserSettings v; memcpy(&v, img, sizeof(v));
                uint32_t c = calculateCrc(v);
                memcpy(img + offsetof(UserSettings, crc32), &c, 4);
            } else if (ver == 5) {
                UserSettingsV5 v; memcpy(&v, img, sizeof(v));
                uint32_t c = calculateCrc(v);
                memcpy(img + offsetof(UserSettingsV5, crc32), &c, 4);
            } else if (ver == 4) {
                UserSettingsV4 v; memcpy(&v, img, sizeof(v));
                uint32_t c = calculateCrc(v);
                memcpy(img + offsetof(UserSettingsV4, crc32), &c, 4);
            }
        }

        UserSettings out;
        memset(&out, 0, sizeof(out));
        auto status = loadFromBytes(img, out);

        // With a valid magic and known/unknown version, acceptance is exactly
        // "the CRC validates" — both directions.
        CHECK(loadStatusOk(status) == crcValidFor(img));
        if (loadStatusOk(status)) accepted++;
    }
    // The stamped-CRC path must have produced some genuine loads (harness check).
    CHECK(accepted > 0);
}
