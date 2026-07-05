#include "pd_manager.h"
#include "hardware.h"
#include "config/board_config.h"
#include "interrupts.h"
#include "logic/cc_controller.h"
#include "logic/settings.h"
#include "logic/pd_diagnostics.h"
#include "utils/logging.h"
#include "utils/pd_voltage.h"
#include <array>
#include <cstring>
#include <cstdlib>  // abs()

// Re-export the pure PD domain logic extracted to pd_diagnostics.h (P4-testable)
// so the call sites below stay unqualified.
using namespace PdDiagnostics;

namespace {

constexpr uint8_t GPPI_RESPONSE_READ_BYTES = 32;
constexpr uint8_t TPS_INTERRUPT_REGISTER_BYTES = 11;

// Absolute difference within tolerance (unsigned-safe). Shared by the PPS/AVS
// tracking checks below.
inline bool withinTolerance(uint32_t lhs, uint32_t rhs, uint32_t tolerance_mv) {
    return (lhs > rhs) ? (lhs - rhs <= tolerance_mv) : (rhs - lhs <= tolerance_mv);
}

std::array<uint8_t, TPS_INTERRUPT_REGISTER_BYTES> makeInterruptClearMask(uint8_t bit_index) {
    std::array<uint8_t, TPS_INTERRUPT_REGISTER_BYTES> clear_mask{};
    if (bit_index < (TPS_INTERRUPT_REGISTER_BYTES * 8)) {
        clear_mask[bit_index / 8] = static_cast<uint8_t>(1u << (bit_index % 8));
    }
    return clear_mask;
}

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

const char* sourceCapabilityTypeName(const TPS26750_SourceCapability& pdo) {
    if (pdo.is_avs) return "AVS";
    if (pdo.is_pps) return "PPS";
    return "FIXED";
}

bool isProgrammableSavedType(SavedStartupContractType type) {
    return type == SavedStartupContractType::PPS || type == SavedStartupContractType::AVS;
}

uint32_t getSavedTargetVoltageMv(const SavedStartupContractSnapshot& snapshot,
                                 const TPS26750_SourceCapability* pdos,
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

bool hasVisibleEprPdos(const TPS26750_SourceCapability* pdos, uint8_t count) {
    for (uint8_t i = 0; i < count; i++) {
        if (pdos[i].voltage_mv > AppConfig::EPR_SPR_MAX_MV) {
            return true;
        }
    }

    return false;
}

bool matchesFixedPdo(const TPS26750_SourceCapability* pdos,
                     uint8_t count,
                     uint32_t voltage_mv,
                     uint32_t tolerance_mv) {
    for (uint8_t i = 0; i < count; i++) {
        const TPS26750_SourceCapability& pdo = pdos[i];
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
        snprintf(buffer, buffer_size, "%s target=%lumV range=%lu-%lumV hint=PDO[%d]",
                 savedStartupContractTypeName(snapshot.type),
                 snapshot.requested_voltage_mv,
                 snapshot.range_min_voltage_mv,
                 snapshot.range_max_voltage_mv,
                 snapshot.pdo_index_hint);
        return;
    }

    snprintf(buffer, buffer_size, "%s target=%lumV hint=PDO[%d]",
             savedStartupContractTypeName(snapshot.type),
             snapshot.requested_voltage_mv,
             snapshot.pdo_index_hint);
}

void describeSourceCapability(const TPS26750_SourceCapability& pdo,
                              char* buffer,
                              size_t buffer_size) {
    if (pdo.is_pps || pdo.is_avs) {
        snprintf(buffer, buffer_size, "%s %lu-%lumV @ %lumA",
                 sourceCapabilityTypeName(pdo),
                 pdo.min_voltage_mv,
                 pdo.voltage_mv,
                 pdo.max_current_ma);
        return;
    }

    snprintf(buffer, buffer_size, "%s %lumV @ %lumA",
             sourceCapabilityTypeName(pdo),
             pdo.voltage_mv,
             pdo.max_current_ma);
}

uint32_t getSourceCapabilityMaxPowerW(const TPS26750_SourceCapability& pdo) {
    if (pdo.is_avs && pdo.max_current_9_15_ma > 0) {
        uint32_t low_band_power_w = powerWatts(15000, pdo.max_current_9_15_ma);
        uint32_t high_band_power_w = powerWatts(pdo.voltage_mv, pdo.max_current_ma);
        return (high_band_power_w > low_band_power_w) ? high_band_power_w : low_band_power_w;
    }

    return powerWatts(pdo.voltage_mv, pdo.max_current_ma);
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
    , _pps{ /*is_avs*/ false, AppConfig::PPS_VOLTAGE_STEP_MV, /*refresh_ms*/ 7000,
            /*tune_threshold_mv*/ 12, /*tune_max_correction_mv*/ 500,
            /*tracking_tolerance_mv*/ 150,  // Allow normal PPS droop without dropping Vset/tuning state
            false, 0, 0, nil_time, 0, 0, false, 0, 0 }
    , _avs{ /*is_avs*/ true, AppConfig::AVS_VOLTAGE_STEP_MV, /*refresh_ms*/ 7000,
            /*tune_threshold_mv*/ 55,  // Converged when error < half a step (100mV steps)
            /*tune_max_correction_mv*/ 500,
            /*tracking_tolerance_mv*/ 250,  // Allow normal AVS regulation error without dropping state
            false, 0, 0, nil_time, 0, 0, false, 0, 0 }
    , _pre_request_voltage_mv(0)
    , _pre_request_current_ma(0)
    , _requested_contract_type(RequestedContractType::NONE)
    , _requested_voltage_mv(0)
    , _requested_current_ma(0)
    , _active_pdo_index(-1)
    , _startup_restore_waiting_for_epr(false)
    , _next_pdo_retry(nil_time)
    , _next_pps_convergence_check(nil_time)
    , _next_avs_convergence_check(nil_time)
    , _next_contract_refresh(nil_time)
    , _wait_pdos_start(nil_time)
    , _epr_exit_state(EprExitState::NONE)
    , _epr_deferred_voltage_mv(0)
    , _epr_deferred_current_ma(0)
    , _epr_deferred_contract_type(RequestedContractType::NONE)
    , _epr_deferred_pdo_index(-1)
    , _epr_exit_start(nil_time)
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
                    LOG_INFO("Contract change detected via polling: %lumV @ %lumA",
                             current_voltage_mv, current_current_ma);
                }

                refreshActiveContract();

                if (isRequestedContractReached()) {
                    _negotiation_state = NegotiationState::SUCCESS;
                } else if (contract_changed_from_pre_request && contract_changed_from_cached) {
                    LOG_INFO("Startup request still pending: holding %lumV @ %lumA while waiting for requested %lumV",
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
            LOG_ERROR("EPR exit sequence timed out after %lums -- aborting", epr_elapsed);
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
            LOG_INFO("EPR step 1/3 complete: AVS at %lumV. Requesting 5V Fixed to exit EPR",
                     _active_contract.voltage_mv);
            _epr_exit_state = EprExitState::REQUESTING_5V;
            // Request 5V Fixed -- bypass EPR interception by setting state first
            _pre_request_voltage_mv = _active_contract.voltage_mv;
            _pre_request_current_ma = _active_contract.current_ma;
            // Deactivate AVS/PPS tracking for the intermediate 5V request
            _avs.active = false;
            _avs.voltage_mv = 0;
            _avs.current_ma = 0;
            _pps.active = false;
            _pps.voltage_mv = 0;
            _pps.current_ma = 0;
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
            LOG_INFO("EPR step 2/3 complete: at %lumV (SPR). Requesting target: %s %lumV @ %lumA",
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
            LOG_INFO("EPR step 3/3 complete: safely transitioned to %lumV @ %lumA",
                     _active_contract.voltage_mv, _active_contract.current_ma);
            _epr_exit_state = EprExitState::NONE;
        }
    }

    // Deferred PDO discovery: if PD revision is unknown, TPS26750 may have negotiated
    // before RP2040 GPIO interrupts were set up (missed edge at cold boot).
    // Retry every 500ms until PDOs are found.
    if (_pd_revision[0] == '\0' && _charger_connected) {
        if (absolute_time_diff_us(_next_pdo_retry, get_absolute_time()) >= 0) {
            _next_pdo_retry = make_timeout_time_ms(PDO_RETRY_INTERVAL_MS);
            _pdo_count = hw.pdController.getSourceCapabilities(_pdo_cache, AppConfig::MAX_PDO_COUNT);
            _pdos_valid = (_pdo_count > 0);
            if (_pdos_valid) {
                detectPdRevision();
                refreshActiveContract();
                LOG_INFO("Deferred PDO discovery: found %d PDOs", _pdo_count);
            }
        }
    }

    // PPS/AVS keep-alive: must refresh the contract every <10 seconds or the source
    // reverts to 5V. One shared routine services both programmable contract types.
    serviceKeepAlive(_pps);

    // Fast convergence check: when PPS tuning is active,
    // check frequently (every 500ms) to detect drift in either direction
    if (_pps.active && settings.isAutoPpsEnabled() && _pps.user_target_mv > 0) {
        if (absolute_time_diff_us(_next_pps_convergence_check, get_absolute_time()) >= 0) {
            _next_pps_convergence_check = make_timeout_time_ms(TUNE_CONVERGENCE_CHECK_MS);
            checkTuningConvergenceImmediate();
        }
    }

    // AVS keep-alive: EPR contracts also need periodic re-request to maintain the contract
    serviceKeepAlive(_avs);

    // Fast convergence check: when AVS tuning is active,
    // check frequently (every 500ms) to detect drift in either direction
    if (_avs.active && settings.isAutoAvsEnabled() && _avs.user_target_mv > 0) {
        if (absolute_time_diff_us(_next_avs_convergence_check, get_absolute_time()) >= 0) {
            _next_avs_convergence_check = make_timeout_time_ms(TUNE_CONVERGENCE_CHECK_MS);
            checkTuningConvergenceImmediate();
        }
    }

    // Periodic contract refresh for display sync (every 1 second)
    // Ensures displayed contract matches actual state after EPR transitions
    if (absolute_time_diff_us(_next_contract_refresh, get_absolute_time()) >= 0) {
        _next_contract_refresh = make_timeout_time_ms(CONTRACT_REFRESH_INTERVAL_MS);

        uint32_t old_voltage = _active_contract.voltage_mv;
        refreshActiveContract();

        // If voltage changed significantly, log it (display updates naturally via overwrite rendering)
        if (_active_contract.voltage_mv != old_voltage) {
            LOG_INFO("Contract voltage changed: %lumV -> %lumV",
                     old_voltage, _active_contract.voltage_mv);
        }
    }
}

// ============================================================================
// PDO Access
// ============================================================================

uint8_t PdManager::getPdoCount() {
    // Refresh cache if needed
    if (!_pdos_valid) {
        _pdo_count = hw.pdController.getSourceCapabilities(_pdo_cache, AppConfig::MAX_PDO_COUNT);
        _pdos_valid = (_pdo_count > 0);
        if (_pdos_valid) detectPdRevision();
    }
    return _pdo_count;
}

uint8_t PdManager::getSourceCapabilities(TPS26750_SourceCapability* caps, uint8_t max_caps) {
    uint8_t available = getPdoCount();  // refresh cache + get count

    // Copy from cache
    uint8_t count = (available < max_caps) ? available : max_caps;
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
    info.charger_power_is_upper_bound = false;

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
        info.cc_orientation = (status_buf[0] & TPS26750_STATUS_ORIENTATION) ? 2 : 1;
    }

    bool has_pps = false;

    for (uint8_t i = 0; i < _pdo_count; i++) {
        const TPS26750_SourceCapability& pdo = _pdo_cache[i];
        uint32_t pdo_power_w = getSourceCapabilityMaxPowerW(pdo);
        if (pdo_power_w > info.charger_max_power_w) {
            info.charger_max_power_w = pdo_power_w;
        }

        if (pdo.is_pps) {
            has_pps = true;
        }
    }

    // Non-PD (Type-C only) source: no source-capability PDOs are advertised, so
    // fall back to the advertised Type-C current (Rp level) in POWER_STATUS.
    //   - Rp-1.5A / Rp-3.0A: the source guarantees that current at 5V -> exact W.
    //   - Rp-default: the source advertises no current over CC; its real capability
    //     (often signaled out-of-band via BC1.2) is unknowable here, so report the
    //     true ceiling for any 5V Type-C source (15W) as an upper bound.
    if (info.charger_max_power_w == 0) {
        uint8_t pwr_status[2] = {0};
        if (hw.pdController.getPowerStatus(pwr_status)) {
            switch (pwr_status[0] & TPS26750_PWR_STATUS_TYPEC_CURR_MASK) {
                case TPS26750_PWR_STATUS_TYPEC_3_0A:
                    info.charger_max_power_w = 15;  // 5V @ 3.0A
                    break;
                case TPS26750_PWR_STATUS_TYPEC_1_5A:
                    info.charger_max_power_w = 7;   // 5V @ 1.5A
                    break;
                case TPS26750_PWR_STATUS_TYPEC_USB_DEF:
                    info.charger_max_power_w = 15;  // 5V Type-C ceiling
                    info.charger_power_is_upper_bound = true;
                    break;
                default:
                    break;  // PD contract advertised but no PDOs cached yet
            }
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
    bool charger_ok = hw.pdController.getManufacturerInfo(TPS26750_GPPI_FRAME_SOP,
                                                          charger_response,
                                                          GPPI_RESPONSE_READ_BYTES,
                                                          &charger_response_len);
    if (!charger_ok) {
        return false;
    }

    uint16_t charger_vendor_id = 0;
    uint16_t charger_product_id = 0;
    char charger_name[32] = {0};
    // Clamp the controller-reported length to the actual buffer size so a bogus
    // length can't drive an out-of-bounds read inside the decoder.
    uint8_t response_len = static_cast<uint8_t>(
        charger_response_len > GPPI_RESPONSE_READ_BYTES ? GPPI_RESPONSE_READ_BYTES
                                                        : charger_response_len);
    if (!decodeManufacturerInfoResponse(charger_response,
                                        response_len,
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

void PdManager::clearTracking(ProgrammableContract& c) {
    c.active = false;
    c.voltage_mv = 0;
    c.current_ma = 0;
    c.user_target_mv = 0;
    c.correction_mv = 0;
    c.tuning_converged = false;
    c.range_min_mv = 0;
    c.range_max_mv = 0;
    _active_pdo_index = -1;
}

// Service one programmable (PPS or AVS) contract's keep-alive + auto-tune cycle.
// The two contract types differ only in their configuration (step, thresholds,
// refresh interval), which settings flag gates auto-tuning, and which TPS26750
// profile request they issue -- all captured by the ProgrammableContract struct.
void PdManager::serviceKeepAlive(ProgrammableContract& c) {
    if (!(c.active && c.voltage_mv > 0)) {
        return;
    }

    const bool auto_enabled = c.is_avs ? settings.isAutoAvsEnabled() : settings.isAutoPpsEnabled();
    const char* const label = c.is_avs ? "AVS" : "PPS";

    uint32_t elapsed_ms = absolute_time_diff_us(c.last_refresh, get_absolute_time()) / 1000;
    uint32_t refresh_interval_ms = c.refresh_ms;
    if (auto_enabled && c.user_target_mv > 0 &&
        !c.tuning_converged && !CcController::isRegulating()) {
        refresh_interval_ms = TUNE_REQUEST_INTERVAL_MS;
    }

    if (elapsed_ms < refresh_interval_ms) {
        return;
    }

    uint32_t request_mv = c.voltage_mv;

    // Auto tuning: measure actual voltage and adjust the request.
    // Skip auto-tuning when CC controller is regulating (CC adjusts voltage for current).
    if (auto_enabled && c.user_target_mv > 0 && !CcController::isRegulating()) {
        // Measure actual output voltage
        float measured_v;
        if (gpio_get(Board::PIN_SWITCH_EN)) {
            measured_v = hw.powerMonitor.getBusVoltage();  // Post-switch (accurate)
        } else {
            measured_v = hw.adc.getVBUS();  // Pre-switch (fallback)
        }
        uint32_t measured_mv = (uint32_t)(measured_v * 1000.0f);

        // Only tune if we have a valid measurement (> 1V, likely the source is delivering)
        if (measured_mv > MIN_TUNING_VOLTAGE_MV) {
            int32_t error = (int32_t)c.user_target_mv - (int32_t)measured_mv;

            if (abs(error) > c.tune_threshold_mv) {
                // Accumulate correction
                c.correction_mv += error;

                // Clamp correction to safety limit
                if (c.correction_mv > c.tune_max_correction_mv)
                    c.correction_mv = c.tune_max_correction_mv;
                if (c.correction_mv < -c.tune_max_correction_mv)
                    c.correction_mv = -c.tune_max_correction_mv;

                c.tuning_converged = false;
                LOG_DEBUG("%s tune: target=%lumV measured=%lumV error=%ldmV correction=%ldmV",
                          label, c.user_target_mv, measured_mv, error, c.correction_mv);
            } else {
                c.tuning_converged = true;
            }
        }

        // Compute adjusted request voltage
        int32_t adjusted = (int32_t)c.user_target_mv + c.correction_mv;

        // Clamp to APDO range
        if (c.range_max_mv > 0) {
            if (adjusted < (int32_t)c.range_min_mv) adjusted = (int32_t)c.range_min_mv;
            if (adjusted > (int32_t)c.range_max_mv) adjusted = (int32_t)c.range_max_mv;
        }

        // Align to the request step before re-requesting the contract.
        adjusted = static_cast<int32_t>(PdVoltage::alignDown(
            static_cast<uint32_t>(adjusted), c.step_mv));

        request_mv = (uint32_t)adjusted;
    }

    LOG_DEBUG("%s keep-alive: requesting %lumV @ %lumA", label, request_mv, c.current_ma);

    bool ok = c.is_avs
        ? hw.pdController.requestAVSProfile(request_mv, c.current_ma, c.range_min_mv, c.range_max_mv)
        : hw.pdController.requestPPSProfile(request_mv, c.current_ma, c.range_min_mv, c.range_max_mv);
    if (ok) {
        c.last_refresh = get_absolute_time();
        c.voltage_mv = request_mv;  // Track what we actually requested
    } else {
        LOG_WARN("%s keep-alive request failed", label);
    }
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
        LOG_INFO("Startup pre-boot request: blindly requesting saved fixed contract %lumV before PDO discovery",
                 saved_target_mv);
        return requestFixedVoltage(saved_target_mv, startup_current_ma);
    }

    if (saved_target_mv > AppConfig::EPR_SPR_MAX_MV) {
        LOG_INFO("Startup pre-boot request: blindly requesting saved EPR target %lumV before PDO discovery",
                 saved_target_mv);
        return requestAvsVoltage(saved_target_mv, startup_current_ma);
    }

    if (snapshot.type == SavedStartupContractType::PPS && saved_target_mv < 5000) {
        LOG_INFO("Startup pre-boot request: blindly requesting saved low-PPS target %lumV before PDO discovery",
                 saved_target_mv);
        return requestPpsVoltage(saved_target_mv, startup_current_ma);
    }

    if (snapshot.type == SavedStartupContractType::AVS) {
        LOG_INFO("Startup pre-boot request: clamping to 5V until PDO discovery can restore AVS target %lumV",
                 saved_target_mv);
    } else if (snapshot.type == SavedStartupContractType::PPS) {
        LOG_INFO("Startup pre-boot request: clamping to 5V until PDO discovery can restore PPS target %lumV",
                 saved_target_mv);
    } else {
        LOG_INFO("Startup pre-boot request: clamping to 5V until PDO discovery can restore programmable target %lumV",
                 saved_target_mv);
    }

    return requestFixedVoltage(5000, startup_current_ma);
}

bool PdManager::requestContract(const TPS26750_SourceCapability& pdo) {
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
    LOG_INFO("Requesting Fixed: %lumV @ %lumA", voltage_mv, current_ma);

    // EPR safe exit: if currently in EPR and target is SPR, initiate 3-step exit
    if (_epr_exit_state == EprExitState::NONE && needsEprExit(voltage_mv, false)) {
        uint32_t avs_v, avs_i;
        int8_t avs_idx = -1;
        if (findAvsSafeVoltage(avs_v, avs_i, avs_idx)) {
            LOG_INFO("EPR exit: 3-step sequence for Fixed %lumV (AVS %lumV -> 5V -> %lumV)",
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
        clearTracking(_pps);

        // Deactivate AVS mode when switching to fixed
        clearTracking(_avs);
    } else {
        _negotiation_state = NegotiationState::FAILED;
        LOG_ERROR("Failed to send Fixed contract request");
    }

    return success;
}

bool PdManager::requestPpsVoltage(uint32_t voltage_mv, uint32_t current_ma, int8_t pdo_index) {
    LOG_INFO("Requesting PPS: %lumV @ %lumA (PDO index %d)", voltage_mv, current_ma, pdo_index);

    // EPR safe exit: if currently in EPR and target is PPS (SPR), initiate 3-step exit
    if (_epr_exit_state == EprExitState::NONE && needsEprExit(voltage_mv, true)) {
        uint32_t avs_v, avs_i;
        int8_t avs_idx = -1;
        if (findAvsSafeVoltage(avs_v, avs_i, avs_idx)) {
            LOG_INFO("EPR exit: 3-step sequence for PPS %lumV (AVS %lumV -> 5V -> PPS %lumV)",
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

    // Broad defaults (3.3-21V) when no matching PPS APDO is found, so the request still goes through.
    return requestProgrammable(_pps, _avs, voltage_mv, current_ma, pdo_index,
                               RequestedContractType::PPS, 3300, 21000);
}

bool PdManager::requestAvsVoltage(uint32_t voltage_mv, uint32_t current_ma, int8_t pdo_index) {
    LOG_INFO("Requesting AVS: %lumV @ %lumA (PDO index %d)", voltage_mv, current_ma, pdo_index);

    // Mirror the fixed-request EPR exit guard here as well. A direct EPR->SPR
    // AVS transition can hard-reset the charger, so route it through the staged
    // AVS step-down sequence first.
    if (_epr_exit_state == EprExitState::NONE && needsEprExit(voltage_mv, false)) {
        uint32_t avs_v, avs_i;
        int8_t avs_idx = -1;
        if (findAvsSafeVoltage(avs_v, avs_i, avs_idx)) {
            LOG_INFO("EPR exit: 3-step sequence for AVS %lumV (AVS %lumV -> 5V -> AVS %lumV)",
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

    // Broad defaults (9-48V) when no matching AVS APDO is found, so the request still goes through.
    return requestProgrammable(_avs, _pps, voltage_mv, current_ma, pdo_index,
                               RequestedContractType::AVS, 9000, 48000);
}

// Shared core for requestPpsVoltage()/requestAvsVoltage(). `self` is the contract type
// being requested; `other` is the sibling type that gets deactivated. The EPR-safe-exit
// guard lives in the type-specific wrappers because its deferred type and log strings differ.
bool PdManager::requestProgrammable(ProgrammableContract& self, ProgrammableContract& other,
                                    uint32_t voltage_mv, uint32_t current_ma, int8_t pdo_index,
                                    RequestedContractType type,
                                    uint32_t default_min_mv, uint32_t default_max_mv) {
    const bool want_avs = self.is_avs;

    // Resolve APDO bounds: if a specific PDO index is provided, use it directly;
    // otherwise fall back to first-match search (startup restore, EPR exit paths).
    uint32_t pdo_min_mv = 0;
    uint32_t pdo_max_mv = 0;
    int8_t resolved_index = pdo_index;
    auto matchesType = [want_avs](const TPS26750_SourceCapability& p) {
        return want_avs ? p.is_avs : p.is_pps;
    };
    if (pdo_index >= 0 && pdo_index < _pdo_count && matchesType(_pdo_cache[pdo_index])) {
        pdo_min_mv = _pdo_cache[pdo_index].min_voltage_mv;
        pdo_max_mv = _pdo_cache[pdo_index].voltage_mv;
    } else {
        for (uint8_t i = 0; i < _pdo_count; i++) {
            if (matchesType(_pdo_cache[i]) &&
                voltage_mv >= _pdo_cache[i].min_voltage_mv &&
                voltage_mv <= _pdo_cache[i].voltage_mv) {
                pdo_min_mv = _pdo_cache[i].min_voltage_mv;
                pdo_max_mv = _pdo_cache[i].voltage_mv;
                resolved_index = (int8_t)i;
                break;
            }
        }
        if (pdo_max_mv == 0) {
            pdo_min_mv = default_min_mv;
            pdo_max_mv = default_max_mv;
        }
    }

    // Store pre-request contract for polling fallback
    _pre_request_voltage_mv = _active_contract.voltage_mv;
    _pre_request_current_ma = _active_contract.current_ma;
    _requested_contract_type = type;
    _requested_voltage_mv = voltage_mv;
    _requested_current_ma = current_ma;

    bool success = want_avs
        ? hw.pdController.requestAVSProfile(voltage_mv, current_ma, pdo_min_mv, pdo_max_mv)
        : hw.pdController.requestPPSProfile(voltage_mv, current_ma, pdo_min_mv, pdo_max_mv);

    if (success) {
        _negotiation_state = NegotiationState::REQUESTING;
        _negotiation_start = get_absolute_time();

        // Deactivate the sibling contract type when switching
        clearTracking(other);

        // Track this contract's state for keep-alive
        self.active = true;
        self.voltage_mv = voltage_mv;
        self.current_ma = current_ma;
        self.last_refresh = get_absolute_time();

        // Auto tuning: store user target and reset correction
        self.user_target_mv = voltage_mv;
        self.correction_mv = 0;
        self.tuning_converged = false;

        // Store resolved identity and bounds for keep-alive clamping and UI highlighting
        _active_pdo_index = resolved_index;
        self.range_min_mv = pdo_min_mv;
        self.range_max_mv = pdo_max_mv;
    } else {
        _negotiation_state = NegotiationState::FAILED;
        LOG_ERROR("Failed to send %s contract request", want_avs ? "AVS" : "PPS");
    }

    return success;
}

void PdManager::setCcKeepAliveVoltage(uint32_t voltage_mv) {
    if (_pps.active) {
        _pps.voltage_mv = voltage_mv;
        _pps.last_refresh = get_absolute_time();
    } else if (_avs.active) {
        _avs.voltage_mv = voltage_mv;
        _avs.last_refresh = get_absolute_time();
    }
}

// ============================================================================
// Active Contract
// ============================================================================

bool PdManager::refreshActiveContract() {
    uint32_t voltage_mv, current_ma;

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
        if (_pps.active && (_pps.voltage_mv == 0 ||
            (!waiting_for_programmable_request &&
             !withinTolerance(voltage_mv, _pps.voltage_mv, _pps.tracking_tolerance_mv) &&
             matches_fixed_pdo))) {
            clearTracking(_pps);
        }
        if (_avs.active && (_avs.voltage_mv == 0 ||
            (!waiting_for_programmable_request &&
             !withinTolerance(voltage_mv, _avs.voltage_mv, _avs.tracking_tolerance_mv) &&
             matches_fixed_pdo))) {
            clearTracking(_avs);
        }

        // Detect PPS/AVS from tracked state or by matching against PDO cache.
        // On warm MCU reset, _pps.active/_avs.active is false but TPS26750 still has a contract.
        // Detect this by checking if the active voltage matches a fixed PDO exactly.
        bool detected_pps = _pps.active;
        bool detected_avs = _avs.active;

        // Suppress warm-reset re-detection while a contract request is in flight.
        // When exiting a programmable (PPS/AVS) contract to a Fixed PDO, the TPS26750
        // transiently keeps reporting the OLD programmable voltage before it settles to
        // the new rail. Re-arming keep-alive from that stale reading would re-request the
        // old voltage and fight the in-flight Fixed request, making the contract
        // impossible to exit. During a request the requested type is authoritative;
        // genuine warm-reset detection only needs to run when idle/settled.
        if (_negotiation_state != NegotiationState::REQUESTING &&
            !_pps.active && !_avs.active && _pdos_valid && voltage_mv > 0) {
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
                        _pps.active = true;
                        _pps.voltage_mv = voltage_mv;
                        _pps.current_ma = current_ma;
                        _pps.last_refresh = get_absolute_time();
                        _pps.range_min_mv = _pdo_cache[i].min_voltage_mv;
                        _pps.range_max_mv = _pdo_cache[i].voltage_mv;
                        _active_pdo_index = (int8_t)i;
                        LOG_INFO("Detected active PPS contract on warm reset: %lumV", voltage_mv);
                        break;
                    }
                    if (_pdo_cache[i].is_avs &&
                        voltage_mv >= _pdo_cache[i].min_voltage_mv &&
                        voltage_mv <= _pdo_cache[i].voltage_mv) {
                        detected_avs = true;
                        _avs.active = true;
                        _avs.voltage_mv = voltage_mv;
                        _avs.current_ma = current_ma;
                        _avs.last_refresh = get_absolute_time();
                        _avs.range_min_mv = _pdo_cache[i].min_voltage_mv;
                        _avs.range_max_mv = _pdo_cache[i].voltage_mv;
                        _active_pdo_index = (int8_t)i;
                        LOG_INFO("Detected active AVS contract on warm reset: %lumV", voltage_mv);
                        break;
                    }
                }
            }
        }

        _active_contract.is_pps = detected_pps;
        _active_contract.is_avs = detected_avs;
        // EPR AVS APDOs always have min_voltage > 9V (e.g. 15V); SPR AVS is always 9V.
        _active_contract.is_epr = detected_avs && (_avs.range_min_mv != 9000 && _avs.range_min_mv != 0);

        // Prefer the requested programmable current when a PPS/AVS contract overlaps
        // a fixed PDO and the controller reports the fixed-PDO current instead.
        if (detected_pps && _pps.current_ma > 0) {
            _active_contract.current_ma = _pps.current_ma;
        } else if (detected_avs && _avs.current_ma > 0) {
            _active_contract.current_ma = _avs.current_ma;
        }

        // Cache the active programmable range for UI/CLI validation.
        if (detected_pps) {
            _active_contract.programmable_min_mv = _pps.range_min_mv;
            _active_contract.programmable_max_mv = _pps.range_max_mv;
        } else if (detected_avs) {
            _active_contract.programmable_min_mv = _avs.range_min_mv;
            _active_contract.programmable_max_mv = _avs.range_max_mv;
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
        clearTracking(_pps);
        clearTracking(_avs);
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
        clearTracking(_pps);
        clearTracking(_avs);
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
        clearTracking(_pps);
        clearTracking(_avs);
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

    switch (_requested_contract_type) {
        case RequestedContractType::FIXED:
            return !_active_contract.is_pps && !_active_contract.is_avs &&
                   withinTolerance(_active_contract.voltage_mv, _requested_voltage_mv,
                                    FIXED_MATCH_TOLERANCE_MV);

        case RequestedContractType::PPS:
            return _active_contract.is_pps &&
                   withinTolerance(_active_contract.voltage_mv, _requested_voltage_mv,
                                    PPS_MATCH_TOLERANCE_MV);

        case RequestedContractType::AVS:
            // During EPR exit step-down, some chargers satisfy the 15V floor using an
            // overlapping fixed contract instead of reporting the AVS APDO we asked
            // for. That is still sufficient for the next exit step because VBUS is
            // already back in the safe SPR range.
            if (_epr_exit_state == EprExitState::STEPPING_DOWN) {
                return withinTolerance(_active_contract.voltage_mv, _requested_voltage_mv,
                                        AVS_MATCH_TOLERANCE_MV);
            }
            return _active_contract.is_avs &&
                   withinTolerance(_active_contract.voltage_mv, _requested_voltage_mv,
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
    return _pps.active && settings.isAutoPpsEnabled() && _pps.user_target_mv > 0;
}

bool PdManager::isAvsTuningActive() const {
    return _avs.active && settings.isAutoAvsEnabled() && _avs.user_target_mv > 0;
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
        if (_pps.active && _pps.user_target_mv > 0) {
            int32_t error = (int32_t)_pps.user_target_mv - (int32_t)measured_mv;
            _pps.tuning_converged = (abs(error) <= _pps.tune_threshold_mv);
        }
        if (_avs.active && _avs.user_target_mv > 0) {
            int32_t error = (int32_t)_avs.user_target_mv - (int32_t)measured_mv;
            _avs.tuning_converged = (abs(error) <= _avs.tune_threshold_mv);
        }
    }
}

// ============================================================================
// Startup Contract Negotiation
// ============================================================================

bool PdManager::waitForPdos(uint32_t timeout_ms) {
    if (is_nil_time(_wait_pdos_start)) {
        _wait_pdos_start = get_absolute_time();
    }

    uint32_t elapsed_ms = absolute_time_diff_us(_wait_pdos_start, get_absolute_time()) / 1000;

    // Check if PDOs are already available
    if (!_pdos_valid) {
        _pdo_count = hw.pdController.getSourceCapabilities(_pdo_cache, AppConfig::MAX_PDO_COUNT);
        _pdos_valid = (_pdo_count > 0);

        if (_pdos_valid) {
            detectPdRevision();
            refreshActiveContract();
            LOG_INFO("Initial PDO discovery: %d PDOs found", _pdo_count);
            _wait_pdos_start = nil_time;
            return true; // We found PDOs! Done!
        }
    }

    // Check timeout
    if (elapsed_ms >= timeout_ms) {
        LOG_WARN("PDO discovery timed out after %lums", elapsed_ms);
        clearTracking(_pps);
        clearTracking(_avs);
        clearRequestedContract();
        _pd_revision[0] = '\0';
        refreshActiveContract();
        _wait_pdos_start = nil_time;
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
        case StartupContractMode::HIGHEST_VOLTAGE:
            // Handled by the early return above; listed so -Wswitch stays clean.
            break;

        case StartupContractMode::LOWEST_VOLTAGE:
            target_idx = lowest_idx;
            if (target_idx >= 0) {
                LOG_INFO("Startup negotiation: Lowest voltage - %lumV (PDO[%d])",
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
                    const TPS26750_SourceCapability& hinted_pdo = _pdo_cache[snapshot.pdo_index_hint];
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
                            LOG_INFO("Startup negotiation: using legacy snapshot hint PDO[%d] to restore %s at %lumV",
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
                            LOG_INFO("Startup negotiation: exact %s restore match on PDO[%d], target %lumV within %lu-%lumV",
                                     savedStartupContractTypeName(snapshot.type),
                                     snapshot.pdo_index_hint,
                                     target_request_voltage_mv,
                                     hinted_pdo.min_voltage_mv,
                                     hinted_pdo.voltage_mv);
                        } else {
                            LOG_INFO("Startup negotiation: saved %s target %lumV is outside charger PDO[%d] range %lu-%lumV",
                                     savedStartupContractTypeName(snapshot.type),
                                     target_voltage_mv,
                                     snapshot.pdo_index_hint,
                                     hinted_pdo.min_voltage_mv,
                                     hinted_pdo.voltage_mv);
                        }
                    } else if (hinted_pdo.voltage_mv == target_voltage_mv) {
                        target_idx = snapshot.pdo_index_hint;
                        target_request_voltage_mv = hinted_pdo.voltage_mv;
                        LOG_INFO("Startup negotiation: exact fixed restore match on PDO[%d] at %lumV",
                                 snapshot.pdo_index_hint,
                                 hinted_pdo.voltage_mv);
                    } else {
                        LOG_INFO("Startup negotiation: saved fixed target %lumV does not match charger PDO[%d] at %lumV",
                                 target_voltage_mv,
                                 snapshot.pdo_index_hint,
                                 hinted_pdo.voltage_mv);
                    }
                }
            }

            if (target_idx < 0) {
                if (allow_epr_wait && target_prefers_epr_retry && !epr_visible) {
                    _startup_restore_waiting_for_epr = true;
                    LOG_INFO("Startup negotiation: saved %s target %lumV may require EPR PDOs, waiting for EPR discovery before fallback",
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
                    const TPS26750_SourceCapability& candidate = _pdo_cache[target_idx];
                    LOG_INFO("Startup negotiation: same-type fallback selected %s PDO[%d] at %lumV (delta %lumV from saved target %lumV)",
                             sourceCapabilityTypeName(candidate),
                             target_idx,
                             target_request_voltage_mv,
                             same_type_match.diff_mv,
                             target_voltage_mv);
                } else {
                    if (snapshot.type != SavedStartupContractType::UNKNOWN) {
                        LOG_INFO("Startup negotiation: no %s candidates available for saved target %lumV",
                                 savedStartupContractTypeName(snapshot.type),
                                 target_voltage_mv);
                    }

                    if (allow_epr_wait && target_prefers_epr_retry && !epr_visible) {
                            _startup_restore_waiting_for_epr = true;
                            LOG_INFO("Startup negotiation: saved %s target %lumV may require EPR PDOs, waiting for EPR discovery before cross-type fallback",
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
                        LOG_WARN("Startup negotiation: no fallback candidate found for saved target %lumV, keeping default",
                                 target_voltage_mv);
                        return false;
                    }

                    target_idx = fallback_match.pdo_index;
                    target_request_voltage_mv = fallback_match.requested_voltage_mv;
                    const TPS26750_SourceCapability& candidate = _pdo_cache[target_idx];
                    LOG_INFO("Startup negotiation: cross-type fallback selected %s PDO[%d] at %lumV (delta %lumV from saved target %lumV)",
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
        const TPS26750_SourceCapability& pdo = _pdo_cache[target_idx];

        if (pdo.is_pps && target_request_voltage_mv > 0) {
            LOG_INFO("Startup negotiation: requesting PPS %lumV using PDO[%d]",
                     target_request_voltage_mv, target_idx);
            return requestPpsVoltage(target_request_voltage_mv, pdo.max_current_ma, target_idx);
        } else if (pdo.is_avs && target_request_voltage_mv > 0) {
            LOG_INFO("Startup negotiation: requesting AVS %lumV using PDO[%d]",
                     target_request_voltage_mv, target_idx);
            return requestAvsVoltage(target_request_voltage_mv, pdo.max_current_ma, target_idx);
        } else {
            if (_active_contract.valid && !_active_contract.is_pps && !_active_contract.is_avs &&
                _active_contract.voltage_mv == pdo.voltage_mv) {
                LOG_INFO("Startup negotiation: fixed target %lumV is already active from the pre-boot request; refreshing with PDO[%d] current data",
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
        hw.pdController.sendCommand(TPS26750_CMD_ESRC);
    }
}