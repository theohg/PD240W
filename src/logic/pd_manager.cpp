#include "pd_manager.h"
#include "hardware.h"
#include "config/board_config.h"
#include "interrupts.h"
#include "logic/cc_controller.h"
#include "logic/settings.h"
#include "utils/logging.h"
#include <cstring>
#include <cstdlib>  // abs()

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
    , _epr_deferred_is_pps(false)
    , _epr_exit_start(nil_time)
{
    _active_contract.voltage_mv = 0;
    _active_contract.current_ma = 0;
    _active_contract.is_pps = false;
    _active_contract.is_avs = false;
    _active_contract.valid = false;
    _active_contract.pps_min_mv = 0;
    _active_contract.pps_max_mv = 0;
    _pd_revision[0] = '\0';
}

// ============================================================================
// Initialization
// ============================================================================

void PdManager::init() {
    // Check TPS26750 mode
    char mode[5];
    if (getMode(mode)) {
        LOG_INFO("TPS26750 mode: %s", mode);

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
                bool contract_changed =
                    (current_voltage_mv != _pre_request_voltage_mv ||
                     current_current_ma != _pre_request_current_ma);

                // Check if contract changed from pre-request state
                if (contract_changed) {
                    LOG_INFO("Contract change detected via polling: %umV @ %umA",
                             current_voltage_mv, current_current_ma);
                }

                refreshActiveContract();

                if (isRequestedContractReached()) {
                    _negotiation_state = NegotiationState::SUCCESS;
                } else if (contract_changed) {
                    LOG_WARN("Ignoring intermediate contract while waiting for requested %umV",
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
                _negotiation_state = NegotiationState::REQUESTING;
                _negotiation_start = get_absolute_time();
            }
        }
        // Step 2 complete: 5V Fixed succeeded -> now request user's actual target
        else if (_epr_exit_state == EprExitState::REQUESTING_5V &&
                 _negotiation_state == NegotiationState::SUCCESS) {
            refreshActiveContract();
            LOG_INFO("EPR step 2/3 complete: at %umV (SPR). Requesting target: %s %umV @ %umA",
                     _active_contract.voltage_mv,
                     _epr_deferred_is_pps ? "PPS" : "Fixed",
                     _epr_deferred_voltage_mv, _epr_deferred_current_ma);
            _epr_exit_state = EprExitState::REQUESTING_TARGET;
            // Fire the user's actual request (EPR exit state prevents re-interception)
            if (_epr_deferred_is_pps) {
                requestPpsVoltage(_epr_deferred_voltage_mv, _epr_deferred_current_ma);
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

        if (elapsed_ms >= PPS_REFRESH_INTERVAL_MS) {
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

                // Round to 20mV steps (PD spec)
                adjusted = (adjusted / AppConfig::PPS_VOLTAGE_STEP_MV) * AppConfig::PPS_VOLTAGE_STEP_MV;

                request_mv = (uint32_t)adjusted;
            }

            LOG_DEBUG("PPS keep-alive: requesting %umV @ %umA", request_mv, _pps_current_ma);

            if (hw.pdController.requestPPSProfile(request_mv, _pps_current_ma)) {
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

        if (elapsed_ms >= AVS_REFRESH_INTERVAL_MS) {
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

                // Round to 25mV steps (AVS PD spec)
                adjusted = (adjusted / AppConfig::AVS_VOLTAGE_STEP_MV) * AppConfig::AVS_VOLTAGE_STEP_MV;

                request_mv = (uint32_t)adjusted;
            }

            LOG_DEBUG("AVS keep-alive: requesting %umV @ %umA", request_mv, _avs_current_ma);

            if (hw.pdController.requestAVSProfile(request_mv, _avs_current_ma)) {
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
    // Check if we have an AVS PDO that can reach SPR range
    for (uint8_t i = 0; i < _pdo_count; i++) {
        if (_pdo_cache[i].is_avs && _pdo_cache[i].min_voltage_mv <= AppConfig::EPR_SPR_MAX_MV) {
            return true;
        }
    }
    return false;
}

bool PdManager::findAvsSafeVoltage(uint32_t& avs_voltage_mv, uint32_t& avs_current_ma) const {
    // Find an AVS PDO and return its minimum voltage (lowest possible = safest exit)
    for (uint8_t i = 0; i < _pdo_count; i++) {
        if (_pdo_cache[i].is_avs && _pdo_cache[i].min_voltage_mv <= AppConfig::EPR_SPR_MAX_MV) {
            // Use AVS PDO min voltage, rounded up to nearest 25mV boundary
            uint32_t min_mv = _pdo_cache[i].min_voltage_mv;
            avs_voltage_mv = ((min_mv + AppConfig::AVS_VOLTAGE_STEP_MV - 1) / AppConfig::AVS_VOLTAGE_STEP_MV) * AppConfig::AVS_VOLTAGE_STEP_MV;
            avs_current_ma = _pdo_cache[i].max_current_ma;
            return true;
        }
    }
    return false;
}

// ============================================================================
// Contract Negotiation
// ============================================================================

static bool is_standard_fixed_rail(uint32_t voltage_mv) {
    return voltage_mv == 5000 ||
           voltage_mv == 9000 ||
           voltage_mv == 15000 ||
           voltage_mv == 20000;
}

void PdManager::clearPpsTracking() {
    _pps_active = false;
    _pps_voltage_mv = 0;
    _pps_current_ma = 0;
    _pps_user_target_mv = 0;
    _pps_correction_mv = 0;
    _pps_tuning_converged = false;
    _pps_range_min_mv = 0;
    _pps_range_max_mv = 0;
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
        LOG_INFO("Startup prime: requesting 5V fixed floor before boot negotiation");
        return requestFixedVoltage(5000, startup_current_ma);
    }

    uint32_t saved_target_mv = settings.getLastPpsVoltageMv();
    if (saved_target_mv > AppConfig::EPR_SPR_MAX_MV) {
        LOG_INFO("Startup prime: requesting saved EPR target %umV before boot restore",
                 saved_target_mv);
        return requestAvsVoltage(saved_target_mv, startup_current_ma);
    }

    if (saved_target_mv > 0 && saved_target_mv < 5000) {
        LOG_INFO("Startup prime: requesting saved low-PPS target %umV before boot restore",
                 saved_target_mv);
        return requestPpsVoltage(saved_target_mv, startup_current_ma);
    }

    if (is_standard_fixed_rail(saved_target_mv)) {
        LOG_INFO("Startup prime: requesting saved fixed rail %umV before boot restore",
                 saved_target_mv);
        return requestFixedVoltage(saved_target_mv, startup_current_ma);
    }

    if (saved_target_mv > 0) {
        LOG_INFO("Startup prime: clamping to 5V before restoring programmable SPR target %umV",
                 saved_target_mv);
    } else {
        LOG_INFO("Startup prime: clamping to 5V before restoring saved fixed contract");
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
        if (findAvsSafeVoltage(avs_v, avs_i)) {
            LOG_INFO("EPR exit: 3-step sequence for Fixed %umV (AVS %umV -> 5V -> %umV)",
                     voltage_mv, avs_v, voltage_mv);
            _epr_deferred_voltage_mv = voltage_mv;
            _epr_deferred_current_ma = current_ma;
            _epr_deferred_is_pps = false;
            _epr_exit_state = EprExitState::STEPPING_DOWN;
            _epr_exit_start = get_absolute_time();
            return requestAvsVoltage(avs_v, avs_i);
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

bool PdManager::requestPpsVoltage(uint32_t voltage_mv, uint32_t current_ma) {
    LOG_INFO("Requesting PPS: %umV @ %umA", voltage_mv, current_ma);

    // EPR safe exit: if currently in EPR and target is PPS (SPR), initiate 3-step exit
    if (_epr_exit_state == EprExitState::NONE && needsEprExit(voltage_mv, true)) {
        uint32_t avs_v, avs_i;
        if (findAvsSafeVoltage(avs_v, avs_i)) {
            LOG_INFO("EPR exit: 3-step sequence for PPS %umV (AVS %umV -> 5V -> PPS %umV)",
                     voltage_mv, avs_v, voltage_mv);
            _epr_deferred_voltage_mv = voltage_mv;
            _epr_deferred_current_ma = current_ma;
            _epr_deferred_is_pps = true;
            _epr_exit_state = EprExitState::STEPPING_DOWN;
            _epr_exit_start = get_absolute_time();
            return requestAvsVoltage(avs_v, avs_i);
        } else {
            LOG_WARN("EPR exit needed but no suitable AVS PDO found -- direct request (may reboot)");
        }
    }

    // Store pre-request contract for polling fallback
    _pre_request_voltage_mv = _active_contract.voltage_mv;
    _pre_request_current_ma = _active_contract.current_ma;
    _requested_contract_type = RequestedContractType::PPS;
    _requested_voltage_mv = voltage_mv;
    _requested_current_ma = current_ma;

    bool success = hw.pdController.requestPPSProfile(voltage_mv, current_ma);

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

        // Find PPS range from cached PDOs for clamping
        for (uint8_t i = 0; i < _pdo_count; i++) {
            if (_pdo_cache[i].is_pps &&
                voltage_mv >= _pdo_cache[i].min_voltage_mv &&
                voltage_mv <= _pdo_cache[i].voltage_mv) {
                _pps_range_min_mv = _pdo_cache[i].min_voltage_mv;
                _pps_range_max_mv = _pdo_cache[i].voltage_mv;
                break;
            }
        }
    } else {
        _negotiation_state = NegotiationState::FAILED;
        LOG_ERROR("Failed to send PPS contract request");
    }

    return success;
}

bool PdManager::requestAvsVoltage(uint32_t voltage_mv, uint32_t current_ma) {
    LOG_INFO("Requesting AVS: %umV @ %umA", voltage_mv, current_ma);

    // Store pre-request contract for polling fallback
    _pre_request_voltage_mv = _active_contract.voltage_mv;
    _pre_request_current_ma = _active_contract.current_ma;
    _requested_contract_type = RequestedContractType::AVS;
    _requested_voltage_mv = voltage_mv;
    _requested_current_ma = current_ma;

    bool success = hw.pdController.requestAVSProfile(voltage_mv, current_ma);

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

        // Find AVS range from cached PDOs for clamping
        for (uint8_t i = 0; i < _pdo_count; i++) {
            if (_pdo_cache[i].is_avs &&
                voltage_mv >= _pdo_cache[i].min_voltage_mv &&
                voltage_mv <= _pdo_cache[i].voltage_mv) {
                _avs_range_min_mv = _pdo_cache[i].min_voltage_mv;
                _avs_range_max_mv = _pdo_cache[i].voltage_mv;
                break;
            }
        }
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

        // Drop stale tracked programmable state when the source changed or boot priming
        // failed to obtain real PDO-backed PPS/AVS support.
        if (_pps_active && (!_pdos_valid || _pps_voltage_mv == 0 ||
            !within_tolerance(voltage_mv, _pps_voltage_mv, PPS_MATCH_TOLERANCE_MV))) {
            clearPpsTracking();
        }
        if (_avs_active && (!_pdos_valid || _avs_voltage_mv == 0 ||
            !within_tolerance(voltage_mv, _avs_voltage_mv, AVS_MATCH_TOLERANCE_MV))) {
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
                        LOG_INFO("Detected active AVS contract on warm reset: %umV", voltage_mv);
                        break;
                    }
                }
            }
        }

        _active_contract.is_pps = detected_pps;
        _active_contract.is_avs = detected_avs;

        // Prefer the requested programmable current when a PPS/AVS contract overlaps
        // a fixed PDO and the controller reports the fixed-PDO current instead.
        if (detected_pps && _pps_current_ma > 0) {
            _active_contract.current_ma = _pps_current_ma;
        } else if (detected_avs && _avs_current_ma > 0) {
            _active_contract.current_ma = _avs_current_ma;
        }

        // Store PPS voltage range if active
        if (detected_pps) {
            _active_contract.pps_min_mv = _pps_voltage_mv;
            _active_contract.pps_max_mv = _pps_voltage_mv;
        } else {
            _active_contract.pps_min_mv = 0;
            _active_contract.pps_max_mv = 0;
        }

        // LOG_DEBUG("Active contract: %umV @ %umA (PPS: %s)",
        //           voltage_mv, current_ma, _pps_active ? "yes" : "no");
        return true;
    }

    _active_contract.valid = false;
    _active_contract.is_pps = false;
    _active_contract.is_avs = false;
    _active_contract.pps_min_mv = 0;
    _active_contract.pps_max_mv = 0;
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
        strcpy(_pd_revision, "PD3.2");
    } else if (has_epr || has_epr_avs) {
        // EPR-only sources map to PD 3.1.
        strcpy(_pd_revision, "PD3.1");
    } else if (has_pps) {
        // PPS was introduced in PD 3.0
        strcpy(_pd_revision, "PD3.0");
    } else if (_pdo_count > 0) {
        // If neither PPS nor AVS are present and <= 7 PDOs, assume PD 2.0
        strcpy(_pd_revision, "PD2.0");
    } else {
        _pd_revision[0] = '\0';
    }

    if (_pd_revision[0] != '\0') {
        LOG_INFO("Detected PD revision (from PDOs): %s", _pd_revision);
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
        uint8_t clear_mask[11] = {0};
        clear_mask[1] = (1 << 4);  // Bit 12
        hw.pdController.clearInterrupts(clear_mask);
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
        uint8_t clear_mask[11] = {0};
        clear_mask[1] = (1 << 6);  // Bit 14
        hw.pdController.clearInterrupts(clear_mask);
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
        _active_contract.valid = false;
        _active_contract.is_pps = false;
        _active_contract.is_avs = false;
        _active_contract.pps_min_mv = 0;
        _active_contract.pps_max_mv = 0;
        _negotiation_state = NegotiationState::IDLE;
        _epr_exit_state = EprExitState::NONE;

        // Check connection status
        uint32_t voltage_mv, current_ma;
        _charger_connected = hw.pdController.getActiveContract(voltage_mv, current_ma);
        if (_charger_connected) {
            refreshActiveContract();
        }

        // Clear the interrupt
        uint8_t clear_mask[11] = {0};
        clear_mask[0] = (1 << 3);  // Bit 3
        hw.pdController.clearInterrupts(clear_mask);
    }

    // Check for hard reset (bit 1)
    if (hw.pdController.isInterruptSet(events, 1)) {
        LOG_WARN("PD Hard Reset received");

        // Invalidate everything
        _pdos_valid = false;
        _active_contract.valid = false;
        _active_contract.is_pps = false;
        _active_contract.is_avs = false;
        _active_contract.pps_min_mv = 0;
        _active_contract.pps_max_mv = 0;
        _negotiation_state = NegotiationState::IDLE;
        clearPpsTracking();
        clearAvsTracking();
        clearRequestedContract();
        _pd_revision[0] = '\0';
        _epr_exit_state = EprExitState::NONE;

        // Clear the interrupt
        uint8_t clear_mask[11] = {0};
        clear_mask[0] = (1 << 1);  // Bit 1
        hw.pdController.clearInterrupts(clear_mask);
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

bool PdManager::negotiateStartupContract() {
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
    uint32_t target_programmable_voltage_mv = 0;

    switch (mode) {
        case StartupContractMode::LOWEST_VOLTAGE:
            target_idx = lowest_idx;
            if (target_idx >= 0) {
                LOG_INFO("Startup negotiation: Lowest voltage - %umV (PDO[%d])",
                         lowest_voltage, target_idx);
            }
            break;

        case StartupContractMode::LAST_USED: {
            int8_t saved_idx = settings.getLastPdoIndex();
            uint32_t saved_pps_voltage = settings.getLastPpsVoltageMv();

            if (saved_idx < 0) {
                LOG_INFO("Startup negotiation: No saved PDO, keeping default");
                return false;
            }

            // First, try exact match by index
            if (saved_idx < _pdo_count) {
                const SourceCapability& saved_pdo = _pdo_cache[saved_idx];

                if ((saved_pdo.is_pps || saved_pdo.is_avs) && saved_pps_voltage > 0) {
                    if (saved_pps_voltage >= saved_pdo.min_voltage_mv &&
                        saved_pps_voltage <= saved_pdo.voltage_mv) {
                        target_idx = saved_idx;
                        target_programmable_voltage_mv = saved_pps_voltage;
                        LOG_INFO("Startup negotiation: Restoring %s %umV (PDO[%d])",
                                 saved_pdo.is_avs ? "AVS" : "PPS",
                                 saved_pps_voltage, saved_idx);
                    }
                } else if (!saved_pdo.is_pps && !saved_pdo.is_avs) {
                    target_idx = saved_idx;
                    LOG_INFO("Startup negotiation: Restoring Fixed %umV (PDO[%d])",
                             saved_pdo.voltage_mv, saved_idx);
                } else if (saved_pdo.is_avs) {
                    target_idx = saved_idx;
                    LOG_WARN("Startup negotiation: Missing saved AVS target, restoring max %umV (PDO[%d])",
                             saved_pdo.voltage_mv, saved_idx);
                }
            }

            // If exact match failed
            if (target_idx < 0) {
                uint32_t target_voltage_mv = 0;
                if (saved_pps_voltage > 0) {
                    target_voltage_mv = saved_pps_voltage;
                } else if (saved_idx < _pdo_count) {
                    target_voltage_mv = _pdo_cache[saved_idx].voltage_mv;
                }

                if (target_voltage_mv == 0) {
                    LOG_WARN("Startup negotiation: Saved PDO %d unavailable and no saved target voltage, keeping default",
                             saved_idx);
                    return false;
                }

                int32_t min_diff = INT32_MAX;
                for (uint8_t i = 0; i < _pdo_count; i++) {
                    int32_t diff = INT32_MAX;
                    uint32_t candidate_programmable_mv = 0;

                    if (_pdo_cache[i].is_pps && saved_pps_voltage > 0) {
                        uint32_t candidate_mv = target_voltage_mv;
                        if (candidate_mv < _pdo_cache[i].min_voltage_mv) candidate_mv = _pdo_cache[i].min_voltage_mv;
                        if (candidate_mv > _pdo_cache[i].voltage_mv) candidate_mv = _pdo_cache[i].voltage_mv;
                        candidate_mv = (candidate_mv / AppConfig::PPS_VOLTAGE_STEP_MV) * AppConfig::PPS_VOLTAGE_STEP_MV;
                        diff = abs((int32_t)candidate_mv - (int32_t)target_voltage_mv);
                        candidate_programmable_mv = candidate_mv;
                    } else if (_pdo_cache[i].is_avs && saved_pps_voltage > 0) {
                        uint32_t candidate_mv = target_voltage_mv;
                        if (candidate_mv < _pdo_cache[i].min_voltage_mv) candidate_mv = _pdo_cache[i].min_voltage_mv;
                        if (candidate_mv > _pdo_cache[i].voltage_mv) candidate_mv = _pdo_cache[i].voltage_mv;
                        candidate_mv = (candidate_mv / AppConfig::AVS_VOLTAGE_STEP_MV) * AppConfig::AVS_VOLTAGE_STEP_MV;
                        diff = abs((int32_t)candidate_mv - (int32_t)target_voltage_mv);
                        candidate_programmable_mv = candidate_mv;
                    } else if (!_pdo_cache[i].is_pps && !_pdo_cache[i].is_avs) {
                        diff = abs((int32_t)_pdo_cache[i].voltage_mv - (int32_t)target_voltage_mv);
                    }

                    if (diff < min_diff) {
                        min_diff = diff;
                        target_idx = i;
                        target_programmable_voltage_mv = candidate_programmable_mv;
                    }

                    if (diff == 0) {
                        break;
                    }
                }

                if (target_idx >= 0) {
                    const SourceCapability& candidate = _pdo_cache[target_idx];
                    if (min_diff > 0) {
                        if (candidate.is_pps || candidate.is_avs) {
                            LOG_INFO("Startup negotiation: Falling back to closest %s target %umV using PDO[%d] at %umV",
                                     candidate.is_avs ? "AVS" : "PPS",
                                     target_voltage_mv, target_idx, target_programmable_voltage_mv);
                        } else {
                            LOG_INFO("Startup negotiation: Falling back to closest fixed rail %umV using PDO[%d] at %umV",
                                     target_voltage_mv, target_idx, candidate.voltage_mv);
                        }
                    }
                }
            }
            break;
        }
    }

    // Execute the negotiation
    if (target_idx >= 0 && target_idx < _pdo_count) {
        const SourceCapability& pdo = _pdo_cache[target_idx];

        if (pdo.is_pps && target_programmable_voltage_mv > 0) {
            return requestPpsVoltage(target_programmable_voltage_mv, pdo.max_current_ma);
        } else if (pdo.is_avs && target_programmable_voltage_mv > 0) {
            return requestAvsVoltage(target_programmable_voltage_mv, pdo.max_current_ma);
        } else {
            return requestContract(pdo);
        }
    }

    return false;
}

void PdManager::probeEpr() {
    bool has_epr = false;
    for (uint8_t i = 0; i < _pdo_count; i++) {
        if (_pdo_cache[i].is_avs || _pdo_cache[i].voltage_mv > AppConfig::EPR_SPR_MAX_MV) {
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