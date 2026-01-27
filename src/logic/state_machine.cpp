#include "state_machine.h"
#include "hardware.h"
#include "interrupts.h"
#include "app_config.h"
#include "utils/logging.h"
#include "drivers/buzzer/buzzer.h"
#include "pd_manager.h"

// Global instance
StateMachine stateMachine;

// ============================================================================
// Boot Stage Messages
// ============================================================================
static const char* BOOT_MESSAGES[] = {
    "",                     // 0: Logo only
    "PD240W",              // 1: Product name
    "Power Supply",        // 2: Subtitle
    "",                     // 3: Playing melody
    "Reading USB-PD...",   // 4: Reading PD
    "",                     // 5: Show contracts
    "Ready"                // 6: Complete
};
static constexpr uint8_t BOOT_STAGE_COUNT = 7;

// Boot stage timing (cumulative milliseconds)
static const uint32_t BOOT_STAGE_TIMES[] = {
    0,      // 0: Logo
    100,    // 1: Product name
    200,    // 2: Subtitle
    300,    // 3: Start melody
    500,    // 4: Reading PD
    1500,   // 5: Show contracts
    2000    // 6: Complete -> transition to MAIN
};

// ============================================================================
// Constructor
// ============================================================================

StateMachine::StateMachine()
    : _state(AppState::BOOT)
    , _previous_state(AppState::BOOT)
    , _state_enter_time(nil_time)
    , _last_activity_time(nil_time)
    , _encoder_press_start(nil_time)
    , _encoder_button_held(false)
    , _boot_stage(0)
    , _selected_menu_item(MenuItem::SELECT_VOLTAGE)
    , _selected_pdo_index(0)
    , _num_pdos(0)
    , _adjust_mode(AdjustMode::NONE)
    , _current_limit_ma(AppConfig::CURRENT_LIMIT_DEFAULT_MA)
    , _adjust_original_value(0)
    , _fault_type(FaultType::NONE)
    , _fault_measured_value(0.0f)
    , _fault_limit_value(0.0f)
    , _last_encoder_ticks(0)
    , _encoder_delta(0)
{
}

// ============================================================================
// Initialization
// ============================================================================

void StateMachine::init() {
    _state_enter_time = get_absolute_time();
    _last_activity_time = get_absolute_time();
    _last_encoder_ticks = hw.encoder.getTicks();

    LOG_INFO("State machine initialized, starting BOOT sequence");
}

// ============================================================================
// Main Update Loop
// ============================================================================

bool StateMachine::update() {
    bool needs_refresh = false;

    // Handle output buttons (BTN1, BTN2) in all states except BOOT
    // Output must remain disabled during boot-up for safety
    if (_state != AppState::BOOT) {
        handleOutputButtons();
    }

    // Check for overcurrent (handled immediately by ISR, but we need to update state)
    if (Interrupts::handleOvercurrent()) {
        setFault(FaultType::OVERCURRENT);
        needs_refresh = true;
    }

    // Read encoder event
    EncoderEvent event = readEncoderEvent();

    // State-specific handling
    switch (_state) {
        case AppState::BOOT:
            handleBootState();
            needs_refresh = true;  // Boot always animates
            break;

        case AppState::MAIN:
            handleMainState(event);
            break;

        case AppState::MENU:
            handleMenuState(event);
            if (event != EncoderEvent::NONE) needs_refresh = true;
            break;

        case AppState::ADJUST:
            handleAdjustState(event);
            if (event != EncoderEvent::NONE) needs_refresh = true;
            break;

        case AppState::FAULT:
            handleFaultState(event);
            break;
    }

    // Check for menu timeout (return to MAIN after 30s inactivity)
    if (_state == AppState::MENU || _state == AppState::ADJUST) {
        int64_t idle_ms = absolute_time_diff_us(_last_activity_time, get_absolute_time()) / 1000;
        if (idle_ms > (int64_t)AppConfig::MENU_TIMEOUT_MS) {
            LOG_INFO("Menu timeout, returning to MAIN");
            transitionTo(AppState::MAIN);
            needs_refresh = true;
        }
    }

    return needs_refresh;
}

