#pragma once

#include <cstdint>
#include "pico/stdlib.h"
#include "tps26750.h"
#include "utils/tps_patch_loader.h"

// ============================================================================
// Application State Machine
// ============================================================================
// Controls the overall application flow with Prusa-style navigation:
// - Encoder rotate: Navigate/adjust values
// - Encoder click: Confirm/select
// - Encoder long press (800ms): Go back/exit
// - BTN1: Toggle load switch (any state)
// - BTN2: Toggle 17V buck (any state, only if VBUS > 18V)
// ============================================================================

// Application states
enum class AppState {
    BOOT,       // Startup: logo, version, melody (3s)
    MAIN,       // Real-time monitoring display
    MENU,       // PDO selection / settings navigation
    ADJUST,     // Adjusting a value (voltage/current)
    FAULT       // Error display, needs acknowledgment
};

// Menu items in MENU state
enum class MenuItem {
    SELECT_VOLTAGE,
    CURRENT_LIMIT,
    SETTINGS,
    ABOUT,
    ABOUT_CHARGER,
    BACK,           // Back to main screen
    MENU_COUNT      // Number of menu items
};

// Settings submenu items
enum class SettingsItem {
    FLASH_EEPROM,
    AUTO_PPS,
    AUTO_AVS,
    AUTO_OUTPUT,
    BRIGHTNESS,
    DIM_TIMEOUT,
    STARTUP_MELODY,
    STARTUP_CONTRACT,  // Startup contract negotiation mode
    SOUNDS,
    BACK,           // Back to main menu
    SETTINGS_COUNT  // Number of settings items
};

// Adjust modes when in ADJUST state
enum class AdjustMode {
    NONE,
    PDO_SELECT,         // Selecting a PDO from the list
    CURRENT_LIMIT,      // Adjusting current limit value
    PPS_VOLTAGE,        // Adjusting PPS voltage within range
    AVS_VOLTAGE,        // Adjusting AVS voltage within range
    EEPROM_FLASH,       // EEPROM flash workflow
    ABOUT,              // Displaying about screen (read-only)
    ABOUT_CHARGER,      // Displaying charger diagnostics (read-only)
    SETTINGS_MENU       // Settings submenu navigation
};

// Fault types
enum class FaultType {
    NONE,
    OVERCURRENT,
    OVERTEMPERATURE,
    PD_DISCONNECT
};

// Encoder event types (processed in state machine)
enum class EncoderEvent {
    NONE,
    ROTATE_CW,      // Clockwise rotation
    ROTATE_CCW,     // Counter-clockwise rotation
    CLICK,          // Short press
    LONG_PRESS      // Long press (800ms)
};

// Result of a shared output-control request (see setLoadSwitch/set17vBuck).
// Lets the CLI translate a rejection into a protocol error while the front-panel
// buttons can simply ignore the reason.
enum class OutputResult {
    OK,
    FAULT_ACTIVE,   // load switch cannot be enabled while a fault is latched
    NOT_AVAILABLE   // 17V buck requires VBUS >= MIN_VBUS_FOR_17V_MV
};

class StateMachine {
public:
    StateMachine();

    // Initialize the state machine (call after hw.init())
    void init();

    // Main update function - call every iteration of main loop
    // Returns true if display needs refresh
    bool update();

    // Get current state
    AppState getState() const { return _state; }

    // Set fault state (called from safety module or interrupts)
    void setFault(FaultType fault);

    // Shared output-control operations used by BOTH the front-panel buttons and
    // the CLI so the policy (fault gating, INA228 latch clear, tuning recheck,
    // 17V VBUS interlock) lives in one place and cannot drift between the two
    // entry points. `set17vBuck` uses the pre-switch ADC VBUS, not INA228.
    OutputResult setLoadSwitch(bool on);
    OutputResult set17vBuck(bool on);

    // Get current fault type
    FaultType getFaultType() const { return _fault_type; }

    // Get selected menu item
    MenuItem getSelectedMenuItem() const { return _selected_menu_item; }

    // Get selected settings item
    SettingsItem getSelectedSettingsItem() const { return _selected_settings_item; }

    // Get selected PDO index
    int8_t getSelectedPdoIndex() const { return _selected_pdo_index; }

