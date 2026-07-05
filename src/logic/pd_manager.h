#pragma once

#include <cstdint>
#include "pico/stdlib.h"
#include "config/app_config.h"
#include "tps26750.h"

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
    bool is_epr;             ///< True when is_avs and the contract is EPR (APDO min > 9V)
    bool valid;
    // Programmable-contract fields (PPS or AVS)
    uint32_t programmable_min_mv;  // Active APDO range min voltage
    uint32_t programmable_max_mv;  // Active APDO range max voltage
};

enum class DetectedCableRating : uint8_t {
    EPR_CAPABLE,           ///< Source exposes an EPR rail (>21 V), implying an EPR-capable cable.
    CAPABLE_5A,            ///< A trustworthy >3 A contract confirms a 5 A cable.
    STANDARD_3A,           ///< Source is capped at 60 W with no trustworthy >3 A path, so a 3 A cable is likely.
    UNKNOWN_CHARGER_LIMIT, ///< Source tops out below 60 W, so the cable rating is not observable.
};

struct ChargerDiagInfo {
    const char* pd_revision;        // Example: "PD3.2", or "N/A" when unknown
    uint8_t cc_orientation;         // 0 = unknown, 1 = CC1, 2 = CC2
    bool supports_qc4;              // Inferred from PPS support
    bool supports_qc5;              // PPS + 100W-or-greater source capability
    bool charger_identity_valid;
    uint16_t charger_vendor_id;
    uint16_t charger_product_id;
    char charger_name[32];
    DetectedCableRating detected_cable_rating;
    uint32_t charger_max_power_w;   // Maximum power offered by the source
    bool charger_power_is_upper_bound;  // true when the value is a "<=" estimate
                                        // (non-PD USB-default source, real cap unknown)
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
    uint8_t getSourceCapabilities(TPS26750_SourceCapability* caps, uint8_t max_caps);

    // Number of cached PDOs (refreshes the cache if needed). Cheap alternative to
    // getSourceCapabilities() when only the count is needed for change detection —
    // avoids copying the whole PDO array on every render tick.
    uint8_t getPdoCount();

    // Request a specific contract (non-blocking)
    bool requestContract(const TPS26750_SourceCapability& pdo);
    bool requestFixedVoltage(uint32_t voltage_mv, uint32_t current_ma);
    /// @param pdo_index Index into the PDO cache of the selected APDO. Pass -1 when the
    ///        caller does not have an explicit index (startup restore, EPR exit) to fall back
    ///        to a first-match search.
    bool requestPpsVoltage(uint32_t voltage_mv, uint32_t current_ma, int8_t pdo_index = -1);
    bool requestAvsVoltage(uint32_t voltage_mv, uint32_t current_ma, int8_t pdo_index = -1);

    // Get negotiation state
    NegotiationState getNegotiationState() const { return _negotiation_state; }
    bool isNegotiating() const { return _negotiation_state == NegotiationState::REQUESTING; }

    // Get active contract info
    const ActiveContract& getActiveContract() const { return _active_contract; }
    bool refreshActiveContract();

    // Check if charger is connected
    bool isChargerConnected() const { return _charger_connected; }

    // Check if PPS contract is active
    bool isPpsActive() const { return _pps.active; }

    // Auto PPS tuning
    bool isPpsTuningActive() const;   // PPS active AND auto-tune enabled
    bool isPpsTuningConverged() const { return _pps.tuning_converged; }
    uint32_t getPpsUserTargetMv() const { return _pps.user_target_mv; }

    // Auto AVS tuning
    bool isAvsTuningActive() const;   // AVS active AND auto-tune enabled
    bool isAvsTuningConverged() const { return _avs.tuning_converged; }
    uint32_t getAvsUserTargetMv() const { return _avs.user_target_mv; }

    // Check if AVS contract is active
    bool isAvsActive() const { return _avs.active; }

    /// @brief Index of the active APDO in the PDO cache, or -1 if unknown/fixed contract.
    int8_t getActivePdoIndex() const { return _active_pdo_index; }

    /// @brief Minimum voltage [mV] of the active PPS APDO range (0 if not PPS).
    uint32_t getPpsRangeMinMv() const { return _pps.range_min_mv; }
    /// @brief Maximum voltage [mV] of the active PPS APDO range (0 if not PPS).
    uint32_t getPpsRangeMaxMv() const { return _pps.range_max_mv; }
    /// @brief Minimum voltage [mV] of the active AVS APDO range (0 if not AVS).
    uint32_t getAvsRangeMinMv() const { return _avs.range_min_mv; }
    /// @brief Maximum voltage [mV] of the active AVS APDO range (0 if not AVS).
    uint32_t getAvsRangeMaxMv() const { return _avs.range_max_mv; }

    // Update keep-alive voltage from CC controller (avoids fighting with CC regulation)
    // Only updates internal tracking, does NOT send a PD request
    void setCcKeepAliveVoltage(uint32_t voltage_mv);