// ============================================================================
// State Handlers
// ============================================================================

void StateMachine::handleBootState() {
    uint32_t elapsed_ms = absolute_time_diff_us(_state_enter_time, get_absolute_time()) / 1000;

    // Advance boot stage based on timing
    while (_boot_stage < BOOT_STAGE_COUNT - 1 &&
           elapsed_ms >= BOOT_STAGE_TIMES[_boot_stage + 1]) {
        _boot_stage++;
        advanceBootStage();
    }

    // Check for boot complete
    if (elapsed_ms >= AppConfig::BOOT_DURATION_MS) {
        transitionTo(AppState::MAIN);
    }
}

void StateMachine::handleMainState(EncoderEvent event) {
    // Long press enters menu
    if (event == EncoderEvent::LONG_PRESS) {
        transitionTo(AppState::MENU);
    }
    // Click could toggle output (alternative to BTN1)
    // For now, we keep BTN1 as primary
}

void StateMachine::handleMenuState(EncoderEvent event) {
    switch (event) {
        case EncoderEvent::ROTATE_CW:
            // Move down in menu
            {
                int next = static_cast<int>(_selected_menu_item) + 1;
                if (next < static_cast<int>(MenuItem::MENU_COUNT)) {
                    _selected_menu_item = static_cast<MenuItem>(next);
                }
            }
            _last_activity_time = get_absolute_time();
            break;

        case EncoderEvent::ROTATE_CCW:
            // Move up in menu
            {
                int prev = static_cast<int>(_selected_menu_item) - 1;
                if (prev >= 0) {
                    _selected_menu_item = static_cast<MenuItem>(prev);
                }
            }
            _last_activity_time = get_absolute_time();
            break;

        case EncoderEvent::CLICK:
            // Select current menu item
            switch (_selected_menu_item) {
                case MenuItem::SELECT_VOLTAGE:
                    loadPdoList();
                    _adjust_mode = AdjustMode::PDO_SELECT;
                    transitionTo(AppState::ADJUST);
                    break;

                case MenuItem::CURRENT_LIMIT:
                    _adjust_original_value = _current_limit_ma;
                    // Clamp current value to effective max (contract may have changed)
                    {
                        uint32_t max_ma = getEffectiveMaxCurrentMa();
                        if (_current_limit_ma > max_ma) {
                            _current_limit_ma = max_ma;
                        }
                    }
                    _adjust_mode = AdjustMode::CURRENT_LIMIT;
                    transitionTo(AppState::ADJUST);
                    break;

                case MenuItem::ABOUT:
                    _adjust_mode = AdjustMode::ABOUT;
                    transitionTo(AppState::ADJUST);
                    break;

                default:
                    break;
            }
            _last_activity_time = get_absolute_time();
            break;

        case EncoderEvent::LONG_PRESS:
            // Return to main
            transitionTo(AppState::MAIN);
            break;

        default:
            break;
    }
}

