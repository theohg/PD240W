#include "pd_manager.h"
#include "hardware.h"
#include "config/board_config.h"
#include "interrupts.h"
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

    // Read initial source capabilities
    _pdo_count = hw.pdController.getSourceCapabilities(_pdo_cache, 13);
    _pdos_valid = (_pdo_count > 0);

    if (_pdos_valid) {
        LOG_INFO("Found %d PDOs from charger", _pdo_count);
        _charger_connected = true;
        detectPdRevision();
    } else {
        LOG_WARN("No PDOs found - EEPROM might not have been read correctly");
    }

    // Read current active contract
    refreshActiveContract();

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

    // Check negotiation timeout
    if (_negotiation_state == NegotiationState::REQUESTING) {
        uint32_t elapsed_ms = absolute_time_diff_us(_negotiation_start, get_absolute_time()) / 1000;

        if (elapsed_ms >= NEGOTIATION_TIMEOUT_MS) {
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
        return requestPpsVoltage(pdo.min_voltage_mv, pdo.max_current_ma);
    } else if (pdo.is_avs) {
        return requestAvsVoltage(pdo.min_voltage_mv, pdo.max_current_ma);
    } else {
        return requestFixedVoltage(pdo.voltage_mv, pdo.max_current_ma);
    }
}

bool PdManager::requestFixedVoltage(uint32_t voltage_mv, uint32_t current_ma) {
    LOG_INFO("Requesting Fixed: %umV @ %umA", voltage_mv, current_ma);

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
    } else {
        _negotiation_state = NegotiationState::FAILED;
        LOG_ERROR("Failed to send Fixed contract request");
    }

    return success;
}

bool PdManager::requestPpsVoltage(uint32_t voltage_mv, uint32_t current_ma) {
    LOG_INFO("Requesting PPS: %umV @ %umA", voltage_mv, current_ma);

    bool success = hw.pdController.requestPPSProfile(voltage_mv, current_ma);

    if (success) {
        _negotiation_state = NegotiationState::REQUESTING;
        _negotiation_start = get_absolute_time();

        // Track PPS state for keep-alive
        _pps_active = true;
        _pps_voltage_mv = voltage_mv;
        _pps_current_ma = current_ma;
        _pps_last_refresh = get_absolute_time();

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

        LOG_DEBUG("Active contract: %umV @ %umA (PPS: %s)",
                  voltage_mv, current_ma, _pps_active ? "yes" : "no");
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

    if (has_avs) {
        strcpy(_pd_revision, "PD3.1");
    } else if (has_pps) {
        strcpy(_pd_revision, "PD3.0");
    } else if (_pdo_count > 0) {
        strcpy(_pd_revision, "PD2.0");
    } else {
        _pd_revision[0] = '\0';
    }

    if (_pd_revision[0] != '\0') {
        LOG_INFO("Detected PD revision: %s", _pd_revision);
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