    // Immediate convergence check (call after applying PPS/AVS voltage)
    void checkTuningConvergenceImmediate();

    // Get TPS26750 mode string
    bool getMode(char* mode_str);

    // Get PD revision string based on source capabilities
    // Returns "PD3.1" for AVS/EPR, "PD3.0" for PPS, "PD2.0" for fixed-only, "" for no PDOs
    const char* getPdRevision() const { return _pd_revision; }

    /**
     * @brief Build a UI-friendly diagnostic snapshot for the connected charger.
     * @param info Output structure populated with PD, charger identity, and inferred cable rating data.
     * @return true when a charger is connected and the snapshot reflects live hardware data.
     */
    bool getChargerDiagInfo(ChargerDiagInfo& info);

    /**
     * @brief Refresh the cached charger identity used by the About This Charger screen.
     * @details Sends a single Get_Manufacturer_Info request to the charger and stores the
     * resulting VID, PID, and brand name when available.
     */
    bool refreshChargerIdentity();

    // Startup contract negotiation based on settings
    // - Lowest: select lowest voltage fixed PDO
    // - Highest: select highest voltage fixed/AVS PDO
    // - Last: restore saved PDO, or find closest if unavailable
    // Returns true if a contract request was initiated
    bool negotiateStartupContract(bool allow_epr_wait = true);

    // Apply a best-effort early startup request before the boot UI delays the
    // normal restore path. This keeps the autonomous TPS26750 boot contract at
    // or below the remembered target until full PDO-based restore runs.
    bool primeStartupContract();

    // Wait for PDOs with timeout (non-blocking polling, call repeatedly in loop)
    // Returns true when PDOs are available, false if still waiting
    bool waitForPdos(uint32_t timeout_ms);

    // Check if PDOs have been discovered
    bool hasPdos() const { return _pdos_valid && _pdo_count > 0; }
    
    // Invalidate PDO cache (forces re-read from chip on next access)
    void invalidatePdoCache() { _pdos_valid = false; }
    
    // Force a probe for EPR capabilities (useful before opening menus)
    void probeEpr();

    // EPR safe exit: check if transitioning from EPR to SPR requires AVS step-down
    bool needsEprExit(uint32_t target_voltage_mv, bool target_is_pps) const;

    // Check if a safe EPR exit path exists (EPR AVS PDO with min <= 20V)
    bool isSafeEprExitPossible() const;

    // Startup request status helpers
    bool hasPendingRequestedContract() const;
    bool isRequestedContractSatisfied() const;
    bool shouldRetryStartupContractAfterEpr() const { return _startup_restore_waiting_for_epr; }

private:
    enum class RequestedContractType { NONE, FIXED, PPS, AVS };

    // Negotiation state
    NegotiationState _negotiation_state;
    absolute_time_t _negotiation_start;
    static constexpr uint32_t NEGOTIATION_TIMEOUT_MS = 2000;

    // Active contract cache
    ActiveContract _active_contract;

    // Connection state
    bool _charger_connected;

    // Cached PDO list
    TPS26750_SourceCapability _pdo_cache[AppConfig::MAX_PDO_COUNT];
    uint8_t _pdo_count;
    bool _pdos_valid;

    // Programmable (PPS/AVS) contract state + per-type configuration. PPS and AVS
    // share identical keep-alive, auto-tune, request, and clear logic; grouping the
    // state into one struct lets a single code path service both, ending the historical
    // "fixed it for PPS, forgot AVS" divergence. Instantiated once each as _pps/_avs.
    struct ProgrammableContract {
        // Per-type configuration (constant after construction)
        bool     is_avs;                 // false = PPS, true = AVS (selects HW profile + settings flag)
        uint32_t step_mv;                // Request voltage alignment step (PD spec)
        uint32_t refresh_ms;             // Keep-alive interval (spec requires <10s)
        int32_t  tune_threshold_mv;      // Converged when |error| <= this
        int32_t  tune_max_correction_mv; // Safety clamp on accumulated correction
        uint32_t tracking_tolerance_mv;  // Droop allowed before tracked state is dropped

        // Runtime state
        bool     active;                 // True if the current contract is this type
        uint32_t voltage_mv;             // Last requested voltage
        uint32_t current_ma;             // Last requested current
        absolute_time_t last_refresh;    // Time of last keep-alive request
        uint32_t user_target_mv;         // What the user asked for (auto-tune setpoint)
        int32_t  correction_mv;          // Accumulated auto-tune correction offset
        bool     tuning_converged;       // True when |error| <= tune_threshold_mv
        uint32_t range_min_mv;           // Active APDO min voltage (for clamping)
        uint32_t range_max_mv;           // Active APDO max voltage (for clamping)
    };

    ProgrammableContract _pps;
    ProgrammableContract _avs;

