// Host unit tests for the pure USB-PD domain logic in src/logic/pd_diagnostics.h.
// These exercise the real production functions (no re-implementation) via Snitch.

#include <snitch/snitch.hpp>

#include <string>

#include "logic/pd_diagnostics.h"

using namespace PdDiagnostics;

namespace {

// Convenience builders for the PDO struct (fields, in order):
//   { voltage_mv, max_current_ma, is_pps, is_avs, min_voltage_mv, max_current_9_15_ma }
TPS26750_SourceCapability fixedPdo(uint32_t v_mv, uint32_t i_ma) {
    return TPS26750_SourceCapability{v_mv, i_ma, false, false, 0, 0};
}
TPS26750_SourceCapability ppsPdo(uint32_t max_v_mv, uint32_t min_v_mv, uint32_t i_ma) {
    return TPS26750_SourceCapability{max_v_mv, i_ma, true, false, min_v_mv, 0};
}
TPS26750_SourceCapability avsPdo(uint32_t max_v_mv, uint32_t min_v_mv, uint32_t i_ma) {
    return TPS26750_SourceCapability{max_v_mv, i_ma, false, true, min_v_mv, 0};
}

}  // namespace

// ============================================================================
// powerWatts
// ============================================================================
TEST_CASE("powerWatts: integer watts, truncated", "[pd_diag]") {
    CHECK(powerWatts(20000, 3000) == 60);   // 60.0 W
    CHECK(powerWatts(21000, 3000) == 63);   // 63.0 W
    CHECK(powerWatts(15000, 3000) == 45);   // 45.0 W
    CHECK(powerWatts(5000, 900) == 4);      // 4.5 W -> truncates to 4
    CHECK(powerWatts(0, 5000) == 0);
}

// ============================================================================
// getVendorBrandName
// ============================================================================
TEST_CASE("getVendorBrandName: known VIDs and fallback", "[pd_diag]") {
    CHECK(std::string(getVendorBrandName(USB_VID_FRAMEWORK)) == "Framework");
    CHECK(std::string(getVendorBrandName(USB_VID_ANKER)) == "Anker");
    CHECK(std::string(getVendorBrandName(USB_VID_APPLE)) == "Apple");
    CHECK(std::string(getVendorBrandName(USB_VID_SAMSUNG)) == "Samsung");
    CHECK(std::string(getVendorBrandName(USB_VID_LENOVO)) == "Lenovo");
    CHECK(std::string(getVendorBrandName(0x0000)) == "Unknown");
    CHECK(std::string(getVendorBrandName(0xFFFF)) == "Unknown");
}

// ============================================================================
// copyStringTruncated
// ============================================================================
TEST_CASE("copyStringTruncated: truncates and null-terminates", "[pd_diag]") {
    char buf[4];

    SECTION("fits") {
        copyStringTruncated(buf, sizeof(buf), "ab");
        CHECK(std::string(buf) == "ab");
    }
    SECTION("truncates to dest_len-1") {
        copyStringTruncated(buf, sizeof(buf), "abcdef");
        CHECK(std::string(buf) == "abc");  // 3 chars + NUL
    }
    SECTION("null src clears") {
        buf[0] = 'x';
        copyStringTruncated(buf, sizeof(buf), nullptr);
        CHECK(buf[0] == '\0');
    }
    SECTION("zero-length dest is a no-op (no write)") {
        char guard = 'Z';
        copyStringTruncated(&guard, 0, "abc");
        CHECK(guard == 'Z');
    }
}

// ============================================================================
// sourceCapabilityMatchesSavedType
// ============================================================================
TEST_CASE("sourceCapabilityMatchesSavedType: type gating", "[pd_diag]") {
    auto fixed = fixedPdo(9000, 3000);
    auto pps   = ppsPdo(21000, 3300, 3000);
    auto avs   = avsPdo(20000, 9000, 5000);

    CHECK(sourceCapabilityMatchesSavedType(fixed, SavedStartupContractType::FIXED));
    CHECK_FALSE(sourceCapabilityMatchesSavedType(fixed, SavedStartupContractType::PPS));

    CHECK(sourceCapabilityMatchesSavedType(pps, SavedStartupContractType::PPS));
    CHECK(sourceCapabilityMatchesSavedType(avs, SavedStartupContractType::AVS));

    // UNKNOWN (legacy saves) matches anything; NONE matches nothing.
    CHECK(sourceCapabilityMatchesSavedType(fixed, SavedStartupContractType::UNKNOWN));
    CHECK(sourceCapabilityMatchesSavedType(pps, SavedStartupContractType::UNKNOWN));
    CHECK_FALSE(sourceCapabilityMatchesSavedType(fixed, SavedStartupContractType::NONE));
    CHECK_FALSE(sourceCapabilityMatchesSavedType(avs, SavedStartupContractType::NONE));
}