void StateMachine::handleAdjustState(EncoderEvent event) {
    // About screen: any click or long press returns to menu
    if (_adjust_mode == AdjustMode::ABOUT) {
        if (event == EncoderEvent::CLICK || event == EncoderEvent::LONG_PRESS) {
            transitionTo(AppState::MENU);
        }
        return;
    }

    switch (event) {
        case EncoderEvent::ROTATE_CW:
            if (_adjust_mode == AdjustMode::PDO_SELECT) {
                if (_selected_pdo_index < _num_pdos - 1) {
                    _selected_pdo_index++;
                }
            } else if (_adjust_mode == AdjustMode::CURRENT_LIMIT) {
                // Acceleration: multiply step by number of accumulated ticks
                uint32_t abs_delta = (_encoder_delta > 0) ? _encoder_delta : 1;
                uint32_t step = AppConfig::CURRENT_LIMIT_STEP_MA * abs_delta;
                uint32_t max_ma = getEffectiveMaxCurrentMa();
                if (_current_limit_ma + step <= max_ma) {
                    _current_limit_ma += step;
                } else {
                    _current_limit_ma = max_ma;
                }
            }
            _last_activity_time = get_absolute_time();
            break;

        case EncoderEvent::ROTATE_CCW:
            if (_adjust_mode == AdjustMode::PDO_SELECT) {
                if (_selected_pdo_index > 0) {
                    _selected_pdo_index--;
                }
            } else if (_adjust_mode == AdjustMode::CURRENT_LIMIT) {
                // Acceleration: multiply step by number of accumulated ticks
                uint32_t abs_delta = (_encoder_delta < 0) ? -_encoder_delta : 1;
                uint32_t step = AppConfig::CURRENT_LIMIT_STEP_MA * abs_delta;
                if (_current_limit_ma > AppConfig::CURRENT_LIMIT_MIN_MA + step) {
                    _current_limit_ma -= step;
                } else {
                    _current_limit_ma = AppConfig::CURRENT_LIMIT_MIN_MA;
                }
            }
            _last_activity_time = get_absolute_time();
            break;

        case EncoderEvent::CLICK:
            // Confirm selection
            if (_adjust_mode == AdjustMode::PDO_SELECT) {
                requestSelectedPdo();
            } else if (_adjust_mode == AdjustMode::CURRENT_LIMIT) {
                applyCurrentLimit();
            }
            transitionTo(AppState::MENU);
            _last_activity_time = get_absolute_time();
            break;

        case EncoderEvent::LONG_PRESS:
            // Cancel and return to menu
            if (_adjust_mode == AdjustMode::CURRENT_LIMIT) {
                _current_limit_ma = _adjust_original_value;  // Restore original
            }
            transitionTo(AppState::MENU);
            break;

        default:
            break;
    }
}

void StateMachine::handleFaultState(EncoderEvent event) {
    // Click to acknowledge fault and return to main
    if (event == EncoderEvent::CLICK) {
        LOG_INFO("Fault acknowledged");
        _fault_type = FaultType::NONE;
        transitionTo(AppState::MAIN);
    }
}

// ============================================================================
// State Transitions
// ============================================================================

void StateMachine::transitionTo(AppState new_state) {
    if (_state == new_state) return;

    LOG_INFO("State transition: %d -> %d", static_cast<int>(_state), static_cast<int>(new_state));

    _previous_state = _state;
    _state = new_state;
    _state_enter_time = get_absolute_time();
    _last_activity_time = get_absolute_time();

    // State entry actions
    switch (new_state) {
        case AppState::BOOT:
            _boot_stage = 0;
            break;

        case AppState::MAIN:
            _adjust_mode = AdjustMode::NONE;
            pdManager.refreshActiveContract();  // Ensure fresh contract data for display
            hw.rgbLed.setColor(0, 255, 0, 50);  // Green = ready
            // Drain any button presses that occurred during BOOT
            if (_previous_state == AppState::BOOT) {
                Interrupts::checkBtn1Clicked();
                Interrupts::checkBtn2Clicked();
            }
            break;

        case AppState::MENU:
            _selected_menu_item = MenuItem::SELECT_VOLTAGE;
            hw.rgbLed.setColor(0, 0, 255, 50);  // Blue = menu
            break;

        case AppState::ADJUST:
            hw.rgbLed.setColor(255, 255, 0, 50);  // Yellow = adjusting
            break;

        case AppState::FAULT:
            hw.rgbLed.setColor(255, 0, 0, 255);  // Red = fault
            hw.buzzer.playTone(1000, 500);  // Alert beep
            break;
    }
}

// ============================================================================
// Input Processing
// ============================================================================

