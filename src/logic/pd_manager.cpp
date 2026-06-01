#include "pd_manager.h"
#include "hardware.h"
#include "config/board_config.h"
#include "interrupts.h"
#include "logic/cc_controller.h"
#include "logic/settings.h"
#include "utils/logging.h"
#include "utils/pd_voltage.h"
#include <array>
#include <cstring>
#include <cstdlib>  // abs()

namespace {

constexpr uint8_t GPPI_RESPONSE_READ_BYTES = 32;
constexpr uint8_t TPS_INTERRUPT_REGISTER_BYTES = 11;

constexpr uint16_t USB_VID_FRAMEWORK = 0x32AC;
constexpr uint16_t USB_VID_ANKER     = 0x291A;
constexpr uint16_t USB_VID_APPLE     = 0x05AC;
constexpr uint16_t USB_VID_SAMSUNG   = 0x04E8;
constexpr uint16_t USB_VID_LENOVO    = 0x17EF;

void copyStringTruncated(char* dest, size_t dest_len, const char* src) {
    if (!dest || dest_len == 0) {
        return;
    }

    if (!src) {
        dest[0] = '\0';
        return;
    }

    strncpy(dest, src, dest_len - 1);
    dest[dest_len - 1] = '\0';
}

std::array<uint8_t, TPS_INTERRUPT_REGISTER_BYTES> makeInterruptClearMask(uint8_t bit_index) {
    std::array<uint8_t, TPS_INTERRUPT_REGISTER_BYTES> clear_mask{};
    if (bit_index < (TPS_INTERRUPT_REGISTER_BYTES * 8)) {
        clear_mask[bit_index / 8] = static_cast<uint8_t>(1u << (bit_index % 8));
    }
    return clear_mask;
}

uint32_t powerWatts(uint32_t voltage_mv, uint32_t current_ma) {
    return (voltage_mv * current_ma) / 1000000UL;
}

struct SavedStartupContractSnapshot {
    SavedStartupContractType type;
    int8_t pdo_index_hint;
    uint32_t requested_voltage_mv;
    uint32_t range_min_voltage_mv;
    uint32_t range_max_voltage_mv;
};

struct StartupMatchResult {
    bool valid;
    int8_t pdo_index;
    uint32_t requested_voltage_mv;
    uint32_t diff_mv;
};

SavedStartupContractSnapshot getSavedStartupContractSnapshot() {
    return {
        settings.getLastContractType(),
        settings.getLastPdoIndex(),
        settings.getLastRequestedVoltageMv(),
        settings.getLastContractMinVoltageMv(),
        settings.getLastContractMaxVoltageMv(),
    };
}

const char* savedStartupContractTypeName(SavedStartupContractType type) {
    switch (type) {
        case SavedStartupContractType::NONE: return "NONE";
        case SavedStartupContractType::UNKNOWN: return "LEGACY";
        case SavedStartupContractType::FIXED: return "FIXED";
        case SavedStartupContractType::PPS: return "PPS";
        case SavedStartupContractType::AVS: return "AVS";
    }

    return "UNKNOWN";
}

const char* sourceCapabilityTypeName(const SourceCapability& pdo) {
    if (pdo.is_avs) return "AVS";
    if (pdo.is_pps) return "PPS";
    return "FIXED";
}

bool sourceCapabilityMatchesSavedType(const SourceCapability& pdo,
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

bool isProgrammableSavedType(SavedStartupContractType type) {
    return type == SavedStartupContractType::PPS || type == SavedStartupContractType::AVS;
}

uint32_t getSavedTargetVoltageMv(const SavedStartupContractSnapshot& snapshot,
                                 const SourceCapability* pdos,
                                 uint8_t count) {
    if (snapshot.requested_voltage_mv > 0) {
        return snapshot.requested_voltage_mv;
    }
    if (snapshot.range_max_voltage_mv > 0) {
        return snapshot.range_max_voltage_mv;
    }
    if (snapshot.pdo_index_hint >= 0 && snapshot.pdo_index_hint < count) {
        return pdos[snapshot.pdo_index_hint].voltage_mv;
    }
    return 0;
}

bool snapshotPrefersEprRetry(const SavedStartupContractSnapshot& snapshot) {
    return snapshot.requested_voltage_mv > AppConfig::EPR_SPR_MAX_MV ||
           snapshot.range_max_voltage_mv > AppConfig::EPR_SPR_MAX_MV;
}

bool hasVisibleEprPdos(const SourceCapability* pdos, uint8_t count) {
    for (uint8_t i = 0; i < count; i++) {
        if (pdos[i].voltage_mv > AppConfig::EPR_SPR_MAX_MV) {
            return true;
        }
    }

    return false;
}

bool matchesFixedPdo(const SourceCapability* pdos,
                     uint8_t count,
                     uint32_t voltage_mv,
                     uint32_t tolerance_mv) {
    for (uint8_t i = 0; i < count; i++) {
        const SourceCapability& pdo = pdos[i];
        if (pdo.is_pps || pdo.is_avs) {
            continue;
        }

        uint32_t diff_mv = (pdo.voltage_mv > voltage_mv)
            ? (pdo.voltage_mv - voltage_mv)
            : (voltage_mv - pdo.voltage_mv);
        if (diff_mv <= tolerance_mv) {
            return true;
        }
    }

    return false;
}

void describeSavedStartupContract(const SavedStartupContractSnapshot& snapshot,
                                  char* buffer,
                                  size_t buffer_size) {
    if (snapshot.type == SavedStartupContractType::NONE) {
        snprintf(buffer, buffer_size, "none");
        return;
    }

    if (snapshot.range_min_voltage_mv > 0 || snapshot.range_max_voltage_mv > 0) {
        snprintf(buffer, buffer_size, "%s target=%umV range=%u-%umV hint=PDO[%d]",
                 savedStartupContractTypeName(snapshot.type),
                 snapshot.requested_voltage_mv,
                 snapshot.range_min_voltage_mv,
                 snapshot.range_max_voltage_mv,
                 snapshot.pdo_index_hint);
        return;
    }

    snprintf(buffer, buffer_size, "%s target=%umV hint=PDO[%d]",
             savedStartupContractTypeName(snapshot.type),
             snapshot.requested_voltage_mv,
             snapshot.pdo_index_hint);
}

void describeSourceCapability(const SourceCapability& pdo,
                              char* buffer,
                              size_t buffer_size) {
    if (pdo.is_pps || pdo.is_avs) {
        snprintf(buffer, buffer_size, "%s %u-%umV @ %umA",
                 sourceCapabilityTypeName(pdo),
                 pdo.min_voltage_mv,
                 pdo.voltage_mv,
                 pdo.max_current_ma);
        return;
    }

    snprintf(buffer, buffer_size, "%s %umV @ %umA",
             sourceCapabilityTypeName(pdo),
             pdo.voltage_mv,
             pdo.max_current_ma);
}

StartupMatchResult findBestStartupMatch(const SavedStartupContractSnapshot& snapshot,
                                        const SourceCapability* pdos,
                                        uint8_t count,
                                        uint32_t target_voltage_mv,
                                        bool same_type_only) {
    StartupMatchResult best{false, -1, 0, UINT32_MAX};

    for (uint8_t i = 0; i < count; i++) {
        const SourceCapability& candidate = pdos[i];
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

const char* getVendorBrandName(uint16_t vid) {
    switch (vid) {
        case USB_VID_FRAMEWORK: return "Framework";
        case USB_VID_ANKER: return "Anker";
        case USB_VID_APPLE: return "Apple";
        case USB_VID_SAMSUNG: return "Samsung";
        case USB_VID_LENOVO: return "Lenovo";
        default: return "Unknown";
    }
}

bool decodeManufacturerInfoResponse(const uint8_t* response_buf,
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

uint32_t getSourceCapabilityMaxPowerW(const SourceCapability& pdo) {
    if (pdo.is_avs && pdo.max_current_9_15_ma > 0) {
        uint32_t low_band_power_w = powerWatts(15000, pdo.max_current_9_15_ma);
        uint32_t high_band_power_w = powerWatts(pdo.voltage_mv, pdo.max_current_ma);
        return (high_band_power_w > low_band_power_w) ? high_band_power_w : low_band_power_w;
    }

    return powerWatts(pdo.voltage_mv, pdo.max_current_ma);
}

// Infer cable capability from advertised source PDOs. Below EPR, fixed rails are the
// strongest signal: a charger must cap them to 3 A when the cable is not 5 A capable.
// Programmable PDOs above 3 A are only trusted when the source also proves it can
// exceed 60 W on its non-programmable rails.
DetectedCableRating inferDetectedCableRating(const SourceCapability* pdos, uint8_t count) {
    bool has_fixed_over_3a = false;
    bool has_programmable_over_3a = false;
    uint32_t max_fixed_power_w = 0;

    for (uint8_t i = 0; i < count; i++) {
        const SourceCapability& pdo = pdos[i];
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

}  // namespace

// Global instance
PdManager pdManager;

// ============================================================================
// Constructor
// ============================================================================

PdManager::PdManager()
    : _negotiation_state(NegotiationState::IDLE)
    , _negotiation_start(nil_time)
    , _charger_connected(false)
    , _pdo_count(0)
    , _pdos_valid(false)
    , _pps_active(false)
    , _pps_voltage_mv(0)
    , _pps_current_ma(0)
    , _pps_last_refresh(nil_time)
    , _pps_user_target_mv(0)
    , _pps_correction_mv(0)
    , _pps_tuning_converged(false)
    , _pps_range_min_mv(0)
    , _pps_range_max_mv(0)
    , _avs_active(false)
    , _avs_voltage_mv(0)
    , _avs_current_ma(0)
    , _avs_last_refresh(nil_time)
    , _avs_user_target_mv(0)
    , _avs_correction_mv(0)
    , _avs_tuning_converged(false)
    , _avs_range_min_mv(0)
    , _avs_range_max_mv(0)
    , _pre_request_voltage_mv(0)
    , _pre_request_current_ma(0)
    , _requested_contract_type(RequestedContractType::NONE)
    , _requested_voltage_mv(0)
    , _requested_current_ma(0)
    , _epr_exit_state(EprExitState::NONE)
    , _epr_deferred_voltage_mv(0)
    , _epr_deferred_current_ma(0)
    , _epr_deferred_contract_type(RequestedContractType::NONE)
    , _epr_deferred_pdo_index(-1)
    , _epr_exit_start(nil_time)
    , _active_pdo_index(-1)
    , _startup_restore_waiting_for_epr(false)
{
    _active_contract.voltage_mv = 0;
    _active_contract.current_ma = 0;
    _active_contract.is_pps = false;
    _active_contract.is_avs = false;
    _active_contract.is_epr = false;
    _active_contract.valid = false;
    _active_contract.programmable_min_mv = 0;
    _active_contract.programmable_max_mv = 0;
    _pd_revision[0] = '\0';
    clearChargerIdentity();
}

// ============================================================================
// Initialization
// ============================================================================

void PdManager::init() {
    // Check TPS26750 mode
    char mode[5];
    if (getMode(mode)) {
        LOG_DEBUG("TPS26750 mode: %s", mode);

        // Check if in APP mode (normal operation)
        if (mode[0] == 'A' && mode[1] == 'P' && mode[2] == 'P') {
            _charger_connected = true;
        }
    } else {
        LOG_WARN("Failed to read TPS26750 mode");
    }

    // Note: PDO discovery is handled adaptively during boot via waitForPdos()
    // This avoids false warnings when TPS26750 hasn't finished negotiating yet.
    // Initial source capabilities will be read during the boot sequence.

    LOG_INFO("PD Manager initialized");
}

// ============================================================================
// Main Update
// ============================================================================

void PdManager::update() {
    // Handle pending PD interrupts
    if (Interrupts::handlePdInterrupt()) {
        handlePdInterrupt();
    }

    // Check negotiation timeout and polling fallback
    if (_negotiation_state == NegotiationState::REQUESTING) {
        uint32_t elapsed_ms = absolute_time_diff_us(_negotiation_start, get_absolute_time()) / 1000;

        // Polling fallback: if no interrupt after 500ms, poll the active contract
        // Some PD2.0 sources don't fire NEW_CONTRACT_AS_SINK interrupt reliably
        if (elapsed_ms >= POLLING_FALLBACK_MS) {
            uint32_t current_voltage_mv, current_current_ma;
            if (hw.pdController.getActiveContract(current_voltage_mv, current_current_ma)) {
                bool contract_changed_from_pre_request =
                    (current_voltage_mv != _pre_request_voltage_mv ||
                     current_current_ma != _pre_request_current_ma);
                bool contract_changed_from_cached =
                    (!_active_contract.valid ||
                     current_voltage_mv != _active_contract.voltage_mv ||
                     current_current_ma != _active_contract.current_ma);

                // Log each distinct intermediate contract once while waiting for the
                // requested target. Repeated polls of the same settled intermediate
                // state (for example 5V while waiting for EPR discovery) are noise.
                if (contract_changed_from_pre_request && contract_changed_from_cached) {
                    LOG_INFO("Contract change detected via polling: %umV @ %umA",
                             current_voltage_mv, current_current_ma);
                }

                refreshActiveContract();

                if (isRequestedContractReached()) {
                    _negotiation_state = NegotiationState::SUCCESS;
                } else if (contract_changed_from_pre_request && contract_changed_from_cached) {
                    LOG_INFO("Startup request still pending: holding %umV @ %umA while waiting for requested %umV",
                             current_voltage_mv,
                             current_current_ma,
                             _requested_voltage_mv);
                }
            }
        }

        if (elapsed_ms >= NEGOTIATION_TIMEOUT_MS && _negotiation_state == NegotiationState::REQUESTING) {
            LOG_WARN("Contract negotiation timeout");
            _negotiation_state = NegotiationState::TIMEOUT;
        }
    }

    // EPR safe exit 3-step state machine:
    // STEPPING_DOWN -> REQUESTING_5V -> REQUESTING_TARGET -> NONE
    if (_epr_exit_state != EprExitState::NONE) {
        // Global timeout for entire EPR exit sequence
        uint32_t epr_elapsed = absolute_time_diff_us(_epr_exit_start, get_absolute_time()) / 1000;
        if (epr_elapsed >= EPR_EXIT_TIMEOUT_MS) {
            LOG_ERROR("EPR exit sequence timed out after %ums -- aborting", epr_elapsed);
            _epr_exit_state = EprExitState::NONE;
        }
        // Step failed or timed out at negotiation level
        else if (_negotiation_state == NegotiationState::FAILED ||
                 _negotiation_state == NegotiationState::TIMEOUT) {
            LOG_ERROR("EPR exit step failed (state=%d, exit_step=%d) -- aborting",
                      (int)_negotiation_state, (int)_epr_exit_state);
            _epr_exit_state = EprExitState::NONE;
        }
        // Step 1 complete: AVS step-down succeeded -> request 5V Fixed to cleanly exit EPR
        else if (_epr_exit_state == EprExitState::STEPPING_DOWN &&
                 _negotiation_state == NegotiationState::SUCCESS) {
            refreshActiveContract();
            LOG_INFO("EPR step 1/3 complete: AVS at %umV. Requesting 5V Fixed to exit EPR",
                     _active_contract.voltage_mv);
            _epr_exit_state = EprExitState::REQUESTING_5V;
            // Request 5V Fixed -- bypass EPR interception by setting state first
            _pre_request_voltage_mv = _active_contract.voltage_mv;
            _pre_request_current_ma = _active_contract.current_ma;
            // Deactivate AVS/PPS tracking for the intermediate 5V request
            _avs_active = false;
            _avs_voltage_mv = 0;
            _avs_current_ma = 0;
            _pps_active = false;
            _pps_voltage_mv = 0;
            _pps_current_ma = 0;
            if (!hw.pdController.requestFixedProfile(EPR_EXIT_SAFE_MV, EPR_EXIT_SAFE_CURRENT_MA)) {
                LOG_ERROR("EPR exit: failed to request 5V Fixed -- aborting");
                _epr_exit_state = EprExitState::NONE;
            } else {
                // Keep the polling fallback aligned with the intermediate 5V request,
                // otherwise the EPR exit sequence keeps comparing against the prior
                // AVS step-down target and never reports this stage as complete.
                _requested_contract_type = RequestedContractType::FIXED;
                _requested_voltage_mv    = EPR_EXIT_SAFE_MV;
                _requested_current_ma    = EPR_EXIT_SAFE_CURRENT_MA;
                _negotiation_state = NegotiationState::REQUESTING;
                _negotiation_start = get_absolute_time();
            }
        }
        // Step 2 complete: 5V Fixed succeeded -> now request user's actual target
        else if (_epr_exit_state == EprExitState::REQUESTING_5V &&
                 _negotiation_state == NegotiationState::SUCCESS) {
            refreshActiveContract();
            const char* type_str = "Fixed";
            if (_epr_deferred_contract_type == RequestedContractType::PPS) type_str = "PPS";
            else if (_epr_deferred_contract_type == RequestedContractType::AVS) type_str = "AVS";
            LOG_INFO("EPR step 2/3 complete: at %umV (SPR). Requesting target: %s %umV @ %umA",
                     _active_contract.voltage_mv,
                     type_str,
                     _epr_deferred_voltage_mv, _epr_deferred_current_ma);
            _epr_exit_state = EprExitState::REQUESTING_TARGET;
            // Fire the user's actual request (EPR exit state prevents re-interception)
            if (_epr_deferred_contract_type == RequestedContractType::PPS) {
                requestPpsVoltage(_epr_deferred_voltage_mv, _epr_deferred_current_ma, _epr_deferred_pdo_index);
            } else if (_epr_deferred_contract_type == RequestedContractType::AVS) {
                requestAvsVoltage(_epr_deferred_voltage_mv, _epr_deferred_current_ma, _epr_deferred_pdo_index);
            } else {
                requestFixedVoltage(_epr_deferred_voltage_mv, _epr_deferred_current_ma);
            }
        }
        // Step 3 complete: target request succeeded -> done
        else if (_epr_exit_state == EprExitState::REQUESTING_TARGET &&
                 _negotiation_state == NegotiationState::SUCCESS) {
            refreshActiveContract();
            LOG_INFO("EPR step 3/3 complete: safely transitioned to %umV @ %umA",
                     _active_contract.voltage_mv, _active_contract.current_ma);
            _epr_exit_state = EprExitState::NONE;
        }
    }

    // Deferred PDO discovery: if PD revision is unknown, TPS26750 may have negotiated
    // before RP2040 GPIO interrupts were set up (missed edge at cold boot).
    // Retry every 500ms until PDOs are found.
    if (_pd_revision[0] == '\0' && _charger_connected) {
        static absolute_time_t next_pdo_retry = {0};
        if (absolute_time_diff_us(next_pdo_retry, get_absolute_time()) >= 0) {
            next_pdo_retry = make_timeout_time_ms(PDO_RETRY_INTERVAL_MS);
            _pdo_count = hw.pdController.getSourceCapabilities(_pdo_cache, AppConfig::MAX_PDO_COUNT);
            _pdos_valid = (_pdo_count > 0);
            if (_pdos_valid) {
                detectPdRevision();
                refreshActiveContract();
                LOG_INFO("Deferred PDO discovery: found %d PDOs", _pdo_count);
            }
        }
    }

    // PPS keep-alive: must refresh contract every <10 seconds or source reverts to 5V
    if (_pps_active && _pps_voltage_mv > 0) {
        uint32_t elapsed_ms = absolute_time_diff_us(_pps_last_refresh, get_absolute_time()) / 1000;
        uint32_t refresh_interval_ms = PPS_REFRESH_INTERVAL_MS;
        if (settings.isAutoPpsEnabled() && _pps_user_target_mv > 0 &&
            !_pps_tuning_converged && !CcController::isRegulating()) {
            refresh_interval_ms = TUNE_REQUEST_INTERVAL_MS;
        }

        if (elapsed_ms >= refresh_interval_ms) {
            uint32_t request_mv = _pps_voltage_mv;

            // Auto PPS tuning: measure actual voltage and adjust request
            // Skip auto-tuning when CC controller is regulating (CC adjusts voltage for current)
            if (settings.isAutoPpsEnabled() && _pps_user_target_mv > 0 && !CcController::isRegulating()) {
                // Measure actual output voltage
                float measured_v;
                if (gpio_get(Board::PIN_SWITCH_EN)) {
                    measured_v = hw.powerMonitor.getBusVoltage();  // Post-switch (accurate)
                } else {
                    measured_v = hw.adc.getVBUS();  // Pre-switch (fallback)
                }
                uint32_t measured_mv = (uint32_t)(measured_v * 1000.0f);

                // Only tune if we have a valid measurement (> 1V, likely PPS is delivering)
                if (measured_mv > MIN_TUNING_VOLTAGE_MV) {
                    int32_t error = (int32_t)_pps_user_target_mv - (int32_t)measured_mv;

                    if (abs(error) > PPS_TUNE_THRESHOLD_MV) {
                        // Accumulate correction
                        _pps_correction_mv += error;

                        // Clamp correction to safety limit
                        if (_pps_correction_mv > PPS_TUNE_MAX_CORRECTION_MV)
                            _pps_correction_mv = PPS_TUNE_MAX_CORRECTION_MV;
                        if (_pps_correction_mv < -PPS_TUNE_MAX_CORRECTION_MV)
                            _pps_correction_mv = -PPS_TUNE_MAX_CORRECTION_MV;

                        _pps_tuning_converged = false;
                        LOG_DEBUG("PPS tune: target=%umV measured=%umV error=%dmV correction=%dmV",
                                  _pps_user_target_mv, measured_mv, error, _pps_correction_mv);
                    } else {
                        _pps_tuning_converged = true;
                    }
                }

                // Compute adjusted request voltage
                int32_t adjusted = (int32_t)_pps_user_target_mv + _pps_correction_mv;

                // Clamp to PPS range
                if (_pps_range_max_mv > 0) {
                    if (adjusted < (int32_t)_pps_range_min_mv) adjusted = (int32_t)_pps_range_min_mv;
                    if (adjusted > (int32_t)_pps_range_max_mv) adjusted = (int32_t)_pps_range_max_mv;
                }

                // Align to the PPS request step before re-requesting the contract.
                adjusted = static_cast<int32_t>(PdVoltage::alignDown(
                    static_cast<uint32_t>(adjusted), AppConfig::PPS_VOLTAGE_STEP_MV));

                request_mv = (uint32_t)adjusted;
            }

            LOG_DEBUG("PPS keep-alive: requesting %umV @ %umA", request_mv, _pps_current_ma);

            if (hw.pdController.requestPPSProfile(request_mv, _pps_current_ma,
                                                    _pps_range_min_mv, _pps_range_max_mv)) {
                _pps_last_refresh = get_absolute_time();
                _pps_voltage_mv = request_mv;  // Track what we actually requested
            } else {
                LOG_WARN("PPS keep-alive request failed");
            }
        }
    }

    // Fast convergence check: when PPS tuning is active,
    // check frequently (every 500ms) to detect drift in either direction
    if (_pps_active && settings.isAutoPpsEnabled() && _pps_user_target_mv > 0) {
        static absolute_time_t next_pps_convergence_check = {0};
        if (absolute_time_diff_us(next_pps_convergence_check, get_absolute_time()) >= 0) {
            next_pps_convergence_check = make_timeout_time_ms(TUNE_CONVERGENCE_CHECK_MS);
            checkTuningConvergenceImmediate();
        }
    }

    // AVS keep-alive: EPR contracts also need periodic re-request to maintain the contract
    if (_avs_active && _avs_voltage_mv > 0) {
        uint32_t elapsed_ms = absolute_time_diff_us(_avs_last_refresh, get_absolute_time()) / 1000;
        uint32_t refresh_interval_ms = AVS_REFRESH_INTERVAL_MS;
        if (settings.isAutoAvsEnabled() && _avs_user_target_mv > 0 &&
            !_avs_tuning_converged && !CcController::isRegulating()) {
            refresh_interval_ms = TUNE_REQUEST_INTERVAL_MS;
        }

        if (elapsed_ms >= refresh_interval_ms) {
            uint32_t request_mv = _avs_voltage_mv;

            // Auto AVS tuning: measure actual voltage and adjust request
            // Skip auto-tuning when CC controller is regulating (CC adjusts voltage for current)
            if (settings.isAutoAvsEnabled() && _avs_user_target_mv > 0 && !CcController::isRegulating()) {
                // Measure actual output voltage
                float measured_v;
                if (gpio_get(Board::PIN_SWITCH_EN)) {
                    measured_v = hw.powerMonitor.getBusVoltage();  // Post-switch (accurate)
                } else {
                    measured_v = hw.adc.getVBUS();  // Pre-switch (fallback)
                }
                uint32_t measured_mv = (uint32_t)(measured_v * 1000.0f);

                // Only tune if we have a valid measurement (> 1V, likely AVS is delivering)
                if (measured_mv > MIN_TUNING_VOLTAGE_MV) {
                    int32_t error = (int32_t)_avs_user_target_mv - (int32_t)measured_mv;

                    if (abs(error) > AVS_TUNE_THRESHOLD_MV) {
                        // Accumulate correction
                        _avs_correction_mv += error;

                        // Clamp correction to safety limit
                        if (_avs_correction_mv > AVS_TUNE_MAX_CORRECTION_MV)
                            _avs_correction_mv = AVS_TUNE_MAX_CORRECTION_MV;
                        if (_avs_correction_mv < -AVS_TUNE_MAX_CORRECTION_MV)
                            _avs_correction_mv = -AVS_TUNE_MAX_CORRECTION_MV;

                        _avs_tuning_converged = false;
                        LOG_DEBUG("AVS tune: target=%umV measured=%umV error=%dmV correction=%dmV",
                                  _avs_user_target_mv, measured_mv, error, _avs_correction_mv);
                    } else {
                        _avs_tuning_converged = true;
                    }
                }

                // Compute adjusted request voltage
                int32_t adjusted = (int32_t)_avs_user_target_mv + _avs_correction_mv;

                // Clamp to AVS range
                if (_avs_range_max_mv > 0) {
                    if (adjusted < (int32_t)_avs_range_min_mv) adjusted = (int32_t)_avs_range_min_mv;
                    if (adjusted > (int32_t)_avs_range_max_mv) adjusted = (int32_t)_avs_range_max_mv;
                }

                // Align to the configured AVS request step before re-requesting the contract.
                adjusted = static_cast<int32_t>(PdVoltage::alignDown(
                    static_cast<uint32_t>(adjusted), AppConfig::AVS_VOLTAGE_STEP_MV));

                request_mv = (uint32_t)adjusted;
            }

            LOG_DEBUG("AVS keep-alive: requesting %umV @ %umA", request_mv, _avs_current_ma);

            if (hw.pdController.requestAVSProfile(request_mv, _avs_current_ma,
                                                    _avs_range_min_mv, _avs_range_max_mv)) {
                _avs_last_refresh = get_absolute_time();
                _avs_voltage_mv = request_mv;  // Track what we actually requested
            } else {
                LOG_WARN("AVS keep-alive request failed");
            }
        }
    }

    // Fast convergence check: when AVS tuning is active,
    // check frequently (every 500ms) to detect drift in either direction
    if (_avs_active && settings.isAutoAvsEnabled() && _avs_user_target_mv > 0) {
        static absolute_time_t next_avs_convergence_check = {0};
        if (absolute_time_diff_us(next_avs_convergence_check, get_absolute_time()) >= 0) {
            next_avs_convergence_check = make_timeout_time_ms(TUNE_CONVERGENCE_CHECK_MS);
            checkTuningConvergenceImmediate();
        }
    }

    // Periodic contract refresh for display sync (every 1 second)
    // Ensures displayed contract matches actual state after EPR transitions
    static absolute_time_t next_contract_refresh = {0};
    if (absolute_time_diff_us(next_contract_refresh, get_absolute_time()) >= 0) {
        next_contract_refresh = make_timeout_time_ms(CONTRACT_REFRESH_INTERVAL_MS);

        uint32_t old_voltage = _active_contract.voltage_mv;
        refreshActiveContract();

        // If voltage changed significantly, log it (display updates naturally via overwrite rendering)
        if (_active_contract.voltage_mv != old_voltage) {
            LOG_INFO("Contract voltage changed: %umV -> %umV",
                     old_voltage, _active_contract.voltage_mv);
        }
    }
}

// ============================================================================
// PDO Access
// ============================================================================

uint8_t PdManager::getSourceCapabilities(SourceCapability* caps, uint8_t max_caps) {
    // Refresh cache if needed
    if (!_pdos_valid) {
        _pdo_count = hw.pdController.getSourceCapabilities(_pdo_cache, AppConfig::MAX_PDO_COUNT);
        _pdos_valid = (_pdo_count > 0);
        if (_pdos_valid) detectPdRevision();
    }

    // Copy from cache
    uint8_t count = (_pdo_count < max_caps) ? _pdo_count : max_caps;
    for (uint8_t i = 0; i < count; i++) {
        caps[i] = _pdo_cache[i];
    }

    return count;
}

bool PdManager::getChargerDiagInfo(ChargerDiagInfo& info) {
    info.pd_revision = "N/A";
    info.cc_orientation = 0;
    info.supports_qc4 = false;
    info.supports_qc5 = false;
    info.charger_identity_valid = _charger_identity_valid;
    info.charger_vendor_id = _charger_vendor_id;
    info.charger_product_id = _charger_product_id;
    copyStringTruncated(info.charger_name, sizeof(info.charger_name), _charger_name);
    info.detected_cable_rating = DetectedCableRating::UNKNOWN_CHARGER_LIMIT;
    info.charger_max_power_w = 0;

    if (!_charger_connected) {
        return false;
    }

    if (!_pdos_valid) {
        _pdo_count = hw.pdController.getSourceCapabilities(_pdo_cache, AppConfig::MAX_PDO_COUNT);
        _pdos_valid = (_pdo_count > 0);
        if (_pdos_valid) {
            detectPdRevision();
        }
    }

    if (_pd_revision[0] != '\0') {
        info.pd_revision = _pd_revision;
    }

    uint8_t status_buf[5] = {0};
    if (hw.pdController.getStatus(status_buf)) {
        info.cc_orientation = (status_buf[0] & TPS_STATUS_ORIENTATION) ? 2 : 1;
    }

    bool has_pps = false;

    for (uint8_t i = 0; i < _pdo_count; i++) {
        const SourceCapability& pdo = _pdo_cache[i];
        uint32_t pdo_power_w = getSourceCapabilityMaxPowerW(pdo);
        if (pdo_power_w > info.charger_max_power_w) {
            info.charger_max_power_w = pdo_power_w;
        }

        if (pdo.is_pps) {
            has_pps = true;
        }
    }

    info.detected_cable_rating = inferDetectedCableRating(_pdo_cache, _pdo_count);
    info.supports_qc4 = has_pps;
    info.supports_qc5 = has_pps && (info.charger_max_power_w >= 100);
    return true;
}

void PdManager::clearChargerIdentity() {
    _charger_identity_valid = false;
    _charger_vendor_id = 0;
    _charger_product_id = 0;
    _charger_name[0] = '\0';
}

bool PdManager::refreshChargerIdentity() {
    clearChargerIdentity();

    if (!_charger_connected) {
        return false;
    }

    uint8_t charger_response[GPPI_RESPONSE_READ_BYTES] = {0};
    uint16_t charger_response_len = 0;
    bool charger_ok = hw.pdController.getManufacturerInfo(GppiFrameType::SOP,
                                                          charger_response,
                                                          GPPI_RESPONSE_READ_BYTES,
                                                          &charger_response_len);
    if (!charger_ok) {
        return false;
    }

    uint16_t charger_vendor_id = 0;
    uint16_t charger_product_id = 0;
    char charger_name[32] = {0};
    if (!decodeManufacturerInfoResponse(charger_response,
                                        static_cast<uint8_t>(charger_response_len),
                                        charger_vendor_id,
                                        charger_product_id,
                                        charger_name,
                                        sizeof(charger_name))) {
        return false;
    }

    _charger_identity_valid = true;
    _charger_vendor_id = charger_vendor_id;
    _charger_product_id = charger_product_id;
    copyStringTruncated(_charger_name, sizeof(_charger_name), charger_name);
    return true;
}

// ============================================================================
// EPR Safe Exit Helpers
// ============================================================================

bool PdManager::needsEprExit(uint32_t target_voltage_mv, bool target_is_pps) const {
    // EPR exit needed when:
    // 1. Currently in EPR territory (>20V)
    // 2. Target is SPR (fixed <=20V or any PPS which is always SPR)
    if (_active_contract.voltage_mv <= AppConfig::EPR_SPR_MAX_MV) {
        return false;  // Already in SPR range
    }
    if (target_is_pps) {
        return true;  // PPS is always SPR (max 21V)
    }
    return target_voltage_mv <= AppConfig::EPR_SPR_MAX_MV;
}

bool PdManager::isSafeEprExitPossible() const {
    // Safe EPR exit requires an EPR AVS APDO that can step VBUS into SPR range
    // without leaving EPR mode first.
    for (uint8_t i = 0; i < _pdo_count; i++) {
        if (_pdo_cache[i].is_avs &&
            _pdo_cache[i].voltage_mv > AppConfig::EPR_SPR_MAX_MV &&
            _pdo_cache[i].min_voltage_mv <= AppConfig::EPR_SPR_MAX_MV) {
            return true;
        }
    }
    return false;
}

bool PdManager::findAvsSafeVoltage(uint32_t& avs_voltage_mv, uint32_t& avs_current_ma,
                                   int8_t& avs_pdo_index) const {
    // Prefer an EPR AVS APDO whose minimum voltage is already within SPR range.
    // This preserves the old firmware behavior: step down while still in EPR,
    // then request 5V to exit EPR cleanly. Using SPR AVS directly here can cause
    // some chargers to reset during the EPR->SPR transition.
    uint32_t best_min_mv = UINT32_MAX;
    int8_t best_index = -1;

    for (uint8_t i = 0; i < _pdo_count; i++) {
        if (_pdo_cache[i].is_avs &&
            _pdo_cache[i].voltage_mv > AppConfig::EPR_SPR_MAX_MV &&
            _pdo_cache[i].min_voltage_mv <= AppConfig::EPR_SPR_MAX_MV &&
            _pdo_cache[i].min_voltage_mv < best_min_mv) {
            best_min_mv = _pdo_cache[i].min_voltage_mv;
            best_index = (int8_t)i;
        }
    }

    if (best_index < 0) {
        return false;
    }

    avs_pdo_index = best_index;
    avs_voltage_mv = ((best_min_mv + AppConfig::AVS_VOLTAGE_STEP_MV - 1) /
                      AppConfig::AVS_VOLTAGE_STEP_MV) * AppConfig::AVS_VOLTAGE_STEP_MV;
    avs_current_ma = _pdo_cache[best_index].max_current_ma;
    return true;
}

// ============================================================================
// Contract Negotiation
// ============================================================================

void PdManager::clearPpsTracking() {
    _pps_active = false;
    _pps_voltage_mv = 0;
    _pps_current_ma = 0;
    _pps_user_target_mv = 0;
    _pps_correction_mv = 0;
    _pps_tuning_converged = false;
    _pps_range_min_mv = 0;
    _pps_range_max_mv = 0;
    _active_pdo_index = -1;
}

void PdManager::clearAvsTracking() {
    _avs_active = false;
    _avs_voltage_mv = 0;
    _avs_current_ma = 0;
    _avs_user_target_mv = 0;
    _avs_correction_mv = 0;
    _avs_tuning_converged = false;
    _avs_range_min_mv = 0;
    _avs_range_max_mv = 0;
    _active_pdo_index = -1;
}

void PdManager::clearRequestedContract() {
    _requested_contract_type = RequestedContractType::NONE;
    _requested_voltage_mv = 0;
    _requested_current_ma = 0;
    _pre_request_voltage_mv = 0;
    _pre_request_current_ma = 0;
}

bool PdManager::primeStartupContract() {
    StartupContractMode mode = settings.getStartupNegotiationMode();
    if (mode == StartupContractMode::HIGHEST_VOLTAGE) {
        return false;
    }

    uint32_t startup_current_ma = settings.getCurrentLimit();
    if (startup_current_ma < 500) {
        startup_current_ma = 500;
    }
    if (startup_current_ma > AppConfig::CURRENT_LIMIT_MAX_MA) {
        startup_current_ma = AppConfig::CURRENT_LIMIT_MAX_MA;
    }

    if (mode == StartupContractMode::LOWEST_VOLTAGE) {
        LOG_INFO("Startup pre-boot request: forcing 5V fixed floor before PDO discovery");
        return requestFixedVoltage(5000, startup_current_ma);
    }

    SavedStartupContractSnapshot snapshot = getSavedStartupContractSnapshot();
    char snapshot_desc[96];
    describeSavedStartupContract(snapshot, snapshot_desc, sizeof(snapshot_desc));
    LOG_INFO("Startup pre-boot request: saved snapshot %s", snapshot_desc);

    uint32_t saved_target_mv = getSavedTargetVoltageMv(snapshot, _pdo_cache, _pdo_count);
    if (snapshot.type == SavedStartupContractType::NONE || saved_target_mv == 0) {
        LOG_INFO("Startup pre-boot request: no saved target, forcing 5V fixed floor before PDO discovery");
        return requestFixedVoltage(5000, startup_current_ma);
    }

    if (snapshot.type == SavedStartupContractType::FIXED) {
        LOG_INFO("Startup pre-boot request: blindly requesting saved fixed contract %umV before PDO discovery",
                 saved_target_mv);
        return requestFixedVoltage(saved_target_mv, startup_current_ma);
    }

    if (saved_target_mv > AppConfig::EPR_SPR_MAX_MV) {
        LOG_INFO("Startup pre-boot request: blindly requesting saved EPR target %umV before PDO discovery",
                 saved_target_mv);
        return requestAvsVoltage(saved_target_mv, startup_current_ma);
    }

    if (snapshot.type == SavedStartupContractType::PPS && saved_target_mv < 5000) {
        LOG_INFO("Startup pre-boot request: blindly requesting saved low-PPS target %umV before PDO discovery",
                 saved_target_mv);
        return requestPpsVoltage(saved_target_mv, startup_current_ma);
    }

    if (snapshot.type == SavedStartupContractType::AVS) {
        LOG_INFO("Startup pre-boot request: clamping to 5V until PDO discovery can restore AVS target %umV",
                 saved_target_mv);
    } else if (snapshot.type == SavedStartupContractType::PPS) {
        LOG_INFO("Startup pre-boot request: clamping to 5V until PDO discovery can restore PPS target %umV",
                 saved_target_mv);
    } else {
        LOG_INFO("Startup pre-boot request: clamping to 5V until PDO discovery can restore programmable target %umV",
                 saved_target_mv);
    }

    return requestFixedVoltage(5000, startup_current_ma);
}

bool PdManager::requestContract(const SourceCapability& pdo) {
    if (pdo.is_pps) {
        // For PPS, request max voltage as default (user can adjust via PPS voltage mode)
        return requestPpsVoltage(pdo.voltage_mv, pdo.max_current_ma);
    } else if (pdo.is_avs) {
        // For AVS, request max voltage (which is stored in voltage_mv field)
        return requestAvsVoltage(pdo.voltage_mv, pdo.max_current_ma);
    } else {
        return requestFixedVoltage(pdo.voltage_mv, pdo.max_current_ma);
    }
}

bool PdManager::requestFixedVoltage(uint32_t voltage_mv, uint32_t current_ma) {
    LOG_INFO("Requesting Fixed: %umV @ %umA", voltage_mv, current_ma);

    // EPR safe exit: if currently in EPR and target is SPR, initiate 3-step exit
    if (_epr_exit_state == EprExitState::NONE && needsEprExit(voltage_mv, false)) {
        uint32_t avs_v, avs_i;
        int8_t avs_idx = -1;
        if (findAvsSafeVoltage(avs_v, avs_i, avs_idx)) {
            LOG_INFO("EPR exit: 3-step sequence for Fixed %umV (AVS %umV -> 5V -> %umV)",
                     voltage_mv, avs_v, voltage_mv);
            _epr_deferred_voltage_mv    = voltage_mv;
            _epr_deferred_current_ma    = current_ma;
            _epr_deferred_contract_type = RequestedContractType::FIXED;
            _epr_exit_state             = EprExitState::STEPPING_DOWN;
            _epr_exit_start             = get_absolute_time();
            return requestAvsVoltage(avs_v, avs_i, avs_idx);
        } else {
            LOG_WARN("EPR exit needed but no suitable AVS PDO found -- direct request (may reboot)");
        }
    }

    // Store pre-request contract for polling fallback
    _pre_request_voltage_mv = _active_contract.voltage_mv;
    _pre_request_current_ma = _active_contract.current_ma;
    _requested_contract_type = RequestedContractType::FIXED;
    _requested_voltage_mv = voltage_mv;
    _requested_current_ma = current_ma;

    bool success = hw.pdController.requestFixedProfile(voltage_mv, current_ma);

    if (success) {
        _negotiation_state = NegotiationState::REQUESTING;
        _negotiation_start = get_absolute_time();

        // Deactivate PPS mode when switching to fixed
        clearPpsTracking();

        // Deactivate AVS mode when switching to fixed
        clearAvsTracking();
    } else {
        _negotiation_state = NegotiationState::FAILED;
        LOG_ERROR("Failed to send Fixed contract request");
    }

    return success;
}

bool PdManager::requestPpsVoltage(uint32_t voltage_mv, uint32_t current_ma, int8_t pdo_index) {
    LOG_INFO("Requesting PPS: %umV @ %umA (PDO index %d)", voltage_mv, current_ma, pdo_index);

    // EPR safe exit: if currently in EPR and target is PPS (SPR), initiate 3-step exit
    if (_epr_exit_state == EprExitState::NONE && needsEprExit(voltage_mv, true)) {
        uint32_t avs_v, avs_i;
        int8_t avs_idx = -1;
        if (findAvsSafeVoltage(avs_v, avs_i, avs_idx)) {
            LOG_INFO("EPR exit: 3-step sequence for PPS %umV (AVS %umV -> 5V -> PPS %umV)",
                     voltage_mv, avs_v, voltage_mv);
            _epr_deferred_voltage_mv    = voltage_mv;
            _epr_deferred_current_ma    = current_ma;
            _epr_deferred_contract_type = RequestedContractType::PPS;
            _epr_deferred_pdo_index     = pdo_index;
            _epr_exit_state             = EprExitState::STEPPING_DOWN;
            _epr_exit_start             = get_absolute_time();
            return requestAvsVoltage(avs_v, avs_i, avs_idx);
        } else {
            LOG_WARN("EPR exit needed but no suitable AVS PDO found -- direct request (may reboot)");
        }
    }

    // Resolve APDO bounds: if a specific PDO index is provided, use it directly;
    // otherwise fall back to first-match search (startup restore, EPR exit paths).
    uint32_t pdo_min_mv = 0;
    uint32_t pdo_max_mv = 0;
    int8_t resolved_index = pdo_index;
    if (pdo_index >= 0 && pdo_index < _pdo_count && _pdo_cache[pdo_index].is_pps) {
        pdo_min_mv = _pdo_cache[pdo_index].min_voltage_mv;
        pdo_max_mv = _pdo_cache[pdo_index].voltage_mv;
    } else {
        for (uint8_t i = 0; i < _pdo_count; i++) {
            if (_pdo_cache[i].is_pps &&
                voltage_mv >= _pdo_cache[i].min_voltage_mv &&
                voltage_mv <= _pdo_cache[i].voltage_mv) {
                pdo_min_mv = _pdo_cache[i].min_voltage_mv;
                pdo_max_mv = _pdo_cache[i].voltage_mv;
                resolved_index = (int8_t)i;
                break;
            }
        }
        if (pdo_max_mv == 0) {
            // No matching PDO found: use broad defaults so the request still goes through.
            pdo_min_mv = 3300;
            pdo_max_mv = 21000;
        }
    }

    // Store pre-request contract for polling fallback
    _pre_request_voltage_mv = _active_contract.voltage_mv;
    _pre_request_current_ma = _active_contract.current_ma;
    _requested_contract_type = RequestedContractType::PPS;
    _requested_voltage_mv = voltage_mv;
    _requested_current_ma = current_ma;

    bool success = hw.pdController.requestPPSProfile(voltage_mv, current_ma, pdo_min_mv, pdo_max_mv);

    if (success) {
        _negotiation_state = NegotiationState::REQUESTING;
        _negotiation_start = get_absolute_time();

        // Track PPS state for keep-alive
        _pps_active = true;
        _pps_voltage_mv = voltage_mv;
        _pps_current_ma = current_ma;
        _pps_last_refresh = get_absolute_time();

        // Deactivate AVS mode when switching to PPS
        clearAvsTracking();

        // Auto PPS tuning: store user target and reset correction
        _pps_user_target_mv = voltage_mv;
        _pps_correction_mv = 0;
        _pps_tuning_converged = false;

        // Store resolved identity and bounds for keep-alive clamping and UI highlighting
        _active_pdo_index = resolved_index;
        _pps_range_min_mv = pdo_min_mv;
        _pps_range_max_mv = pdo_max_mv;
    } else {
        _negotiation_state = NegotiationState::FAILED;
        LOG_ERROR("Failed to send PPS contract request");
    }

    return success;
}

bool PdManager::requestAvsVoltage(uint32_t voltage_mv, uint32_t current_ma, int8_t pdo_index) {
    LOG_INFO("Requesting AVS: %umV @ %umA (PDO index %d)", voltage_mv, current_ma, pdo_index);

    // Mirror the fixed-request EPR exit guard here as well. A direct EPR->SPR
    // AVS transition can hard-reset the charger, so route it through the staged
    // AVS step-down sequence first.
    if (_epr_exit_state == EprExitState::NONE && needsEprExit(voltage_mv, false)) {
        uint32_t avs_v, avs_i;
        int8_t avs_idx = -1;
        if (findAvsSafeVoltage(avs_v, avs_i, avs_idx)) {
            LOG_INFO("EPR exit: 3-step sequence for AVS %umV (AVS %umV -> 5V -> AVS %umV)",
                     voltage_mv, avs_v, voltage_mv);
            _epr_deferred_voltage_mv    = voltage_mv;
            _epr_deferred_current_ma    = current_ma;
            _epr_deferred_contract_type = RequestedContractType::AVS;
            _epr_deferred_pdo_index     = pdo_index;
            _epr_exit_state             = EprExitState::STEPPING_DOWN;
            _epr_exit_start             = get_absolute_time();
            return requestAvsVoltage(avs_v, avs_i, avs_idx);  // re-entrant safe: state != NONE
        } else {
            LOG_WARN("EPR exit needed but no suitable AVS PDO found -- direct request (may reboot)");
        }
    }

    // Resolve APDO bounds: if a specific PDO index is provided, use it directly;
    // otherwise fall back to first-match search (startup restore, EPR step-down paths).
    uint32_t pdo_min_mv = 0;
    uint32_t pdo_max_mv = 0;
    int8_t resolved_index = pdo_index;
    if (pdo_index >= 0 && pdo_index < _pdo_count && _pdo_cache[pdo_index].is_avs) {
        pdo_min_mv = _pdo_cache[pdo_index].min_voltage_mv;
        pdo_max_mv = _pdo_cache[pdo_index].voltage_mv;
    } else {
        for (uint8_t i = 0; i < _pdo_count; i++) {
            if (_pdo_cache[i].is_avs &&
                voltage_mv >= _pdo_cache[i].min_voltage_mv &&
                voltage_mv <= _pdo_cache[i].voltage_mv) {
                pdo_min_mv = _pdo_cache[i].min_voltage_mv;
                pdo_max_mv = _pdo_cache[i].voltage_mv;
                resolved_index = (int8_t)i;
                break;
            }
        }
        if (pdo_max_mv == 0) {
            // No matching PDO found: use broad defaults so the request still goes through.
            pdo_min_mv = 9000;
            pdo_max_mv = 48000;
        }
    }

    // Store pre-request contract for polling fallback
    _pre_request_voltage_mv = _active_contract.voltage_mv;
    _pre_request_current_ma = _active_contract.current_ma;
    _requested_contract_type = RequestedContractType::AVS;
    _requested_voltage_mv = voltage_mv;
    _requested_current_ma = current_ma;

    bool success = hw.pdController.requestAVSProfile(voltage_mv, current_ma, pdo_min_mv, pdo_max_mv);

    if (success) {
        _negotiation_state = NegotiationState::REQUESTING;
        _negotiation_start = get_absolute_time();

        // Deactivate PPS mode when switching to AVS
        clearPpsTracking();

        // Track AVS state for keep-alive
        _avs_active = true;
        _avs_voltage_mv = voltage_mv;
        _avs_current_ma = current_ma;
        _avs_last_refresh = get_absolute_time();

        // Auto AVS tuning: store user target and reset correction
        _avs_user_target_mv = voltage_mv;
        _avs_correction_mv = 0;
        _avs_tuning_converged = false;

        // Store resolved identity and bounds for keep-alive clamping and UI highlighting
        _active_pdo_index = resolved_index;
        _avs_range_min_mv = pdo_min_mv;
        _avs_range_max_mv = pdo_max_mv;
    } else {
        _negotiation_state = NegotiationState::FAILED;
        LOG_ERROR("Failed to send AVS contract request");
    }

    return success;
}

void PdManager::setCcKeepAliveVoltage(uint32_t voltage_mv) {
    if (_pps_active) {
        _pps_voltage_mv = voltage_mv;
        _pps_last_refresh = get_absolute_time();
    } else if (_avs_active) {
        _avs_voltage_mv = voltage_mv;
        _avs_last_refresh = get_absolute_time();
    }
}

// ============================================================================
// Active Contract
// ============================================================================

bool PdManager::refreshActiveContract() {
    uint32_t voltage_mv, current_ma;
    auto within_tolerance = [](uint32_t lhs, uint32_t rhs, uint32_t tolerance_mv) {
        return (lhs > rhs) ? (lhs - rhs <= tolerance_mv) : (rhs - lhs <= tolerance_mv);
    };

    if (hw.pdController.getActiveContract(voltage_mv, current_ma)) {
        _active_contract.voltage_mv = voltage_mv;
        _active_contract.current_ma = current_ma;
        _active_contract.valid = true;

        bool matches_fixed_pdo = _pdos_valid &&
            matchesFixedPdo(_pdo_cache, _pdo_count, voltage_mv, FIXED_MATCH_TOLERANCE_MV);
        bool waiting_for_programmable_request =
            (_negotiation_state == NegotiationState::REQUESTING) &&
            (_requested_contract_type == RequestedContractType::PPS ||
             _requested_contract_type == RequestedContractType::AVS);

        // Drop stale tracked programmable state when the source changed or boot priming
        // failed to obtain real PDO-backed PPS/AVS support.
        // Note: cache validity (!_pdos_valid) is intentionally NOT tested here -- doing so
        // would clear user-facing state (Vset, target) every time the menu opens and
        // invalidates the cache. Preserve programmable state through normal load-induced
        // droop. Also preserve it while a PPS/AVS request is still in flight, otherwise a
        // poll of the pre-existing fixed contract can hide Vset before negotiation completes.
        // Only drop it once the live contract has actually snapped back to a fixed rail.
        if (_pps_active && (_pps_voltage_mv == 0 ||
            (!waiting_for_programmable_request &&
             !within_tolerance(voltage_mv, _pps_voltage_mv, PPS_TRACKING_TOLERANCE_MV) &&
             matches_fixed_pdo))) {
            clearPpsTracking();
        }
        if (_avs_active && (_avs_voltage_mv == 0 ||
            (!waiting_for_programmable_request &&
             !within_tolerance(voltage_mv, _avs_voltage_mv, AVS_TRACKING_TOLERANCE_MV) &&
             matches_fixed_pdo))) {
            clearAvsTracking();
        }

        // Detect PPS/AVS from tracked state or by matching against PDO cache.
        // On warm MCU reset, _pps_active/_avs_active is false but TPS26750 still has a contract.
        // Detect this by checking if the active voltage matches a fixed PDO exactly.
        bool detected_pps = _pps_active;
        bool detected_avs = _avs_active;

        if (!_pps_active && !_avs_active && _pdos_valid && voltage_mv > 0) {
            // Check if this voltage matches any fixed PDO
            bool matches_fixed = false;
            for (uint8_t i = 0; i < _pdo_count; i++) {
                if (!_pdo_cache[i].is_pps && !_pdo_cache[i].is_avs &&
                    _pdo_cache[i].voltage_mv == voltage_mv) {
                    matches_fixed = true;
                    break;
                }
            }
            // If no fixed PDO matches, check if a PPS or AVS PDO covers this voltage
            if (!matches_fixed) {
                for (uint8_t i = 0; i < _pdo_count; i++) {
                    if (_pdo_cache[i].is_pps &&
                        voltage_mv >= _pdo_cache[i].min_voltage_mv &&
                        voltage_mv <= _pdo_cache[i].voltage_mv) {
                        detected_pps = true;
                        // Restore PPS keep-alive state so it doesn't revert to 5V
                        _pps_active = true;
                        _pps_voltage_mv = voltage_mv;
                        _pps_current_ma = current_ma;
                        _pps_last_refresh = get_absolute_time();
                        _pps_range_min_mv = _pdo_cache[i].min_voltage_mv;
                        _pps_range_max_mv = _pdo_cache[i].voltage_mv;
                        _active_pdo_index = (int8_t)i;
                        LOG_INFO("Detected active PPS contract on warm reset: %umV", voltage_mv);
                        break;
                    }
                    if (_pdo_cache[i].is_avs &&
                        voltage_mv >= _pdo_cache[i].min_voltage_mv &&
                        voltage_mv <= _pdo_cache[i].voltage_mv) {
                        detected_avs = true;
                        _avs_active = true;
                        _avs_voltage_mv = voltage_mv;
                        _avs_current_ma = current_ma;
                        _avs_last_refresh = get_absolute_time();
                        _avs_range_min_mv = _pdo_cache[i].min_voltage_mv;
                        _avs_range_max_mv = _pdo_cache[i].voltage_mv;
                        _active_pdo_index = (int8_t)i;
                        LOG_INFO("Detected active AVS contract on warm reset: %umV", voltage_mv);
                        break;
                    }
                }
            }
        }

        _active_contract.is_pps = detected_pps;
        _active_contract.is_avs = detected_avs;
        // EPR AVS APDOs always have min_voltage > 9V (e.g. 15V); SPR AVS is always 9V.
        _active_contract.is_epr = detected_avs && (_avs_range_min_mv != 9000 && _avs_range_min_mv != 0);

        // Prefer the requested programmable current when a PPS/AVS contract overlaps
        // a fixed PDO and the controller reports the fixed-PDO current instead.
        if (detected_pps && _pps_current_ma > 0) {
            _active_contract.current_ma = _pps_current_ma;
        } else if (detected_avs && _avs_current_ma > 0) {
            _active_contract.current_ma = _avs_current_ma;
        }

        // Cache the active programmable range for UI/CLI validation.
        if (detected_pps) {
            _active_contract.programmable_min_mv = _pps_range_min_mv;
            _active_contract.programmable_max_mv = _pps_range_max_mv;
        } else if (detected_avs) {
            _active_contract.programmable_min_mv = _avs_range_min_mv;
            _active_contract.programmable_max_mv = _avs_range_max_mv;
        } else {
            _active_contract.programmable_min_mv = 0;
            _active_contract.programmable_max_mv = 0;
        }

        return true;
    }

    _active_contract.valid = false;
    _active_contract.is_pps = false;
    _active_contract.is_avs = false;
    _active_contract.is_epr = false;
    _active_contract.programmable_min_mv = 0;
    _active_contract.programmable_max_mv = 0;
    if (!_pdos_valid || !_charger_connected) {
        clearPpsTracking();
        clearAvsTracking();
        _pd_revision[0] = '\0';
    }
    return false;
}

// ============================================================================
// Mode Query
// ============================================================================

bool PdManager::getMode(char* mode_str) {
    return hw.pdController.getMode(mode_str);
}

// ============================================================================
// PD Revision Detection
// ============================================================================

void PdManager::detectPdRevision() {
    bool has_epr = (_pdo_count > 7);
    bool has_pps = false;
    bool has_epr_avs = false;
    bool has_spr_avs = false;
    const char* detected_revision = "";

    for (uint8_t i = 0; i < _pdo_count; i++) {
        if (_pdo_cache[i].is_avs) {
            if (_pdo_cache[i].min_voltage_mv == 9000) {
                has_spr_avs = true;
            } else {
                has_epr_avs = true;
                has_epr = true;
            }
        }
        if (_pdo_cache[i].is_pps) has_pps = true;
    }

    // SPR AVS is new in PD 3.2, so its presence is enough to identify PD 3.2.
    if (has_spr_avs) {
        detected_revision = "PD3.2";
    } else if (has_epr || has_epr_avs) {
        // EPR-only sources map to PD 3.1.
        detected_revision = "PD3.1";
    } else if (has_pps) {
        // PPS was introduced in PD 3.0
        detected_revision = "PD3.0";
    } else if (_pdo_count > 0) {
        // If neither PPS nor AVS are present and <= 7 PDOs, assume PD 2.0
        detected_revision = "PD2.0";
    }

    if (strcmp(_pd_revision, detected_revision) != 0) {
        if (detected_revision[0] != '\0') {
            copyStringTruncated(_pd_revision, sizeof(_pd_revision), detected_revision);
            LOG_INFO("Detected PD revision (from PDOs): %s", _pd_revision);
        } else {
            _pd_revision[0] = '\0';
        }
    }
}

// ============================================================================
// Interrupt Handling
// ============================================================================

void PdManager::handlePdInterrupt() {
    uint8_t events[11] = {0};

    if (!hw.pdController.readInterrupts(events)) {
        LOG_WARN("Failed to read PD interrupts");
        return;
    }

    // Check for new contract event (bit 12)
    if (hw.pdController.isInterruptSet(events, 12)) {
        LOG_INFO("New contract negotiated (interrupt)");

        // Refresh active contract
        refreshActiveContract();

        // Refresh PDO cache and PD revision if not yet valid (e.g. cold boot)
        if (!_pdos_valid) {
            _pdo_count = hw.pdController.getSourceCapabilities(_pdo_cache, AppConfig::MAX_PDO_COUNT);
            _pdos_valid = (_pdo_count > 0);
            if (_pdos_valid) {
                detectPdRevision();
            }
        }

        // Update negotiation state
        if (_negotiation_state == NegotiationState::REQUESTING && isRequestedContractReached()) {
            _negotiation_state = NegotiationState::SUCCESS;
        }

        // Clear the interrupt
        const auto clear_mask = makeInterruptClearMask(12);
        hw.pdController.clearInterrupts(clear_mask.data());
    }

    // Check for source capabilities received (bit 14) - happens on EPR mode entry/exit
    if (hw.pdController.isInterruptSet(events, 14)) {
        uint8_t old_count = _pdo_count;

        // Invalidate and refresh PDO cache
        _pdos_valid = false;
        _pdo_count = hw.pdController.getSourceCapabilities(_pdo_cache, AppConfig::MAX_PDO_COUNT);
        _pdos_valid = (_pdo_count > 0);

        if (_pdos_valid) {
            detectPdRevision();
            LOG_INFO("Source capabilities updated: %d PDOs (was %d)", _pdo_count, old_count);
        } else {
            LOG_WARN("Source capabilities received but no PDOs found");
        }

        // Also refresh active contract as it may have changed
        refreshActiveContract();

        // Clear the interrupt
        const auto clear_mask = makeInterruptClearMask(14);
        hw.pdController.clearInterrupts(clear_mask.data());
    }

    // Check for plug insert/removal (bit 3)
    if (hw.pdController.isInterruptSet(events, 3)) {
        LOG_INFO("Plug insert/removal detected");

        // Invalidate PDO cache
        _pdos_valid = false;
        clearPpsTracking();
        clearAvsTracking();
        clearRequestedContract();
        _pd_revision[0] = '\0';
        clearChargerIdentity();
        _active_contract.valid = false;
        _active_contract.is_pps = false;
        _active_contract.is_avs = false;
        _active_contract.programmable_min_mv = 0;
        _active_contract.programmable_max_mv = 0;
        _negotiation_state = NegotiationState::IDLE;
        _epr_exit_state = EprExitState::NONE;

        // Check connection status
        uint32_t voltage_mv, current_ma;
        _charger_connected = hw.pdController.getActiveContract(voltage_mv, current_ma);
        if (_charger_connected) {
            refreshActiveContract();
        }

        // Clear the interrupt
        const auto clear_mask = makeInterruptClearMask(3);
        hw.pdController.clearInterrupts(clear_mask.data());
    }

    // Check for hard reset (bit 1)
    if (hw.pdController.isInterruptSet(events, 1)) {
        LOG_WARN("PD Hard Reset received");

        // Invalidate everything
        _pdos_valid = false;
        _active_contract.valid = false;
        _active_contract.is_pps = false;
        _active_contract.is_avs = false;
        _active_contract.programmable_min_mv = 0;
        _active_contract.programmable_max_mv = 0;
        _negotiation_state = NegotiationState::IDLE;
        clearPpsTracking();
        clearAvsTracking();
        clearRequestedContract();
        _pd_revision[0] = '\0';
        clearChargerIdentity();
        _epr_exit_state = EprExitState::NONE;

        // Clear the interrupt
        const auto clear_mask = makeInterruptClearMask(1);
        hw.pdController.clearInterrupts(clear_mask.data());
    }
}

bool PdManager::isRequestedContractReached() const {
    if (!_active_contract.valid) {
        return false;
    }

    auto within_tolerance = [](uint32_t lhs, uint32_t rhs, uint32_t tolerance_mv) {
        return (lhs > rhs) ? (lhs - rhs <= tolerance_mv) : (rhs - lhs <= tolerance_mv);
    };

    switch (_requested_contract_type) {
        case RequestedContractType::FIXED:
            return !_active_contract.is_pps && !_active_contract.is_avs &&
                   within_tolerance(_active_contract.voltage_mv, _requested_voltage_mv,
                                    FIXED_MATCH_TOLERANCE_MV);

        case RequestedContractType::PPS:
            return _active_contract.is_pps &&
                   within_tolerance(_active_contract.voltage_mv, _requested_voltage_mv,
                                    PPS_MATCH_TOLERANCE_MV);

        case RequestedContractType::AVS:
            // During EPR exit step-down, some chargers satisfy the 15V floor using an
            // overlapping fixed contract instead of reporting the AVS APDO we asked
            // for. That is still sufficient for the next exit step because VBUS is
            // already back in the safe SPR range.
            if (_epr_exit_state == EprExitState::STEPPING_DOWN) {
                return within_tolerance(_active_contract.voltage_mv, _requested_voltage_mv,
                                        AVS_MATCH_TOLERANCE_MV);
            }
            return _active_contract.is_avs &&
                   within_tolerance(_active_contract.voltage_mv, _requested_voltage_mv,
                                    AVS_MATCH_TOLERANCE_MV);

        case RequestedContractType::NONE:
        default:
            return false;
    }
}

bool PdManager::hasPendingRequestedContract() const {
    return _requested_contract_type != RequestedContractType::NONE &&
           !isRequestedContractReached();
}

bool PdManager::isRequestedContractSatisfied() const {
    return _requested_contract_type == RequestedContractType::NONE ||
           isRequestedContractReached();
}

bool PdManager::isPpsTuningActive() const {
    return _pps_active && settings.isAutoPpsEnabled() && _pps_user_target_mv > 0;
}

bool PdManager::isAvsTuningActive() const {
    return _avs_active && settings.isAutoAvsEnabled() && _avs_user_target_mv > 0;
}

void PdManager::checkTuningConvergenceImmediate() {
    float measured_v;
    if (gpio_get(Board::PIN_SWITCH_EN)) {
        measured_v = hw.powerMonitor.getBusVoltage();
    } else {
        measured_v = hw.adc.getVBUS();
    }
    uint32_t measured_mv = (uint32_t)(measured_v * 1000.0f);

    if (measured_mv > MIN_TUNING_VOLTAGE_MV) {
        if (_pps_active && _pps_user_target_mv > 0) {
            int32_t error = (int32_t)_pps_user_target_mv - (int32_t)measured_mv;
            _pps_tuning_converged = (abs(error) <= PPS_TUNE_THRESHOLD_MV);
        }
        if (_avs_active && _avs_user_target_mv > 0) {
            int32_t error = (int32_t)_avs_user_target_mv - (int32_t)measured_mv;
            _avs_tuning_converged = (abs(error) <= AVS_TUNE_THRESHOLD_MV);
        }
    }
}

// ============================================================================
// Startup Contract Negotiation
// ============================================================================

bool PdManager::waitForPdos(uint32_t timeout_ms) {
    static absolute_time_t wait_start = nil_time;

    if (is_nil_time(wait_start)) {
        wait_start = get_absolute_time();
    }

    uint32_t elapsed_ms = absolute_time_diff_us(wait_start, get_absolute_time()) / 1000;

    // Check if PDOs are already available
    if (!_pdos_valid) {
        _pdo_count = hw.pdController.getSourceCapabilities(_pdo_cache, AppConfig::MAX_PDO_COUNT);
        _pdos_valid = (_pdo_count > 0);

        if (_pdos_valid) {
            detectPdRevision();
            refreshActiveContract();
            LOG_INFO("Initial PDO discovery: %d PDOs found", _pdo_count);
            wait_start = nil_time;
            return true; // We found PDOs! Done!
        }
    }

    // Check timeout
    if (elapsed_ms >= timeout_ms) {
        LOG_WARN("PDO discovery timed out after %ums", elapsed_ms);
        clearPpsTracking();
        clearAvsTracking();
        clearRequestedContract();
        _pd_revision[0] = '\0';
        refreshActiveContract();
        wait_start = nil_time;
        return true;  // Return true to stop waiting
    }

    return false;  // Still waiting
}

bool PdManager::negotiateStartupContract(bool allow_epr_wait) {
    _startup_restore_waiting_for_epr = false;

    // Nothing to negotiate if no PDOs available
    if (!_pdos_valid || _pdo_count == 0) {
        LOG_DEBUG("No PDOs available for startup negotiation");
        return false;
    }

    StartupContractMode mode = settings.getStartupNegotiationMode();

    if (mode == StartupContractMode::HIGHEST_VOLTAGE) {
        // TPS26750 automatically negotiates highest voltage due to its EEPROM config.
        // We don't interfere. Doing so breaks autonomous EPR entry sequences.
        LOG_INFO("Startup negotiation: Highest voltage - letting TPS26750 auto-negotiate");
        return false;
    }

    // Build list of fixed/AVS PDOs sorted by voltage for selection
    int8_t lowest_idx = -1;
    uint32_t lowest_voltage = UINT32_MAX;

    for (uint8_t i = 0; i < _pdo_count; i++) {
        if (!_pdo_cache[i].is_pps) {
            uint32_t voltage = _pdo_cache[i].voltage_mv;
            if (voltage < lowest_voltage) {
                lowest_voltage = voltage;
                lowest_idx = i;
            }
        }
    }

    int8_t target_idx = -1;
    uint32_t target_request_voltage_mv = 0;

    switch (mode) {
        case StartupContractMode::LOWEST_VOLTAGE:
            target_idx = lowest_idx;
            if (target_idx >= 0) {
                LOG_INFO("Startup negotiation: Lowest voltage - %umV (PDO[%d])",
                         lowest_voltage, target_idx);
            }
            break;

        case StartupContractMode::LAST_USED: {
            SavedStartupContractSnapshot snapshot = getSavedStartupContractSnapshot();
            char snapshot_desc[96];
            describeSavedStartupContract(snapshot, snapshot_desc, sizeof(snapshot_desc));
            LOG_INFO("Startup negotiation: saved snapshot %s", snapshot_desc);

            if (snapshot.type == SavedStartupContractType::NONE) {
                LOG_INFO("Startup negotiation: No saved contract snapshot, keeping default");
                return false;
            }

            uint32_t target_voltage_mv = getSavedTargetVoltageMv(snapshot, _pdo_cache, _pdo_count);
            if (target_voltage_mv == 0) {
                LOG_WARN("Startup negotiation: Saved snapshot has no target voltage, keeping default");
                return false;
            }

            bool target_prefers_epr_retry = snapshotPrefersEprRetry(snapshot);
            bool epr_visible = hasVisibleEprPdos(_pdo_cache, _pdo_count);

            if (snapshot.pdo_index_hint >= 0) {
                if (snapshot.pdo_index_hint >= _pdo_count) {
                    LOG_INFO("Startup negotiation: saved PDO hint PDO[%d] is out of range on this charger (%d PDOs)",
                             snapshot.pdo_index_hint, _pdo_count);
                } else {
                    const SourceCapability& hinted_pdo = _pdo_cache[snapshot.pdo_index_hint];
                    char hinted_desc[64];
                    describeSourceCapability(hinted_pdo, hinted_desc, sizeof(hinted_desc));
                    LOG_INFO("Startup negotiation: charger PDO[%d] is %s",
                             snapshot.pdo_index_hint, hinted_desc);

                    if (snapshot.type == SavedStartupContractType::UNKNOWN) {
                        if ((hinted_pdo.is_pps || hinted_pdo.is_avs) &&
                            target_voltage_mv >= hinted_pdo.min_voltage_mv &&
                            target_voltage_mv <= hinted_pdo.voltage_mv) {
                            target_idx = snapshot.pdo_index_hint;
                            target_request_voltage_mv = hinted_pdo.is_pps ?
                                PdVoltage::alignDown(target_voltage_mv, AppConfig::PPS_VOLTAGE_STEP_MV) :
                                PdVoltage::alignDown(target_voltage_mv, AppConfig::AVS_VOLTAGE_STEP_MV);
                            LOG_INFO("Startup negotiation: using legacy snapshot hint PDO[%d] to restore %s at %umV",
                                     snapshot.pdo_index_hint,
                                     sourceCapabilityTypeName(hinted_pdo),
                                     target_request_voltage_mv);
                        } else {
                            target_idx = snapshot.pdo_index_hint;
                            target_request_voltage_mv = hinted_pdo.voltage_mv;
                            LOG_INFO("Startup negotiation: using legacy snapshot hint PDO[%d] directly as %s",
                                     snapshot.pdo_index_hint,
                                     hinted_desc);
                        }
                    } else if (!sourceCapabilityMatchesSavedType(hinted_pdo, snapshot.type)) {
                        LOG_INFO("Startup negotiation: saved hint type mismatch, expected %s but charger PDO[%d] is %s",
                                 savedStartupContractTypeName(snapshot.type),
                                 snapshot.pdo_index_hint,
                                 sourceCapabilityTypeName(hinted_pdo));
                    } else if (isProgrammableSavedType(snapshot.type)) {
                        if (target_voltage_mv >= hinted_pdo.min_voltage_mv &&
                            target_voltage_mv <= hinted_pdo.voltage_mv) {
                            target_idx = snapshot.pdo_index_hint;
                            target_request_voltage_mv = hinted_pdo.is_pps ?
                                PdVoltage::alignDown(target_voltage_mv, AppConfig::PPS_VOLTAGE_STEP_MV) :
                                PdVoltage::alignDown(target_voltage_mv, AppConfig::AVS_VOLTAGE_STEP_MV);
                            LOG_INFO("Startup negotiation: exact %s restore match on PDO[%d], target %umV within %u-%umV",
                                     savedStartupContractTypeName(snapshot.type),
                                     snapshot.pdo_index_hint,
                                     target_request_voltage_mv,
                                     hinted_pdo.min_voltage_mv,
                                     hinted_pdo.voltage_mv);
                        } else {
                            LOG_INFO("Startup negotiation: saved %s target %umV is outside charger PDO[%d] range %u-%umV",
                                     savedStartupContractTypeName(snapshot.type),
                                     target_voltage_mv,
                                     snapshot.pdo_index_hint,
                                     hinted_pdo.min_voltage_mv,
                                     hinted_pdo.voltage_mv);
                        }
                    } else if (hinted_pdo.voltage_mv == target_voltage_mv) {
                        target_idx = snapshot.pdo_index_hint;
                        target_request_voltage_mv = hinted_pdo.voltage_mv;
                        LOG_INFO("Startup negotiation: exact fixed restore match on PDO[%d] at %umV",
                                 snapshot.pdo_index_hint,
                                 hinted_pdo.voltage_mv);
                    } else {
                        LOG_INFO("Startup negotiation: saved fixed target %umV does not match charger PDO[%d] at %umV",
                                 target_voltage_mv,
                                 snapshot.pdo_index_hint,
                                 hinted_pdo.voltage_mv);
                    }
                }
            }

            if (target_idx < 0) {
                if (allow_epr_wait && target_prefers_epr_retry && !epr_visible) {
                    _startup_restore_waiting_for_epr = true;
                    LOG_INFO("Startup negotiation: saved %s target %umV may require EPR PDOs, waiting for EPR discovery before fallback",
                             savedStartupContractTypeName(snapshot.type),
                             target_voltage_mv);
                    return false;
                }

                StartupMatchResult same_type_match{false, -1, 0, UINT32_MAX};
                if (snapshot.type != SavedStartupContractType::UNKNOWN) {
                    same_type_match = findBestStartupMatch(snapshot,
                                                          _pdo_cache,
                                                          _pdo_count,
                                                          target_voltage_mv,
                                                          true);
                }

                if (same_type_match.valid) {
                    target_idx = same_type_match.pdo_index;
                    target_request_voltage_mv = same_type_match.requested_voltage_mv;
                    const SourceCapability& candidate = _pdo_cache[target_idx];
                    LOG_INFO("Startup negotiation: same-type fallback selected %s PDO[%d] at %umV (delta %umV from saved target %umV)",
                             sourceCapabilityTypeName(candidate),
                             target_idx,
                             target_request_voltage_mv,
                             same_type_match.diff_mv,
                             target_voltage_mv);
                } else {
                    if (snapshot.type != SavedStartupContractType::UNKNOWN) {
                        LOG_INFO("Startup negotiation: no %s candidates available for saved target %umV",
                                 savedStartupContractTypeName(snapshot.type),
                                 target_voltage_mv);
                    }

                    if (allow_epr_wait && target_prefers_epr_retry && !epr_visible) {
                            _startup_restore_waiting_for_epr = true;
                            LOG_INFO("Startup negotiation: saved %s target %umV may require EPR PDOs, waiting for EPR discovery before cross-type fallback",
                                     savedStartupContractTypeName(snapshot.type),
                                     target_voltage_mv);
                            return false;
                    }

                    StartupMatchResult fallback_match = findBestStartupMatch(snapshot,
                                                                            _pdo_cache,
                                                                            _pdo_count,
                                                                            target_voltage_mv,
                                                                            false);
                    if (!fallback_match.valid) {
                        LOG_WARN("Startup negotiation: no fallback candidate found for saved target %umV, keeping default",
                                 target_voltage_mv);
                        return false;
                    }

                    target_idx = fallback_match.pdo_index;
                    target_request_voltage_mv = fallback_match.requested_voltage_mv;
                    const SourceCapability& candidate = _pdo_cache[target_idx];
                    LOG_INFO("Startup negotiation: cross-type fallback selected %s PDO[%d] at %umV (delta %umV from saved target %umV)",
                             sourceCapabilityTypeName(candidate),
                             target_idx,
                             target_request_voltage_mv,
                             fallback_match.diff_mv,
                             target_voltage_mv);
                }
            }
            break;
        }
    }

    // Execute the negotiation
    if (target_idx >= 0 && target_idx < _pdo_count) {
        const SourceCapability& pdo = _pdo_cache[target_idx];

        if (pdo.is_pps && target_request_voltage_mv > 0) {
            LOG_INFO("Startup negotiation: requesting PPS %umV using PDO[%d]",
                     target_request_voltage_mv, target_idx);
            return requestPpsVoltage(target_request_voltage_mv, pdo.max_current_ma, target_idx);
        } else if (pdo.is_avs && target_request_voltage_mv > 0) {
            LOG_INFO("Startup negotiation: requesting AVS %umV using PDO[%d]",
                     target_request_voltage_mv, target_idx);
            return requestAvsVoltage(target_request_voltage_mv, pdo.max_current_ma, target_idx);
        } else {
            if (_active_contract.valid && !_active_contract.is_pps && !_active_contract.is_avs &&
                _active_contract.voltage_mv == pdo.voltage_mv) {
                LOG_INFO("Startup negotiation: fixed target %umV is already active from the pre-boot request; refreshing with PDO[%d] current data",
                         pdo.voltage_mv, target_idx);
            }
            return requestContract(pdo);
        }
    }

    return false;
}

void PdManager::probeEpr() {
    bool has_epr = false;
    for (uint8_t i = 0; i < _pdo_count; i++) {
        // Only count a PDO as EPR if its voltage (or max voltage for APDOs) exceeds
        // the SPR ceiling. SPR AVS has voltage_mv == EPR_SPR_MAX_MV (20 V exactly),
        // so the strict > test correctly excludes it while matching EPR fixed PDOs
        // (28 V / 36 V / 48 V) and EPR AVS (max > 20 V).
        if (_pdo_cache[i].voltage_mv > AppConfig::EPR_SPR_MAX_MV) {
            has_epr = true;
            break;
        }
    }
    
    // Request EPR capabilities if we haven't received them yet
    if (!has_epr && _pdo_count > 0) {
        LOG_INFO("Probing for EPR capabilities...");
        hw.pdController.sendCommand(TPS_CMD_ESrC);
    }
}