// ============================================================================
// findBestStartupMatch
// ============================================================================
TEST_CASE("findBestStartupMatch: exact fixed match wins with diff 0", "[pd_diag]") {
    TPS26750_SourceCapability pdos[] = {
        fixedPdo(5000, 3000),
        fixedPdo(9000, 3000),
        fixedPdo(15000, 3000),
    };
    SavedStartupContractSnapshot snap{SavedStartupContractType::FIXED, -1, 9000, 9000, 9000};

    auto r = findBestStartupMatch(snap, pdos, 3, 9000, /*same_type_only=*/true);
    REQUIRE(r.valid);
    CHECK(r.pdo_index == 1);
    CHECK(r.requested_voltage_mv == 9000);
    CHECK(r.diff_mv == 0);
}

TEST_CASE("findBestStartupMatch: PPS candidate is clamped and 20mV step-aligned", "[pd_diag]") {
    TPS26750_SourceCapability pdos[] = {ppsPdo(21000, 3300, 3000)};
    SavedStartupContractSnapshot snap{SavedStartupContractType::PPS, -1, 9050, 3300, 21000};

    auto r = findBestStartupMatch(snap, pdos, 1, 9050, true);
    REQUIRE(r.valid);
    CHECK(r.pdo_index == 0);
    CHECK(r.requested_voltage_mv == 9040);  // alignDown(9050, 20)
    CHECK(r.diff_mv == 10);
}

TEST_CASE("findBestStartupMatch: PPS target below range clamps up to min", "[pd_diag]") {
    TPS26750_SourceCapability pdos[] = {ppsPdo(21000, 5000, 3000)};
    SavedStartupContractSnapshot snap{SavedStartupContractType::PPS, -1, 3300, 5000, 21000};

    auto r = findBestStartupMatch(snap, pdos, 1, 3300, true);
    REQUIRE(r.valid);
    CHECK(r.requested_voltage_mv == 5000);          // clamped to min, already 20mV-aligned
    CHECK(r.diff_mv == (5000 - 3300));
}

TEST_CASE("findBestStartupMatch: AVS candidate step-aligned to 100mV", "[pd_diag]") {
    TPS26750_SourceCapability pdos[] = {avsPdo(20000, 9000, 5000)};
    SavedStartupContractSnapshot snap{SavedStartupContractType::AVS, -1, 15050, 9000, 20000};

    auto r = findBestStartupMatch(snap, pdos, 1, 15050, true);
    REQUIRE(r.valid);
    CHECK(r.requested_voltage_mv == 15000);  // alignDown(15050, 100)
    CHECK(r.diff_mv == 50);
}

TEST_CASE("findBestStartupMatch: same_type_only skips mismatched PDOs", "[pd_diag]") {
    TPS26750_SourceCapability pdos[] = {
        ppsPdo(21000, 3300, 3000),   // index 0, wrong type
        fixedPdo(9000, 3000),        // index 1, the only FIXED
    };
    SavedStartupContractSnapshot snap{SavedStartupContractType::FIXED, -1, 9000, 9000, 9000};

    auto r = findBestStartupMatch(snap, pdos, 2, 9000, /*same_type_only=*/true);
    REQUIRE(r.valid);
    CHECK(r.pdo_index == 1);

    // With same_type_only=false the PPS at index 0 is considered too; it can be
    // clamped to exactly 9000 (diff 0) and, being first, wins the tie.
    auto r2 = findBestStartupMatch(snap, pdos, 2, 9000, /*same_type_only=*/false);
    REQUIRE(r2.valid);
    CHECK(r2.pdo_index == 0);
    CHECK(r2.diff_mv == 0);
}

TEST_CASE("findBestStartupMatch: no candidates -> invalid", "[pd_diag]") {
    TPS26750_SourceCapability pdos[] = {ppsPdo(21000, 3300, 3000)};
    SavedStartupContractSnapshot snap{SavedStartupContractType::FIXED, -1, 9000, 9000, 9000};

    auto r = findBestStartupMatch(snap, pdos, 1, 9000, /*same_type_only=*/true);
    CHECK_FALSE(r.valid);
    CHECK(r.pdo_index == -1);
}

