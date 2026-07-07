#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdlib>  // abs()
#include "tps26750.h"
#include "config/app_config.h"
#include "utils/pd_voltage.h"
#include "logic/settings_types.h"  // SavedStartupContractType (pure, no Pico SDK)

// ============================================================================
// PD Diagnostics — pure USB-PD domain logic
// ============================================================================
// This header holds the trickiest, purely-computational USB-PD logic that used
// to live in pd_manager.cpp's anonymous namespace: startup-contract matching,
// charger-identity decoding, and cable-rating inference.
//
// Nothing here touches hardware, globals, or the `settings`/`hw` singletons — every
// function takes its inputs explicitly and returns a value. That makes it directly
// includable and exercisable from a host unit-test build (see notes/UNIT_TEST.md,
// P4) without the Pico SDK or any mock HAL. `PdManager` re-exports these into its
// own translation unit with `using namespace PdDiagnostics;`, so its call sites are
// unchanged.
// ============================================================================

/// @brief Inferred cable current/voltage rating, deduced from the source's advertised PDOs.
/// Defined at namespace scope (not inside PdDiagnostics) because it is part of the
/// public `ChargerDiagInfo` surface consumed by the UI.
enum class DetectedCableRating : uint8_t {
    EPR_CAPABLE,           ///< Source exposes an EPR rail (>21 V), implying an EPR-capable cable.
    CAPABLE_5A,            ///< A trustworthy >3 A contract confirms a 5 A cable.
    STANDARD_3A,           ///< Source is capped at 60 W with no trustworthy >3 A path, so a 3 A cable is likely.
    UNKNOWN_CHARGER_LIMIT, ///< Source tops out below 60 W, so the cable rating is not observable.
};