    // Get current limit value in mA
    uint32_t getCurrentLimitMa() const { return _current_limit_ma; }

    // Synchronize the cached current limit value from CLI or restored settings
    void setCurrentLimitMa(uint32_t limit_ma);

    // Get adjust mode
    AdjustMode getAdjustMode() const { return _adjust_mode; }

    // Get encoder delta (number of ticks since last read, for acceleration)
    int getEncoderDelta() const { return _encoder_delta; }

    // Get the effective max current limit (capped by active contract)
    uint32_t getEffectiveMaxCurrentMa() const;

    // Get boot progress (0-100%)
    uint8_t getBootProgress() const;

    // Get boot stage message
    const char* getBootStageMessage() const;

private:
    // Current state
    AppState _state;
    AppState _previous_state;

    // Timing
    absolute_time_t _state_enter_time;
    absolute_time_t _last_activity_time;
    absolute_time_t _encoder_press_start;
    bool _encoder_button_held;
    bool _screen_dimmed;  // True when auto-dim is active

    // Boot sequence
    uint8_t _boot_stage;
    bool _boot_pdos_found;              // True once PDOs loaded with results
    bool _boot_contract_requested;      // True once startup contract negotiation initiated
    bool _boot_contract_complete;       // True once negotiation finished (success or timeout)
    bool _boot_retry_after_epr;         // True when LAST_USED must be retried after EPR PDO discovery
    bool _boot_epr_probed;              // True once EPR probe sent after negotiation
    absolute_time_t _boot_epr_probe_time; // When EPR probe was sent
    uint32_t _boot_last_epr_poll_ms;    // Elapsed time of last EPR PDO poll during boot
    bool _boot_epr_pdos_found;          // True once EPR PDOs appeared during boot probe
    absolute_time_t _boot_ready_time;   // When "Ready!" was first shown (for adaptive exit)
    absolute_time_t _boot_neg_start;    // When contract negotiation started (for timeout)

    // PD-config push (RP2040 -> TPS26750 over I2C0) performed before PDO discovery.
    // Pushes the embedded patch bundle when the TPS booted in PTCH mode (blank
    // EEPROM / future EEPROM-less board); skips instantly when already in APP mode.
    TpsPatchSession _boot_patch_session;
    bool _boot_patch_active;            // True while the push runs (drives loading bar/message)
    bool _boot_patch_done;             // True once the push has been attempted (run once)
    uint8_t _boot_patch_progress;      // Push progress 0-100 (for loading bar)

    // Menu navigation
    MenuItem _selected_menu_item;
    SettingsItem _selected_settings_item;
    int8_t _selected_pdo_index;
    int8_t _num_pdos;

    // Adjust state
    AdjustMode _adjust_mode;
    uint32_t _current_limit_ma;

    // Non-blocking EPR probe (replaces a blocking sleep_ms after probeEpr() in the
    // menu). When the user opens "Select Voltage" / "About Charger" we send the
    // probe, record the time, and keep pumping the main loop until the charger has
    // had EPR_PROBE_DELAY_MS to respond, then load PDOs and enter the target screen.
    bool _epr_probe_pending;
    absolute_time_t _epr_probe_start;
    AdjustMode _epr_probe_target;       // PDO_SELECT or ABOUT_CHARGER once the probe completes

    // Fault handling
    FaultType _fault_type;
    float _fault_measured_value;
    float _fault_limit_value;

    // Encoder tracking
    int _last_encoder_ticks;
    int _encoder_delta;     // Accumulated ticks since last read (for acceleration)

    // PPS adjustment state
    uint32_t _pps_target_voltage_mv;    // Current target voltage
    uint32_t _pps_min_voltage_mv;       // Min voltage from PPS PDO
    uint32_t _pps_max_voltage_mv;       // Max voltage from PPS PDO
    uint32_t _pps_max_current_ma;       // Max current from PPS PDO
    uint8_t _pps_pdo_index;             // Index of selected PPS PDO