EncoderEvent StateMachine::readEncoderEvent() {
    EncoderEvent event = EncoderEvent::NONE;

    // Check encoder rotation
    int current_ticks = hw.encoder.getTicks();
    int delta = current_ticks - _last_encoder_ticks;
    _encoder_delta = delta;  // Store for acceleration (used by current limit adjust)

    if (delta > 0) {
        event = EncoderEvent::ROTATE_CW;
        _last_encoder_ticks = current_ticks;
    } else if (delta < 0) {
        event = EncoderEvent::ROTATE_CCW;
        _last_encoder_ticks = current_ticks;
    }

    // Check encoder button for long press
    bool button_pressed = hw.btnEnc.isPressed();

    if (button_pressed && !_encoder_button_held) {
        // Button just pressed - start timing
        _encoder_press_start = get_absolute_time();
        _encoder_button_held = true;
    } else if (!button_pressed && _encoder_button_held) {
        // Button released
        uint32_t press_duration = absolute_time_diff_us(_encoder_press_start, get_absolute_time()) / 1000;

        if (press_duration >= AppConfig::ENCODER_LONG_PRESS_MS) {
            event = EncoderEvent::LONG_PRESS;
        } else if (press_duration > 50) {  // Debounce threshold
            event = EncoderEvent::CLICK;
        }

        _encoder_button_held = false;
    }

    return event;
}

void StateMachine::handleOutputButtons() {
    // BTN1: Toggle load switch
    // Use ISR-based detection for reliable quick press capture
    if (Interrupts::checkBtn1Clicked()) {
        // Clear INA228 fault latch before enabling
        hw.powerMonitor.getDiagnoseAlert();

        bool current_state = hw.loadSwitch.read();
        if (current_state) {
            hw.loadSwitch.off();
            LOG_INFO("Load switch DISABLED (BTN1)");
        } else {
            hw.loadSwitch.on();
            LOG_INFO("Load switch ENABLED (BTN1)");
        }
    }

    // BTN2: Toggle 17V buck (only if VBUS > 18V)
    // Use ISR-based detection for reliable quick press capture
    if (Interrupts::checkBtn2Clicked()) {
        // Check VBUS voltage via INA228
        float vbus_mv = hw.powerMonitor.getBusVoltage() * 1000.0f;

        if (vbus_mv >= AppConfig::MIN_VBUS_FOR_17V_MV) {
            bool current_state = hw.EN_17V.read();
            if (current_state) {
                hw.EN_17V.off();
                LOG_INFO("17V buck DISABLED (BTN2)");
            } else {
                hw.EN_17V.on();
                LOG_INFO("17V buck ENABLED (BTN2)");
            }
        } else {
            LOG_WARN("Cannot enable 17V buck: VBUS=%.1fV < 18V", vbus_mv / 1000.0f);
            hw.buzzer.playTone(200, 100);  // Error beep
        }
    }
}

// ============================================================================
// Fault Handling
// ============================================================================

void StateMachine::setFault(FaultType fault) {
    if (_state == AppState::FAULT && fault == _fault_type) {
        return;  // Already in this fault state
    }

    _fault_type = fault;

    // Store fault values for display
    switch (fault) {
        case FaultType::OVERCURRENT:
            _fault_measured_value = hw.powerMonitor.getCurrent();
            _fault_limit_value = 5.0f;  // 5A max
            LOG_ERROR("FAULT: Overcurrent - %.2fA (limit %.2fA)",
                     _fault_measured_value, _fault_limit_value);
            break;

        case FaultType::OVERTEMPERATURE:
            _fault_measured_value = hw.adc.getTemperature();
            _fault_limit_value = static_cast<float>(AppConfig::TEMP_SHUTDOWN_C);
            LOG_ERROR("FAULT: Overtemperature - %.1fC (limit %.1fC)",
                     _fault_measured_value, _fault_limit_value);
            break;

        case FaultType::PD_DISCONNECT:
            LOG_ERROR("FAULT: USB-PD disconnected");
            break;

        default:
            break;
    }

    // Disable load switch on any fault
    hw.loadSwitch.off();

    transitionTo(AppState::FAULT);
}

