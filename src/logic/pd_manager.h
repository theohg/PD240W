#pragma once

#include <cstdint>
#include "pico/stdlib.h"
#include "drivers/power/tps26750/tps26750.h"

// ============================================================================
// USB Power Delivery Manager
// ============================================================================
// Higher-level management of USB-PD contracts.
// Wraps TPS26750 driver with negotiation state tracking and event handling.
// ============================================================================

// Negotiation states
enum class NegotiationState {
    IDLE,               // No negotiation in progress
    REQUESTING,         // Request sent, waiting for response
    SUCCESS,            // Negotiation completed successfully
    FAILED,             // Negotiation failed
    TIMEOUT             // No response within timeout
};

// Contract info for display
struct ActiveContract {
    uint32_t voltage_mv;
    uint32_t current_ma;
    bool is_pps;
    bool is_avs;
    bool valid;
    // PPS-specific fields
    uint32_t pps_min_mv;    // PPS range min voltage
    uint32_t pps_max_mv;    // PPS range max voltage
};

class PdManager {
public:
    PdManager();

    // Initialize PD manager
    void init();

    // Main update function - call every iteration of main loop
    // Handles negotiation state machine and interrupt processing
    void update();

    // Get available PDOs from charger
    uint8_t getSourceCapabilities(SourceCapability* caps, uint8_t max_caps);

    // Request a specific contract (non-blocking)
    bool requestContract(const SourceCapability& pdo);
    bool requestFixedVoltage(uint32_t voltage_mv, uint32_t current_ma);
    bool requestPpsVoltage(uint32_t voltage_mv, uint32_t current_ma);
    bool requestAvsVoltage(uint32_t voltage_mv, uint32_t current_ma);

    // Get negotiation state
    NegotiationState getNegotiationState() const { return _negotiation_state; }
    bool isNegotiating() const { return _negotiation_state == NegotiationState::REQUESTING; }

    // Get active contract info
    const ActiveContract& getActiveContract() const { return _active_contract; }
    bool refreshActiveContract();

    // Check if charger is connected
    bool isChargerConnected() const { return _charger_connected; }

    // Check if PPS contract is active
    bool isPpsActive() const { return _pps_active; }

    // Get TPS26750 mode string
    bool getMode(char* mode_str);

private:
    // Negotiation state
    NegotiationState _negotiation_state;
    absolute_time_t _negotiation_start;
    static constexpr uint32_t NEGOTIATION_TIMEOUT_MS = 2000;

    // Active contract cache
    ActiveContract _active_contract;

    // Connection state
    bool _charger_connected;

    // Cached PDO list
    SourceCapability _pdo_cache[13];
    uint8_t _pdo_count;
    bool _pdos_valid;

    // PPS keep-alive state
    bool _pps_active;                   // True if current contract is PPS
    uint32_t _pps_voltage_mv;           // Last requested PPS voltage
    uint32_t _pps_current_ma;           // Last requested PPS current
    absolute_time_t _pps_last_refresh;  // Time of last PPS request
    static constexpr uint32_t PPS_REFRESH_INTERVAL_MS = 7000;  // Refresh every 7s (spec requires <10s)

    // Process PD interrupt events
    void handlePdInterrupt();

    // Check for new contract event
    bool checkNewContractEvent();
};

// Global instance
extern PdManager pdManager;
