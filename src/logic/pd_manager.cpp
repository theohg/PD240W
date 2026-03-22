#include "pd_manager.h"
#include "hardware.h"
#include "config/board_config.h"
#include "interrupts.h"
#include "logic/settings.h"
#include "ui/display_manager.h"
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
    , _pre_request_voltage_mv(0)
    , _pre_request_current_ma(0)
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
                // Check if contract changed from pre-request state
                if (current_voltage_mv != _pre_request_voltage_mv ||
                    current_current_ma != _pre_request_current_ma) {
                    LOG_INFO("Contract change detected via polling: %umV @ %umA",
                             current_voltage_mv, current_current_ma);
                    refreshActiveContract();
                    _negotiation_state = NegotiationState::SUCCESS;
                }
            }
        }

        if (elapsed_ms >= NEGOTIATION_TIMEOUT_MS && _negotiation_state == NegotiationState::REQUESTING) {
            LOG_WARN("Contract negotiation timeout");
            _negotiation_state = NegotiationState::TIMEOUT;
        }
    }

    // Deferred PDO discovery: if PD revision is unknown, TPS26750 may have negotiated
    // before RP2040 GPIO interrupts were set up (missed edge at cold boot).
    // Retry every 500ms until PDOs are found.
    if (_pd_revision[0] == '\0' && _charger_connected) {
        static absolute_time_t next_pdo_retry = {0};
        if (absolute_time_diff_us(next_pdo_retry, get_absolute_time()) >= 0) {
            next_pdo_retry = make_timeout_time_ms(500);
            _pdo_count = hw.pdController.getSourceCapabilities(_pdo_cache, 13);
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
            if (settings.isAutoPpsEnabled() && _pps_user_target_mv > 0) {
                // Measure actual output voltage
                float measured_v;
                if (gpio_get(Board::PIN_SWITCH_EN)) {
                    measured_v = hw.powerMonitor.getBusVoltage();  // Post-switch (accurate)
                } else {
                    measured_v = hw.adc.getVBUS();  // Pre-switch (fallback)
                }
                uint32_t measured_mv = (uint32_t)(measured_v * 1000.0f);

                // Only tune if we have a valid measurement (> 1V, likely PPS is delivering)
                if (measured_mv > 1000) {
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
                adjusted = (adjusted / 20) * 20;

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

    // AVS keep-alive: EPR contracts also need periodic re-request to maintain the contract
    if (_avs_active && _avs_voltage_mv > 0) {
        uint32_t elapsed_ms = absolute_time_diff_us(_avs_last_refresh, get_absolute_time()) / 1000;

        if (elapsed_ms >= AVS_REFRESH_INTERVAL_MS) {
            LOG_DEBUG("AVS keep-alive: requesting %umV @ %umA", _avs_voltage_mv, _avs_current_ma);

            if (hw.pdController.requestAVSProfile(_avs_voltage_mv, _avs_current_ma)) {
                _avs_last_refresh = get_absolute_time();
            } else {
                LOG_WARN("AVS keep-alive request failed");
            }
        }
    }

    // Periodic contract refresh for display sync (every 1 second)
    // Ensures displayed contract matches actual state after EPR transitions
    static absolute_time_t next_contract_refresh = {0};
    if (absolute_time_diff_us(next_contract_refresh, get_absolute_time()) >= 0) {
        next_contract_refresh = make_timeout_time_ms(1000);

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
        _pdo_count = hw.pdController.getSourceCapabilities(_pdo_cache, 13);
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
// Contract Negotiation
// ============================================================================

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

    // Store pre-request contract for polling fallback
    _pre_request_voltage_mv = _active_contract.voltage_mv;
    _pre_request_current_ma = _active_contract.current_ma;

    bool success = hw.pdController.requestFixedProfile(voltage_mv, current_ma);

    if (success) {
        _negotiation_state = NegotiationState::REQUESTING;
        _negotiation_start = get_absolute_time();

        // Deactivate PPS mode when switching to fixed
        _pps_active = false;
        _pps_voltage_mv = 0;
        _pps_current_ma = 0;
        _pps_user_target_mv = 0;
        _pps_correction_mv = 0;
        _pps_tuning_converged = false;

        // Deactivate AVS mode when switching to fixed
        _avs_active = false;
        _avs_voltage_mv = 0;
        _avs_current_ma = 0;
    } else {
        _negotiation_state = NegotiationState::FAILED;
        LOG_ERROR("Failed to send Fixed contract request");
    }

    return success;
}

bool PdManager::requestPpsVoltage(uint32_t voltage_mv, uint32_t current_ma) {
    LOG_INFO("Requesting PPS: %umV @ %umA", voltage_mv, current_ma);

    // Store pre-request contract for polling fallback
    _pre_request_voltage_mv = _active_contract.voltage_mv;
    _pre_request_current_ma = _active_contract.current_ma;

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
        _avs_active = false;
        _avs_voltage_mv = 0;
        _avs_current_ma = 0;

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

    bool success = hw.pdController.requestAVSProfile(voltage_mv, current_ma);

    if (success) {
        _negotiation_state = NegotiationState::REQUESTING;
        _negotiation_start = get_absolute_time();

        // Deactivate PPS mode when switching to AVS
        _pps_active = false;
        _pps_voltage_mv = 0;
        _pps_current_ma = 0;
        _pps_user_target_mv = 0;
        _pps_correction_mv = 0;
        _pps_tuning_converged = false;

        // Track AVS state for keep-alive
        _avs_active = true;
        _avs_voltage_mv = voltage_mv;
        _avs_current_ma = current_ma;
        _avs_last_refresh = get_absolute_time();
    } else {
        _negotiation_state = NegotiationState::FAILED;
        LOG_ERROR("Failed to send AVS contract request");
    }

    return success;
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

        // Detect PPS/AVS from tracked state or by matching against PDO cache.
        // On warm MCU reset, _pps_active is false but TPS26750 still has a PPS contract.
        // Detect this by checking if the active voltage matches a fixed PDO exactly.
        bool detected_pps = _pps_active;
        bool detected_avs = false;

        if (!_pps_active && _pdos_valid && voltage_mv > 0) {
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
                        break;
                    }
                }
            }
        }

        _active_contract.is_pps = detected_pps;
        _active_contract.is_avs = detected_avs;

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
    bool has_avs = false;
    bool has_pps = false;

    for (uint8_t i = 0; i < _pdo_count; i++) {
        if (_pdo_cache[i].is_avs) has_avs = true;
        if (_pdo_cache[i].is_pps) has_pps = true;
    }

    // Standard SPR (PD 2.0/3.0) allows max 7 PDOs. 
    // 8+ PDOs or the presence of AVS guarantees EPR (PD 3.1+).
    if (has_avs || _pdo_count > 7) {
        // You can safely assume at least PD 3.1. 
        // (PD 3.2 chargers will fall into this bucket as well).
        strcpy(_pd_revision, "PD3.1+"); 
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
            _pdo_count = hw.pdController.getSourceCapabilities(_pdo_cache, 13);
            _pdos_valid = (_pdo_count > 0);
            if (_pdos_valid) {
                detectPdRevision();
            }
        }

        // Update negotiation state
        if (_negotiation_state == NegotiationState::REQUESTING) {
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
        _pdo_count = hw.pdController.getSourceCapabilities(_pdo_cache, 13);
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

        // Check connection status
        uint32_t voltage_mv, current_ma;
        _charger_connected = hw.pdController.getActiveContract(voltage_mv, current_ma);

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
        _negotiation_state = NegotiationState::IDLE;

        // Clear the interrupt
        uint8_t clear_mask[11] = {0};
        clear_mask[0] = (1 << 1);  // Bit 1
        hw.pdController.clearInterrupts(clear_mask);
    }
}

bool PdManager::isPpsTuningActive() const {
    return _pps_active && settings.isAutoPpsEnabled() && _pps_user_target_mv > 0;
}

bool PdManager::checkNewContractEvent() {
    uint8_t events[11] = {0};

    if (hw.pdController.readInterrupts(events)) {
        return hw.pdController.isInterruptSet(events, 12);
    }

    return false;
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
        _pdo_count = hw.pdController.getSourceCapabilities(_pdo_cache, 13);
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
    uint32_t target_pps_voltage_mv = 0; 

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

                if (saved_pdo.is_pps && saved_pps_voltage > 0) {
                    if (saved_pps_voltage >= saved_pdo.min_voltage_mv &&
                        saved_pps_voltage <= saved_pdo.voltage_mv) {
                        target_idx = saved_idx;
                        target_pps_voltage_mv = saved_pps_voltage;
                        LOG_INFO("Startup negotiation: Restoring PPS %umV (PDO[%d])",
                                 saved_pps_voltage, saved_idx);
                    }
                } else if (!saved_pdo.is_pps) {
                    target_idx = saved_idx;
                    LOG_INFO("Startup negotiation: Restoring %s %umV (PDO[%d])",
                             saved_pdo.is_avs ? "AVS" : "Fixed",
                             saved_pdo.voltage_mv, saved_idx);
                }
            }

            // If exact match failed
            if (target_idx < 0) {
                if (saved_idx >= _pdo_count) {
                    // Safe guard: The charger might be in the middle of fetching EPR capabilities.
                    LOG_WARN("Startup negotiation: Saved PDO %d > current count %d. Assuming EPR arriving later.", saved_idx, _pdo_count);
                    return false; 
                }

                // Find closest fixed/AVS PDO by voltage
                uint32_t target_voltage_mv = 0;
                if (saved_idx < _pdo_count) {
                    target_voltage_mv = _pdo_cache[saved_idx].voltage_mv;
                }
                if (saved_pps_voltage > 0) {
                    target_voltage_mv = saved_pps_voltage;
                }

                int32_t min_diff = INT32_MAX;
                for (uint8_t i = 0; i < _pdo_count; i++) {
                    if (!_pdo_cache[i].is_pps) {
                        int32_t diff = abs((int32_t)_pdo_cache[i].voltage_mv - (int32_t)target_voltage_mv);
                        if (diff < min_diff) {
                            min_diff = diff;
                            target_idx = i;
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

        if (pdo.is_pps && target_pps_voltage_mv > 0) {
            return requestPpsVoltage(target_pps_voltage_mv, pdo.max_current_ma);
        } else {
            return requestContract(pdo);
        }
    }

    return false;
}

void PdManager::probeEpr() {
    bool has_epr = false;
    for (uint8_t i = 0; i < _pdo_count; i++) {
        if (_pdo_cache[i].is_avs || _pdo_cache[i].voltage_mv > 20000) {
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