// ============================================================================
// decodeManufacturerInfoResponse
// ============================================================================
TEST_CASE("decodeManufacturerInfoResponse: decodes VID/PID little-endian", "[pd_diag]") {
    // Framework VID 0x32AC, PID 0x0010
    const uint8_t resp[] = {0xAC, 0x32, 0x10, 0x00};
    uint16_t vid = 0, pid = 0;
    char name[32];

    bool ok = decodeManufacturerInfoResponse(resp, sizeof(resp), vid, pid, name, sizeof(name));
    CHECK(ok);
    CHECK(vid == 0x32AC);
    CHECK(pid == 0x0010);
    // Empty name payload -> falls back to brand name for the VID.
    CHECK(std::string(name) == "Framework");
}

TEST_CASE("decodeManufacturerInfoResponse: trims leading/trailing spaces", "[pd_diag]") {
    const uint8_t resp[] = {0x00, 0x00, 0x00, 0x00, ' ', ' ', 'H', 'i', ' ', ' '};
    uint16_t vid = 1, pid = 1;
    char name[32];

    bool ok = decodeManufacturerInfoResponse(resp, sizeof(resp), vid, pid, name, sizeof(name));
    CHECK_FALSE(ok);  // VID is zero
    CHECK(std::string(name) == "Hi");
}

TEST_CASE("decodeManufacturerInfoResponse: sanitizes non-printable bytes", "[pd_diag]") {
    const uint8_t resp[] = {0x10, 0x00, 0x00, 0x00, 'A', 0x01, 'B', 0x7F};
    uint16_t vid = 0, pid = 0;
    char name[32];

    decodeManufacturerInfoResponse(resp, sizeof(resp), vid, pid, name, sizeof(name));
    CHECK(std::string(name) == "A.B.");  // 0x01 and 0x7F -> '.'
}

TEST_CASE("decodeManufacturerInfoResponse: short response is rejected", "[pd_diag]") {
    const uint8_t resp[] = {0x10, 0x00};
    uint16_t vid = 9, pid = 9;
    char name[8] = "seed";

    bool ok = decodeManufacturerInfoResponse(resp, sizeof(resp), vid, pid, name, sizeof(name));
    CHECK_FALSE(ok);
    CHECK(vid == 0);
    CHECK(pid == 0);
    CHECK(name[0] == '\0');
}

TEST_CASE("decodeManufacturerInfoResponse: name truncated to buffer", "[pd_diag]") {
    const uint8_t resp[] = {0x10, 0x00, 0x00, 0x00, 'A', 'B', 'C', 'D', 'E'};
    uint16_t vid = 0, pid = 0;
    char name[4];  // room for 3 chars + NUL

    decodeManufacturerInfoResponse(resp, sizeof(resp), vid, pid, name, sizeof(name));
    CHECK(std::string(name) == "ABC");
}

// ============================================================================
// inferDetectedCableRating
// ============================================================================
TEST_CASE("inferDetectedCableRating: EPR rail implies EPR-capable cable", "[pd_diag]") {
    TPS26750_SourceCapability pdos[] = {
        fixedPdo(5000, 3000),
        fixedPdo(28000, 5000),  // >21V
    };
    CHECK(inferDetectedCableRating(pdos, 2) == DetectedCableRating::EPR_CAPABLE);
}

TEST_CASE("inferDetectedCableRating: fixed rail over 3A implies 5A cable", "[pd_diag]") {
    TPS26750_SourceCapability pdos[] = {
        fixedPdo(5000, 3000),
        fixedPdo(20000, 5000),  // >3A fixed
    };
    CHECK(inferDetectedCableRating(pdos, 2) == DetectedCableRating::CAPABLE_5A);
}

TEST_CASE("inferDetectedCableRating: programmable >3A trusted only above 60W", "[pd_diag]") {
    // Fixed rails stay <=3A but prove >60W (21V*3A = 63W); PPS advertises >3A.
    TPS26750_SourceCapability pdos[] = {
        fixedPdo(21000, 3000),        // 63W, exactly 3A
        ppsPdo(21000, 3300, 5000),    // programmable >3A
    };
    CHECK(inferDetectedCableRating(pdos, 2) == DetectedCableRating::CAPABLE_5A);
}

TEST_CASE("inferDetectedCableRating: 60W cap, no >3A path -> standard 3A", "[pd_diag]") {
    TPS26750_SourceCapability pdos[] = {
        fixedPdo(5000, 3000),
        fixedPdo(20000, 3000),  // exactly 60W, 3A
    };
    CHECK(inferDetectedCableRating(pdos, 2) == DetectedCableRating::STANDARD_3A);
}

TEST_CASE("inferDetectedCableRating: below 60W -> rating unobservable", "[pd_diag]") {
    TPS26750_SourceCapability pdos[] = {
        fixedPdo(5000, 3000),
        fixedPdo(15000, 3000),  // 45W
    };
    CHECK(inferDetectedCableRating(pdos, 2) == DetectedCableRating::UNKNOWN_CHARGER_LIMIT);
}
