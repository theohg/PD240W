#include "pd_manager.h"
#include "hardware.h"
#include "interrupts.h"
#include "utils/logging.h"

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
{
    _active_contract.voltage_mv = 0;
    _active_contract.current_ma = 0;
    _active_contract.is_pps = false;
    _active_contract.is_avs = false;
    _active_contract.valid = false;
    _active_contract.pps_min_mv = 0;
    _active_contract.pps_max_mv = 0;
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

    // PPS keep-alive: must refresh contract every <10 seconds or source reverts to 5V
    if (_pps_active && _pps_voltage_mv > 0) {
        uint32_t elapsed_ms = absolute_time_diff_us(_pps_last_refresh, get_absolute_time()) / 1000;

        if (elapsed_ms >= PPS_REFRESH_INTERVAL_MS) {
            LOG_DEBUG("PPS keep-alive: refreshing %umV @ %umA", _pps_voltage_mv, _pps_current_ma);

            // Re-request the same PPS contract
            if (hw.pdController.requestPPSProfile(_pps_voltage_mv, _pps_current_ma)) {
                _pps_last_refresh = get_absolute_time();
            } else {
                LOG_WARN("PPS keep-alive request failed");
                // Don't deactivate - let it retry next cycle
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

        // Use our tracked PPS state for proper detection
        _active_contract.is_pps = _pps_active;
        _active_contract.is_avs = false;  // TODO: Add AVS tracking when needed

        // Store PPS voltage range if active
        if (_pps_active) {
            _active_contract.pps_min_mv = _pps_voltage_mv;  // Store current requested voltage
            _active_contract.pps_max_mv = _pps_voltage_mv;  // Will be updated from capability
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

bool PdManager::checkNewContractEvent() {
    uint8_t events[11] = {0};

    if (hw.pdController.readInterrupts(events)) {
        return hw.pdController.isInterruptSet(events, 12);
    }

    return false;
}
