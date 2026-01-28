#pragma once

#include <cstdint>
#include "pico/stdlib.h"

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
    FLASH_EEPROM,
    ABOUT,
    MENU_COUNT  // Number of menu items
};

// Adjust modes when in ADJUST state
enum class AdjustMode {
    NONE,
    PDO_SELECT,     // Selecting a PDO from the list
    CURRENT_LIMIT,  // Adjusting current limit value
    EEPROM_FLASH,   // EEPROM flash workflow
    ABOUT           // Displaying about screen (read-only)
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

    // Get current fault type
    FaultType getFaultType() const { return _fault_type; }

    // Get selected menu item
    MenuItem getSelectedMenuItem() const { return _selected_menu_item; }

    // Get selected PDO index
    int8_t getSelectedPdoIndex() const { return _selected_pdo_index; }

    // Get current limit value in mA
    uint32_t getCurrentLimitMa() const { return _current_limit_ma; }

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

    // Boot sequence
    uint8_t _boot_stage;

    // Menu navigation
    MenuItem _selected_menu_item;
    int8_t _selected_pdo_index;
    int8_t _num_pdos;

    // Adjust state
    AdjustMode _adjust_mode;
    uint32_t _current_limit_ma;
    uint32_t _adjust_original_value;    // For cancellation

    // Fault handling
    FaultType _fault_type;
    float _fault_measured_value;
    float _fault_limit_value;

    // Encoder tracking
    int _last_encoder_ticks;
    int _encoder_delta;     // Accumulated ticks since last read (for acceleration)

    // EEPROM flash state
    uint8_t _eeprom_stage;        // 0=compare, 1=confirm, 2=flashing, 3=done
    uint8_t _eeprom_phase;        // 0=write, 1=verify
    uint8_t _eeprom_progress;     // 0-100%
    bool _eeprom_result;          // true=success, false=failure
    bool _eeprom_confirm_yes;     // Yes/No selection
    const char* _eeprom_message;  // Status message for display

    // State handlers
    void handleBootState();
    void handleMainState(EncoderEvent event);
    void handleMenuState(EncoderEvent event);
    void handleAdjustState(EncoderEvent event);
    void handleFaultState(EncoderEvent event);

    // State transitions
    void transitionTo(AppState new_state);

    // Input processing
    EncoderEvent readEncoderEvent();
    void handleOutputButtons();

    // Boot sequence helpers
    void advanceBootStage();

    // Menu/Adjust helpers
    void loadPdoList();
    void requestSelectedPdo();
    void applyCurrentLimit();

    // EEPROM flash helpers
    void startEepromCompare();
    void executeEepromFlash();

public:
    // EEPROM flash state accessors (for display manager)
    uint8_t getEepromStage() const { return _eeprom_stage; }
    uint8_t getEepromPhase() const { return _eeprom_phase; }
    uint8_t getEepromProgress() const { return _eeprom_progress; }
    bool getEepromResult() const { return _eeprom_result; }
    bool getEepromConfirmYes() const { return _eeprom_confirm_yes; }
    const char* getEepromMessage() const { return _eeprom_message; }

    // EEPROM progress callback (called from eeprom_loader)
    void setEepromProgress(uint8_t phase, uint8_t progress);
};

// Global instance
extern StateMachine stateMachine;