    // PD revision string (cached)
    char _pd_revision[8];

    // Charger identity cached from Get_Manufacturer_Info.
    bool _charger_identity_valid;
    uint16_t _charger_vendor_id;
    uint16_t _charger_product_id;
    char _charger_name[32];

    // Polling fallback state (for chargers that don't fire interrupt)
    uint32_t _pre_request_voltage_mv;
    uint32_t _pre_request_current_ma;
    RequestedContractType _requested_contract_type;
    uint32_t _requested_voltage_mv;
    uint32_t _requested_current_ma;
    static constexpr uint32_t POLLING_FALLBACK_MS = 500;
    static constexpr uint32_t PDO_RETRY_INTERVAL_MS = 500;           // Deferred PDO discovery retry
    static constexpr uint32_t TUNE_REQUEST_INTERVAL_MS = 1500;       // Faster retries while tuning is still converging
    static constexpr uint32_t TUNE_CONVERGENCE_CHECK_MS = 500;       // Fast convergence check interval
    static constexpr uint32_t CONTRACT_REFRESH_INTERVAL_MS = 1000;   // Periodic contract refresh
    static constexpr uint32_t MIN_TUNING_VOLTAGE_MV = 1000;          // Min voltage for tuning to engage
    static constexpr uint32_t FIXED_MATCH_TOLERANCE_MV = 50;
    static constexpr uint32_t PPS_MATCH_TOLERANCE_MV = 20;
    static constexpr uint32_t AVS_MATCH_TOLERANCE_MV = 25;

    // Index of the active PPS or AVS APDO in _pdo_cache (-1 = unknown / fixed contract)
    int8_t _active_pdo_index;

    // Startup restore coordination
    bool _startup_restore_waiting_for_epr;

    // Non-blocking timer state for update()/waitForPdos(). Member variables (not
    // function-local statics) so they reset on warm boot and waitForPdos() is
    // re-entrant across unplug/replug discovery cycles.
    absolute_time_t _next_pdo_retry;
    absolute_time_t _next_pps_convergence_check;
    absolute_time_t _next_avs_convergence_check;
    absolute_time_t _next_contract_refresh;
    absolute_time_t _wait_pdos_start;

    // EPR safe exit: 3-step sequence to avoid hard reset when exiting EPR to SPR
    // Step 1 (STEPPING_DOWN): AVS to min voltage (e.g. 15V) — reduces VBUS within EPR
    // Step 2 (REQUESTING_5V): Request 5V Fixed — cleanly exits EPR mode (no voltage rise)
    // Step 3 (REQUESTING_TARGET): Request user's actual SPR target (e.g. 20V) — standard SPR transition
    enum class EprExitState { NONE, STEPPING_DOWN, REQUESTING_5V, REQUESTING_TARGET };
    EprExitState _epr_exit_state;
    uint32_t _epr_deferred_voltage_mv;          // User's real target voltage
    uint32_t _epr_deferred_current_ma;          // User's real target current
    RequestedContractType _epr_deferred_contract_type;  // Deferred contract type for EPR exit step 3
    int8_t _epr_deferred_pdo_index;             // PDO cache index of user's target (-1 = fallback search)
    absolute_time_t _epr_exit_start;            // Timeout tracking for full sequence
    static constexpr uint32_t EPR_EXIT_TIMEOUT_MS = 5000;   // Total timeout for 3-step sequence
    static constexpr uint32_t EPR_EXIT_SAFE_MV = 5000;       // Intermediate 5V request voltage [mV]
    static constexpr uint32_t EPR_EXIT_SAFE_CURRENT_MA = 3000; // Intermediate 5V request current [mA]

    // EPR exit helpers
    bool findAvsSafeVoltage(uint32_t& avs_voltage_mv, uint32_t& avs_current_ma,
                            int8_t& avs_pdo_index) const;

    // Clear cached charger identity shown on the diagnostics screen.
    void clearChargerIdentity();

    // Detect PD revision from cached PDOs
    void detectPdRevision();

    // Clear tracked programmable contract state when the source no longer matches it
    void clearTracking(ProgrammableContract& contract);
    void clearRequestedContract();

    // Service one programmable contract's keep-alive + auto-tune cycle (PPS or AVS)
    void serviceKeepAlive(ProgrammableContract& contract);

    // Shared core for requestPpsVoltage()/requestAvsVoltage(): resolves APDO bounds,
    // sends the profile request, and updates tracking state on success.
    bool requestProgrammable(ProgrammableContract& self, ProgrammableContract& other,
                             uint32_t voltage_mv, uint32_t current_ma, int8_t pdo_index,
                             RequestedContractType type,
                             uint32_t default_min_mv, uint32_t default_max_mv);

    // Process PD interrupt events
    void handlePdInterrupt();

    // Check whether the active contract matches the pending request
    bool isRequestedContractReached() const;
};

// Global instance
extern PdManager pdManager;