// ============================================================================
// Boot Sequence Helpers
// ============================================================================

void StateMachine::advanceBootStage() {
    switch (_boot_stage) {
        case 3:
            // Start Mario power-up melody
            //hw.buzzer.playMelody(MARIO_POWERUP, MARIO_POWERUP_LENGTH);
            break;

        case 4:
            // Read USB-PD source capabilities
            loadPdoList();
            break;

        default:
            break;
    }
}

uint8_t StateMachine::getBootProgress() const {
    if (_state != AppState::BOOT) return 100;

    uint32_t elapsed_ms = absolute_time_diff_us(_state_enter_time, get_absolute_time()) / 1000;
    uint32_t progress = (elapsed_ms * 100) / AppConfig::BOOT_DURATION_MS;
    return (progress > 100) ? 100 : static_cast<uint8_t>(progress);
}

const char* StateMachine::getBootStageMessage() const {
    if (_boot_stage < BOOT_STAGE_COUNT) {
        return BOOT_MESSAGES[_boot_stage];
    }
    return "";
}

// ============================================================================
// PDO Management Helpers
// ============================================================================

// Storage for PDO list (shared with display)
static SourceCapability s_pdo_list[13];

void StateMachine::loadPdoList() {
    _num_pdos = hw.pdController.getSourceCapabilities(s_pdo_list, 13);
    _selected_pdo_index = 0;

    LOG_INFO("Loaded %d PDOs from charger", _num_pdos);
}

void StateMachine::requestSelectedPdo() {
    if (_selected_pdo_index < 0 || _selected_pdo_index >= _num_pdos) {
        LOG_ERROR("Invalid PDO index: %d", _selected_pdo_index);
        return;
    }

    SourceCapability& pdo = s_pdo_list[_selected_pdo_index];

    LOG_INFO("Requesting PDO[%d]: %umV @ %umA (PPS=%d, AVS=%d)",
             _selected_pdo_index, pdo.voltage_mv, pdo.max_current_ma,
             pdo.is_pps, pdo.is_avs);

    // Route through PD manager for proper negotiation tracking
    bool success = pdManager.requestContract(pdo);

    if (success) {
        LOG_INFO("PDO request sent successfully");
        hw.buzzer.playTone(1000, 50);  // Confirmation beep
    } else {
        LOG_ERROR("Failed to request PDO");
        hw.buzzer.playTone(200, 200);  // Error beep
    }
}

// ============================================================================
// Current Limit Helpers
// ============================================================================

void StateMachine::applyCurrentLimit() {
    LOG_INFO("Current limit set to %u mA", _current_limit_ma);

    // TODO: Apply to INA228 alert threshold
    // For now, just store the value - it will be checked in safety module

    hw.buzzer.playTone(1000, 50);  // Confirmation beep
}

uint32_t StateMachine::getEffectiveMaxCurrentMa() const {
    const ActiveContract& contract = pdManager.getActiveContract();
    if (contract.valid && contract.current_ma > 0) {
        // Cap to the lesser of hardware max and contract max
        return (contract.current_ma < AppConfig::CURRENT_LIMIT_MAX_MA)
               ? contract.current_ma
               : AppConfig::CURRENT_LIMIT_MAX_MA;
    }
    // No valid contract - use hardware max
    return AppConfig::CURRENT_LIMIT_MAX_MA;
}

// Expose PDO list for display manager
const SourceCapability* getPdoList() {
    return s_pdo_list;
}

uint8_t getPdoCount() {
    return stateMachine.getSelectedPdoIndex() >= 0 ?
           static_cast<uint8_t>(stateMachine.getSelectedPdoIndex() + 1) : 0;
}