namespace PdDiagnostics {

// USB Vendor IDs of chargers we can name from the manufacturer-info VDO.
constexpr uint16_t USB_VID_FRAMEWORK = 0x32AC;
constexpr uint16_t USB_VID_ANKER     = 0x291A;
constexpr uint16_t USB_VID_APPLE     = 0x05AC;
constexpr uint16_t USB_VID_SAMSUNG   = 0x04E8;
constexpr uint16_t USB_VID_LENOVO    = 0x17EF;

/// @brief Power in whole watts from a mV × mA product (integer, truncated).
inline uint32_t powerWatts(uint32_t voltage_mv, uint32_t current_ma) {
    return (voltage_mv * current_ma) / 1000000UL;
}

/// @brief Copy @p src into @p dest with truncation and guaranteed null-termination.
/// Uses snprintf to sidestep the -Wstringop-truncation false positive that the
/// strncpy + manual terminator idiom trips in optimized builds.
inline void copyStringTruncated(char* dest, size_t dest_len, const char* src) {
    if (!dest || dest_len == 0) {
        return;
    }

    if (!src) {
        dest[0] = '\0';
        return;
    }

    snprintf(dest, dest_len, "%s", src);
}

/// @brief Human-readable brand name for a known USB vendor ID, or "Unknown".
inline const char* getVendorBrandName(uint16_t vid) {
    switch (vid) {
        case USB_VID_FRAMEWORK: return "Framework";
        case USB_VID_ANKER: return "Anker";
        case USB_VID_APPLE: return "Apple";
        case USB_VID_SAMSUNG: return "Samsung";
        case USB_VID_LENOVO: return "Lenovo";
        default: return "Unknown";
    }
}

// ----------------------------------------------------------------------------
// Startup-contract restore matching
// ----------------------------------------------------------------------------

/// @brief Snapshot of the last-negotiated contract persisted to flash, used to
/// re-select the closest PDO on the next boot.
struct SavedStartupContractSnapshot {
    SavedStartupContractType type;
    int8_t pdo_index_hint;
    uint32_t requested_voltage_mv;
    uint32_t range_min_voltage_mv;
    uint32_t range_max_voltage_mv;
};

/// @brief Result of scanning the advertised PDOs for the best startup-restore match.
struct StartupMatchResult {
    bool valid;
    int8_t pdo_index;
    uint32_t requested_voltage_mv;
    uint32_t diff_mv;
};

/// @brief True when a PDO's contract type matches the saved contract type.
/// UNKNOWN (legacy saves) matches anything; NONE matches nothing.
inline bool sourceCapabilityMatchesSavedType(const TPS26750_SourceCapability& pdo,
                                             SavedStartupContractType type) {
    switch (type) {
        case SavedStartupContractType::FIXED:
            return !pdo.is_pps && !pdo.is_avs;
        case SavedStartupContractType::PPS:
            return pdo.is_pps;
        case SavedStartupContractType::AVS:
            return pdo.is_avs;
        case SavedStartupContractType::UNKNOWN:
            return true;
        case SavedStartupContractType::NONE:
            return false;
    }

    return false;
}

/// @brief Find the advertised PDO closest to @p target_voltage_mv for startup restore.
/// For programmable PDOs the candidate voltage is clamped into range and step-aligned
/// before comparison. When @p same_type_only is set, only PDOs matching the saved
/// contract type are considered.
inline StartupMatchResult findBestStartupMatch(const SavedStartupContractSnapshot& snapshot,
                                               const TPS26750_SourceCapability* pdos,
                                               uint8_t count,
                                               uint32_t target_voltage_mv,
                                               bool same_type_only) {
    StartupMatchResult best{false, -1, 0, UINT32_MAX};

    for (uint8_t i = 0; i < count; i++) {
        const TPS26750_SourceCapability& candidate = pdos[i];
        if (same_type_only && !sourceCapabilityMatchesSavedType(candidate, snapshot.type)) {
            continue;
        }

        uint32_t candidate_requested_mv = candidate.voltage_mv;
        uint32_t diff_mv = UINT32_MAX;

        if (candidate.is_pps) {
            candidate_requested_mv = target_voltage_mv;
            if (candidate_requested_mv < candidate.min_voltage_mv) candidate_requested_mv = candidate.min_voltage_mv;
            if (candidate_requested_mv > candidate.voltage_mv) candidate_requested_mv = candidate.voltage_mv;
            candidate_requested_mv = PdVoltage::alignDown(candidate_requested_mv,
                                                          AppConfig::PPS_VOLTAGE_STEP_MV);
            diff_mv = static_cast<uint32_t>(abs(static_cast<int32_t>(candidate_requested_mv) -
                                                static_cast<int32_t>(target_voltage_mv)));
        } else if (candidate.is_avs) {
            candidate_requested_mv = target_voltage_mv;
            if (candidate_requested_mv < candidate.min_voltage_mv) candidate_requested_mv = candidate.min_voltage_mv;
            if (candidate_requested_mv > candidate.voltage_mv) candidate_requested_mv = candidate.voltage_mv;
            candidate_requested_mv = PdVoltage::alignDown(candidate_requested_mv,
                                                          AppConfig::AVS_VOLTAGE_STEP_MV);
            diff_mv = static_cast<uint32_t>(abs(static_cast<int32_t>(candidate_requested_mv) -
                                                static_cast<int32_t>(target_voltage_mv)));
        } else {
            diff_mv = static_cast<uint32_t>(abs(static_cast<int32_t>(candidate.voltage_mv) -
                                                static_cast<int32_t>(target_voltage_mv)));
        }

        if (!best.valid || diff_mv < best.diff_mv) {
            best.valid = true;
            best.pdo_index = static_cast<int8_t>(i);
            best.requested_voltage_mv = candidate_requested_mv;
            best.diff_mv = diff_mv;
        }

        if (best.valid && best.diff_mv == 0) {
            break;
        }
    }

    return best;
}

// ----------------------------------------------------------------------------
// Charger identity decoding
// ----------------------------------------------------------------------------

/// @brief Decode a TPS26750 manufacturer-info VDO response into VID/PID + a cleaned
/// manufacturer name. The name is trimmed, sanitized to printable ASCII, and falls
/// back to the vendor brand name when empty. Returns true when a non-zero VID was read.
inline bool decodeManufacturerInfoResponse(const uint8_t* response_buf,
                                           uint8_t response_len,
                                           uint16_t& vendor_id,
                                           uint16_t& product_id,
                                           char* manufacturer_name,
                                           size_t manufacturer_name_len) {
    vendor_id = 0;
    product_id = 0;

    if (manufacturer_name && manufacturer_name_len > 0) {
        manufacturer_name[0] = '\0';
    }

    if (!response_buf || response_len < 4) {
        return false;
    }

    vendor_id = static_cast<uint16_t>(response_buf[0]) |
                (static_cast<uint16_t>(response_buf[1]) << 8);
    product_id = static_cast<uint16_t>(response_buf[2]) |
                 (static_cast<uint16_t>(response_buf[3]) << 8);

    if (!manufacturer_name || manufacturer_name_len == 0) {
        return vendor_id != 0;
    }

    size_t name_bytes = response_len - 4;
    if (name_bytes >= manufacturer_name_len) {
        name_bytes = manufacturer_name_len - 1;
    }

    memcpy(manufacturer_name, &response_buf[4], name_bytes);
    manufacturer_name[name_bytes] = '\0';

    while (name_bytes > 0 &&
           (manufacturer_name[name_bytes - 1] == ' ' || manufacturer_name[name_bytes - 1] == '\0')) {
        name_bytes--;
        manufacturer_name[name_bytes] = '\0';
    }

    size_t first_non_space = 0;
    while (first_non_space < name_bytes && manufacturer_name[first_non_space] == ' ') {
        first_non_space++;
    }
    if (first_non_space > 0 && first_non_space < name_bytes) {
        memmove(manufacturer_name,
                &manufacturer_name[first_non_space],
                name_bytes - first_non_space + 1);
        name_bytes -= first_non_space;
    } else if (first_non_space >= name_bytes) {
        name_bytes = 0;
        manufacturer_name[0] = '\0';
    }

    for (size_t i = 0; i < name_bytes; i++) {
        char& ch = manufacturer_name[i];
        if (ch < 32 || ch > 126) {
            ch = '.';
        }
    }

    if (name_bytes == 0) {
        const char* fallback_name = getVendorBrandName(vendor_id);
        copyStringTruncated(manufacturer_name, manufacturer_name_len, fallback_name);
    }

    return vendor_id != 0;
}

// ----------------------------------------------------------------------------
// Cable-rating inference
// ----------------------------------------------------------------------------

/// @brief Infer cable capability from advertised source PDOs. Below EPR, fixed rails
/// are the strongest signal: a charger must cap them to 3 A when the cable is not 5 A
/// capable. Programmable PDOs above 3 A are only trusted when the source also proves it
/// can exceed 60 W on its non-programmable rails.
inline DetectedCableRating inferDetectedCableRating(const TPS26750_SourceCapability* pdos, uint8_t count) {
    bool has_fixed_over_3a = false;
    bool has_programmable_over_3a = false;
    uint32_t max_fixed_power_w = 0;

    for (uint8_t i = 0; i < count; i++) {
        const TPS26750_SourceCapability& pdo = pdos[i];
        if (pdo.voltage_mv > 21000) {
            return DetectedCableRating::EPR_CAPABLE;
        }

        if (!pdo.is_pps && !pdo.is_avs) {
            uint32_t power_w = powerWatts(pdo.voltage_mv, pdo.max_current_ma);
            if (power_w > max_fixed_power_w) {
                max_fixed_power_w = power_w;
            }

            if (pdo.max_current_ma > 3000) {
                has_fixed_over_3a = true;
            }
            continue;
        }

        if (pdo.max_current_ma > 3000 || pdo.max_current_9_15_ma > 3000) {
            has_programmable_over_3a = true;
        }
    }

    if (has_fixed_over_3a) {
        return DetectedCableRating::CAPABLE_5A;
    }

    if (has_programmable_over_3a && max_fixed_power_w > 60) {
        return DetectedCableRating::CAPABLE_5A;
    }

    if (max_fixed_power_w < 60) {
        return DetectedCableRating::UNKNOWN_CHARGER_LIMIT;
    }

    return DetectedCableRating::STANDARD_3A;
}

// ----------------------------------------------------------------------------
// PD-revision inference
// ----------------------------------------------------------------------------

/// @brief Infer the USB-PD revision badge string from the shape of the advertised
/// PDOs. Mirrors the badge rules in the project docs:
///   - SPR AVS present (an AVS PDO with a 9 V floor, new in PD 3.2)        -> "PD3.2"
///   - any EPR rail: >7 PDOs, or an EPR AVS PDO (AVS floor above 9 V)      -> "PD3.1"
///   - PPS present (no AVS)                                                -> "PD3.0"
///   - at least one fixed PDO and none of the above                       -> "PD2.0"
///   - no PDOs at all                                                     -> ""
/// Pure: reads only the PDO array, so it is host-testable. The caller owns the
/// side effects (caching the string, logging the change).
inline const char* inferPdRevision(const TPS26750_SourceCapability* pdos, uint8_t count) {
    bool has_epr = (count > 7);
    bool has_pps = false;
    bool has_epr_avs = false;
    bool has_spr_avs = false;

    for (uint8_t i = 0; i < count; i++) {
        if (pdos[i].is_avs) {
            if (pdos[i].min_voltage_mv == 9000) {
                has_spr_avs = true;
            } else {
                has_epr_avs = true;
                has_epr = true;
            }
        }
        if (pdos[i].is_pps) has_pps = true;
    }

    // SPR AVS is new in PD 3.2, so its presence is enough to identify PD 3.2.
    if (has_spr_avs) {
        return "PD3.2";
    } else if (has_epr || has_epr_avs) {
        // EPR-only sources map to PD 3.1.
        return "PD3.1";
    } else if (has_pps) {
        // PPS was introduced in PD 3.0.
        return "PD3.0";
    } else if (count > 0) {
        // Neither PPS nor AVS present and <= 7 PDOs: assume PD 2.0.
        return "PD2.0";
    }

    return "";
}

// ----------------------------------------------------------------------------
// EPR safe-exit decisions
// ----------------------------------------------------------------------------
// The firmware intercepts direct EPR->SPR transitions and replaces them with a
// stepped sequence (AVS step-down within EPR -> 5 V -> target), because a direct
// request reboots some chargers. These three helpers are the pure decision layer:
// "does this transition need the dance", "can we do it safely", and "which APDO do
// we step down through". The FSM that drives the hardware requests stays in
// PdManager; only the arithmetic lives here so it is host-testable.

/// @brief True when leaving the active contract for @p target_voltage_mv requires the
/// EPR safe-exit sequence: currently above the SPR/EPR boundary (>20 V) and the target
/// is within SPR range (any PPS, or a fixed/AVS target <= 20 V).
inline bool needsEprExit(uint32_t active_voltage_mv, uint32_t target_voltage_mv, bool target_is_pps) {
    if (active_voltage_mv <= AppConfig::EPR_SPR_MAX_MV) {
        return false;  // Already in SPR range
    }
    if (target_is_pps) {
        return true;   // PPS is always SPR (max 21 V)
    }
    return target_voltage_mv <= AppConfig::EPR_SPR_MAX_MV;
}

/// @brief True when the source advertises an EPR AVS APDO able to step VBUS from EPR
/// into SPR range without first leaving EPR mode (min <= 20 V < max).
inline bool isSafeEprExitPossible(const TPS26750_SourceCapability* pdos, uint8_t count) {
    for (uint8_t i = 0; i < count; i++) {
        if (pdos[i].is_avs &&
            pdos[i].voltage_mv > AppConfig::EPR_SPR_MAX_MV &&
            pdos[i].min_voltage_mv <= AppConfig::EPR_SPR_MAX_MV) {
            return true;
        }
    }
    return false;
}

/// @brief Result of choosing the intermediate AVS step-down for the EPR safe exit.
struct AvsSafeVoltageResult {
    bool valid;
    uint32_t voltage_mv;   ///< Step-aligned request voltage within SPR range
    uint32_t current_ma;   ///< Chosen APDO's max current
    int8_t pdo_index;      ///< PDO-cache index of the chosen APDO (-1 when none)
};

/// @brief Pick the intermediate EPR AVS step-down for the safe exit. Selects the EPR
/// AVS APDO with the lowest SPR-reachable floor, then requests that floor rounded up
/// to the AVS voltage step. Returns {valid=false} when no such APDO exists.
inline AvsSafeVoltageResult findAvsSafeVoltage(const TPS26750_SourceCapability* pdos, uint8_t count) {
    uint32_t best_min_mv = UINT32_MAX;
    int8_t best_index = -1;

    for (uint8_t i = 0; i < count; i++) {
        if (pdos[i].is_avs &&
            pdos[i].voltage_mv > AppConfig::EPR_SPR_MAX_MV &&
            pdos[i].min_voltage_mv <= AppConfig::EPR_SPR_MAX_MV &&
            pdos[i].min_voltage_mv < best_min_mv) {
            best_min_mv = pdos[i].min_voltage_mv;
            best_index = static_cast<int8_t>(i);
        }
    }

    if (best_index < 0) {
        return {false, 0, 0, -1};
    }

    uint32_t aligned = ((best_min_mv + AppConfig::AVS_VOLTAGE_STEP_MV - 1) /
                        AppConfig::AVS_VOLTAGE_STEP_MV) * AppConfig::AVS_VOLTAGE_STEP_MV;
    return {true, aligned, pdos[best_index].max_current_ma, best_index};
}

// ============================================================================
// EPR safe-exit sequencing FSM (pure transition table)
// ============================================================================
// The 3-step exit PdManager::update() runs to leave EPR without a voltage rise:
//   STEPPING_DOWN -> REQUESTING_5V -> REQUESTING_TARGET -> NONE
// The transitions (which step advances, and when the whole thing aborts) are
// pure; only the actions they trigger touch hardware. Extracting the table here
// makes the sequencing — historically a bug-nest — host-testable, while
// PdManager keeps the hardware side (issuing PD requests, refreshing the active
// contract, logging) by dispatching on the returned action.

/// @brief The EPR exit stage. PdManager aliases its `EprExitState` member type to
/// this (identical enumerators) so call sites are unchanged.
enum class EprExitStage : uint8_t { NONE, STEPPING_DOWN, REQUESTING_5V, REQUESTING_TARGET };

/// @brief The side effect PdManager must perform after a transition.
enum class EprExitAction : uint8_t {
    NONE,            ///< Still in flight (negotiation pending): do nothing.
    ABORT_TIMEOUT,   ///< Whole sequence exceeded its timeout: abort.
    ABORT_FAILED,    ///< A step failed/timed out at the negotiation level: abort.
    REQUEST_5V,      ///< Step 1 (AVS step-down) done: request 5V Fixed to exit EPR.
    REQUEST_TARGET,  ///< Step 2 (5V) done: fire the user's deferred target request.
    DONE,            ///< Step 3 (target) done: sequence complete.
};

struct EprExitStep {
    EprExitStage next;     ///< The stage to store back.
    EprExitAction action;  ///< What PdManager should do this step.
};

/// @brief One transition of the EPR safe-exit FSM. Precedence mirrors
/// PdManager::update(): a whole-sequence timeout aborts first, then a
/// negotiation-level failure, then a successful step advances the sequence;
/// otherwise the step is still in flight and nothing changes.
///
/// @param stage        Current exit stage.
/// @param neg_success  The current negotiation reported SUCCESS.
/// @param neg_failed   The current negotiation reported FAILED or TIMEOUT.
/// @param timed_out    The overall EPR-exit deadline elapsed.
inline EprExitStep eprExitStep(EprExitStage stage, bool neg_success,
                               bool neg_failed, bool timed_out) {
    if (stage == EprExitStage::NONE) {
        return {EprExitStage::NONE, EprExitAction::NONE};
    }
    if (timed_out) {
        return {EprExitStage::NONE, EprExitAction::ABORT_TIMEOUT};
    }
    if (neg_failed) {
        return {EprExitStage::NONE, EprExitAction::ABORT_FAILED};
    }
    if (neg_success) {
        switch (stage) {
            case EprExitStage::STEPPING_DOWN:
                return {EprExitStage::REQUESTING_5V, EprExitAction::REQUEST_5V};
            case EprExitStage::REQUESTING_5V:
                return {EprExitStage::REQUESTING_TARGET, EprExitAction::REQUEST_TARGET};
            case EprExitStage::REQUESTING_TARGET:
                return {EprExitStage::NONE, EprExitAction::DONE};
            case EprExitStage::NONE:
                break;  // unreachable (handled above)
        }
    }
    // Negotiation still in progress for this step: hold.
    return {stage, EprExitAction::NONE};
}

}  // namespace PdDiagnostics