    // AVS adjustment state
    uint32_t _avs_target_voltage_mv;    // Current target voltage
    uint32_t _avs_min_voltage_mv;       // Min voltage from AVS PDO
    uint32_t _avs_max_voltage_mv;       // Max voltage from AVS PDO
    uint32_t _avs_max_current_ma;       // Max current from AVS PDO
    uint8_t _avs_pdo_index;             // Index of selected AVS PDO

    // State handlers
    void handleBootState();

    // Push the PD config to the TPS26750 over I2C (blocking, animates the boot
    // loading bar). No-op/instant skip when the TPS is already in APP mode.
    void runPatchPush();
    void handleMainState(EncoderEvent event);
    void handleMenuState(EncoderEvent event);
    void handleAdjustState(EncoderEvent event);
    void handleFaultState(EncoderEvent event);

    // State transitions
    void transitionTo(AppState new_state);

    // Input processing
    EncoderEvent readEncoderEvent();
    void handleOutputButtons();

    // Play the navigation beep if sounds are enabled (shared by the menu handlers).
    void playNavBeep();

    // Menu/Adjust helpers
    void loadPdoList();
    void requestSelectedPdo();
    void applyCurrentLimit();
    void applyPpsVoltage();
    void applyAvsVoltage();
    void handleSettingsMenuState(EncoderEvent event);
    void saveStartupContractSnapshot(const TPS26750_SourceCapability& pdo,
                                     int8_t pdo_index,
                                     uint32_t requested_voltage_mv);
    bool saveCurrentContractSnapshot();

    // Brightness adjustment state
    uint8_t _brightness_value;      // Current brightness during adjustment
    bool _brightness_adjusting;     // True when in brightness adjust mode (click to toggle)

    // Dim timeout adjustment state
    uint8_t _dim_timeout_value;     // Current dim timeout during adjustment (minutes)
    bool _dim_timeout_adjusting;    // True when in dim timeout adjust mode

    // Startup melody adjustment state
    uint8_t _melody_value;          // Current melody index during adjustment
    bool _melody_adjusting;         // True when in melody adjust mode

    // Startup contract mode adjustment state
    uint8_t _contract_mode_value;   // Current contract mode during adjustment (0-2)
    bool _contract_mode_adjusting;  // True when in contract mode adjust mode

    // Energy display mode (toggled by long press on MAIN when no PPS/AVS)
    bool _energy_display_mwh;       // false = mAh (default), true = mWh

public:
    // Brightness state accessors (for display manager)
    uint8_t getBrightnessValue() const { return _brightness_value; }
    bool isBrightnessAdjusting() const { return _brightness_adjusting; }

    // Dim timeout state accessors (for display manager)
    uint8_t getDimTimeoutValue() const { return _dim_timeout_value; }
    bool isDimTimeoutAdjusting() const { return _dim_timeout_adjusting; }

    // Melody state accessors (for display manager)
    uint8_t getMelodyValue() const { return _melody_value; }
    bool isMelodyAdjusting() const { return _melody_adjusting; }

    // Contract mode state accessors (for display manager)
    uint8_t getContractModeValue() const { return _contract_mode_value; }
    bool isContractModeAdjusting() const { return _contract_mode_adjusting; }

    // Energy display mode toggle (long press on MAIN when no PPS/AVS)
    bool isEnergyDisplayMwh() const { return _energy_display_mwh; }

    // Fault state accessors (for display manager)
    float getFaultLimitValue() const { return _fault_limit_value; }

    // PPS state accessors (for display manager)
    uint32_t getPpsTargetVoltageMv() const { return _pps_target_voltage_mv; }
    uint32_t getPpsMinVoltageMv() const { return _pps_min_voltage_mv; }
    uint32_t getPpsMaxVoltageMv() const { return _pps_max_voltage_mv; }
    uint32_t getPpsMaxCurrentMa() const { return _pps_max_current_ma; }

    // AVS state accessors (for display manager)
    uint32_t getAvsTargetVoltageMv() const { return _avs_target_voltage_mv; }
    uint32_t getAvsMinVoltageMv() const { return _avs_min_voltage_mv; }
    uint32_t getAvsMaxVoltageMv() const { return _avs_max_voltage_mv; }
    uint32_t getAvsMaxCurrentMa() const { return _avs_max_current_ma; }
};

// Global instance
extern StateMachine stateMachine;
