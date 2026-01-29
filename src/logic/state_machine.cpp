#include "state_machine.h"
#include "hardware.h"
#include "interrupts.h"
#include "app_config.h"
#include "utils/logging.h"
#include "utils/eeprom_loader.h"
#include "drivers/buzzer/buzzer.h"
#include "pd_manager.h"
#include "ui/display_manager.h"

// Global instance
StateMachine stateMachine;

// ============================================================================
// Boot Stage Configuration
// ============================================================================
// Simplified boot sequence:
//   Stage 0 (0ms):    Logo displayed, melody plays
//   Stage 1 (500ms):  Read USB-PD contracts, show "Reading USB-PD..."
//   Stage 2 (1500ms): Show "Ready"
//   At 2000ms:        Transition to MAIN

static const char* BOOT_MESSAGES[] = {
    "",                    // 0: Logo only (melody plays)
    "Reading USB-PD...",   // 1: Reading PD contracts
    "Ready!"                // 2: Complete
};
static constexpr uint8_t BOOT_STAGE_COUNT = 3;

// Boot stage timing (cumulative milliseconds)
static const uint32_t BOOT_STAGE_TIMES[] = {
    0,      // 0: Logo + start melody
    500,    // 1: Read USB-PD
    1500    // 2: Ready
};

// Storage for PDO list (shared with display)
static SourceCapability s_pdo_list[13];

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
    , _pps_target_voltage_mv(0)
    , _pps_min_voltage_mv(0)
    , _pps_max_voltage_mv(0)
    , _pps_max_current_ma(0)
    , _pps_pdo_index(0)
    , _eeprom_stage(0)
    , _eeprom_phase(0)
    , _eeprom_progress(0)
    , _eeprom_result(false)
    , _eeprom_confirm_yes(false)
    , _eeprom_message(nullptr)
{
}

// ============================================================================
// Initialization
// ============================================================================

void StateMachine::init() {
    _state_enter_time = get_absolute_time();
    _last_activity_time = get_absolute_time();
    _last_encoder_ticks = hw.encoder.getTicks();

    // Start Mario power-up melody at boot
    hw.buzzer.playMelody(MARIO_POWERUP, MARIO_POWERUP_LENGTH);

    LOG_INFO("State machine initialized, starting BOOT sequence");
}

// ============================================================================
// Main Update Loop
// ============================================================================

bool StateMachine::update() {
    bool needs_refresh = false;

    // Handle output buttons (BTN1, BTN2) in all states except BOOT and FAULT
    // Output must remain disabled during boot-up for safety
    // During FAULT, outputs are disabled and must not be toggled
    if (_state != AppState::BOOT && _state != AppState::FAULT) {
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
    // Long press or click enters menu
    if (event == EncoderEvent::LONG_PRESS || event == EncoderEvent::CLICK) {
        hw.buzzer.playTone(1200, 30);  // Menu entry beep
        transitionTo(AppState::MENU);
    }
}

void StateMachine::handleMenuState(EncoderEvent event) {
    switch (event) {
        case EncoderEvent::ROTATE_CW:
            // Move down in menu (with wrap-around)
            {
                int next = static_cast<int>(_selected_menu_item) + 1;
                if (next >= static_cast<int>(MenuItem::MENU_COUNT)) {
                    next = 0;  // Wrap to first item
                }
                _selected_menu_item = static_cast<MenuItem>(next);
            }
            _last_activity_time = get_absolute_time();
            break;

        case EncoderEvent::ROTATE_CCW:
            // Move up in menu (with wrap-around)
            {
                int prev = static_cast<int>(_selected_menu_item) - 1;
                if (prev < 0) {
                    prev = static_cast<int>(MenuItem::MENU_COUNT) - 1;  // Wrap to last item
                }
                _selected_menu_item = static_cast<MenuItem>(prev);
            }
            _last_activity_time = get_absolute_time();
            break;

        case EncoderEvent::CLICK:
            // Select current menu item
            switch (_selected_menu_item) {
                case MenuItem::SELECT_VOLTAGE:
                    hw.buzzer.playTone(1400, 30);  // Submenu beep
                    loadPdoList();
                    _adjust_mode = AdjustMode::PDO_SELECT;
                    transitionTo(AppState::ADJUST);
                    break;

                case MenuItem::CURRENT_LIMIT:
                    hw.buzzer.playTone(1400, 30);  // Submenu beep
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

                case MenuItem::FLASH_EEPROM:
                    hw.buzzer.playTone(1400, 30);  // Submenu beep
                    _adjust_mode = AdjustMode::EEPROM_FLASH;
                    _eeprom_stage = 0;  // Start with compare stage
                    _eeprom_message = "Initializing...";
                    _eeprom_progress = 0;
                    _eeprom_result = false;
                    _eeprom_confirm_yes = false;
                    transitionTo(AppState::ADJUST);
                    startEepromCompare();
                    break;

                case MenuItem::ABOUT:
                    hw.buzzer.playTone(1400, 30);  // Submenu beep
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
            hw.buzzer.playTone(1000, 30);  // Exit beep
            transitionTo(AppState::MENU);
        }
        return;
    }

    // EEPROM flash mode: special handling based on stage
    if (_adjust_mode == AdjustMode::EEPROM_FLASH) {
        switch (_eeprom_stage) {
            case 0:  // Compare stage - show result, no interaction yet
                // Handled by startEepromCompare(), wait for it to set stage=1
                break;

            case 1:  // Confirm stage - Yes/No selection
                if (event == EncoderEvent::ROTATE_CW || event == EncoderEvent::ROTATE_CCW) {
                    _eeprom_confirm_yes = !_eeprom_confirm_yes;
                    _last_activity_time = get_absolute_time();
                } else if (event == EncoderEvent::CLICK) {
                    if (_eeprom_confirm_yes) {
                        // User confirmed - start flash
                        _eeprom_stage = 2;  // Flashing
                        _eeprom_message = "Flashing...";
                        _eeprom_progress = 0;
                        executeEepromFlash();
                    } else {
                        // User cancelled
                        hw.buzzer.playTone(1000, 30);  // Cancel beep
                        eepromDeinit();
                        transitionTo(AppState::MENU);
                    }
                    _last_activity_time = get_absolute_time();
                } else if (event == EncoderEvent::LONG_PRESS) {
                    // Cancel
                    hw.buzzer.playTone(1000, 30);  // Cancel beep
                    eepromDeinit();
                    transitionTo(AppState::MENU);
                }
                break;

            case 2:  // Flashing stage - no user interaction (blocking)
                // Progress updates come from the callback
                break;

            case 3:  // Done stage - show result, click to exit
                if (event == EncoderEvent::CLICK || event == EncoderEvent::LONG_PRESS) {
                    hw.buzzer.playTone(1000, 30);  // Exit beep
                    eepromDeinit();
                    transitionTo(AppState::MENU);
                }
                break;
        }
        return;
    }

    switch (event) {
        case EncoderEvent::ROTATE_CW:
            if (_adjust_mode == AdjustMode::PDO_SELECT) {
                if (_selected_pdo_index < _num_pdos - 1) {
                    _selected_pdo_index++;
                } else {
                    _selected_pdo_index = 0;  // Wrap to first
                }
            } else if (_adjust_mode == AdjustMode::CURRENT_LIMIT) {
                // Velocity-based acceleration for current limit (smaller range: 0-5A)
                uint32_t velocity_mult = (hw.encoder.getVelocityMultiplier() + AppConfig::CURRENT_LIMIT_VELOCITY_DIV - 1) 
                                         / AppConfig::CURRENT_LIMIT_VELOCITY_DIV;
                if (velocity_mult < 1) velocity_mult = 1;
                uint32_t step = AppConfig::CURRENT_LIMIT_STEP_MA * velocity_mult;
                uint32_t max_ma = getEffectiveMaxCurrentMa();
                if (_current_limit_ma + step <= max_ma) {
                    _current_limit_ma += step;
                } else {
                    _current_limit_ma = max_ma;
                }
            } else if (_adjust_mode == AdjustMode::PPS_VOLTAGE) {
                // PPS voltage: Use velocity-based acceleration (larger range: up to 21V)
                uint32_t velocity_mult = hw.encoder.getVelocityMultiplier() * AppConfig::PPS_VELOCITY_MULT;
                uint32_t step = AppConfig::PPS_VOLTAGE_STEP_MV * velocity_mult;
                if (_pps_target_voltage_mv + step <= _pps_max_voltage_mv) {
                    _pps_target_voltage_mv += step;
                } else {
                    _pps_target_voltage_mv = _pps_max_voltage_mv;
                }
                // Round to 20mV boundary (PD spec requirement)
                _pps_target_voltage_mv = (_pps_target_voltage_mv / 20) * 20;
            }
            _last_activity_time = get_absolute_time();
            break;

        case EncoderEvent::ROTATE_CCW:
            if (_adjust_mode == AdjustMode::PDO_SELECT) {
                if (_selected_pdo_index > 0) {
                    _selected_pdo_index--;
                } else {
                    _selected_pdo_index = _num_pdos - 1;  // Wrap to last
                }
            } else if (_adjust_mode == AdjustMode::CURRENT_LIMIT) {
                // Velocity-based acceleration for current limit (smaller range: 0-5A)
                uint32_t velocity_mult = (hw.encoder.getVelocityMultiplier() + AppConfig::CURRENT_LIMIT_VELOCITY_DIV - 1) 
                                         / AppConfig::CURRENT_LIMIT_VELOCITY_DIV;
                if (velocity_mult < 1) velocity_mult = 1;
                uint32_t step = AppConfig::CURRENT_LIMIT_STEP_MA * velocity_mult;
                if (_current_limit_ma > AppConfig::CURRENT_LIMIT_MIN_MA + step) {
                    _current_limit_ma -= step;
                } else {
                    _current_limit_ma = AppConfig::CURRENT_LIMIT_MIN_MA;
                }
            } else if (_adjust_mode == AdjustMode::PPS_VOLTAGE) {
                // PPS voltage: Use velocity-based acceleration (larger range: up to 21V)
                uint32_t velocity_mult = hw.encoder.getVelocityMultiplier() * AppConfig::PPS_VELOCITY_MULT;
                uint32_t step = AppConfig::PPS_VOLTAGE_STEP_MV * velocity_mult;
                if (_pps_target_voltage_mv >= _pps_min_voltage_mv + step) {
                    _pps_target_voltage_mv -= step;
                } else {
                    _pps_target_voltage_mv = _pps_min_voltage_mv;
                }
                // Round to 20mV boundary (PD spec requirement)
                _pps_target_voltage_mv = (_pps_target_voltage_mv / 20) * 20;
            }
            _last_activity_time = get_absolute_time();
            break;

        case EncoderEvent::CLICK:
            // Confirm selection
            if (_adjust_mode == AdjustMode::PDO_SELECT) {
                // If no PDOs, click returns to menu
                if (_num_pdos == 0) {
                    hw.buzzer.playTone(1000, 30);  // Exit beep
                    transitionTo(AppState::MENU);
                    _last_activity_time = get_absolute_time();
                    break;
                }
                // Check if selected PDO is PPS - if so, enter voltage adjustment mode
                if (_selected_pdo_index >= 0 && _selected_pdo_index < _num_pdos) {
                    SourceCapability& pdo = s_pdo_list[_selected_pdo_index];
                    if (pdo.is_pps) {
                        // Enter PPS voltage adjustment mode
                        _pps_pdo_index = _selected_pdo_index;
                        _pps_min_voltage_mv = pdo.min_voltage_mv;
                        _pps_max_voltage_mv = pdo.voltage_mv;
                        _pps_max_current_ma = pdo.max_current_ma;
                        // Start at mid-range voltage
                        _pps_target_voltage_mv = (_pps_min_voltage_mv + _pps_max_voltage_mv) / 2;
                        // Round to 20mV step (PPS resolution)
                        _pps_target_voltage_mv = (_pps_target_voltage_mv / 20) * 20;
                        _adjust_mode = AdjustMode::PPS_VOLTAGE;
                        LOG_INFO("Entering PPS voltage adjustment: %u-%umV", _pps_min_voltage_mv, _pps_max_voltage_mv);
                        // Force display redraw since we changed mode within same state
                        displayManager.invalidate();
                    } else {
                        // Fixed or AVS - request immediately
                        requestSelectedPdo();
                        transitionTo(AppState::MENU);
                    }
                }
            } else if (_adjust_mode == AdjustMode::CURRENT_LIMIT) {
                applyCurrentLimit();
                transitionTo(AppState::MENU);
            } else if (_adjust_mode == AdjustMode::PPS_VOLTAGE) {
                applyPpsVoltage();
                transitionTo(AppState::MENU);
            }
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
            // Drain any button presses that occurred during BOOT or FAULT
            if (_previous_state == AppState::BOOT || _previous_state == AppState::FAULT) {
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
            // Drain button ISR flags to prevent stale presses after acknowledgment
            Interrupts::checkBtn1Clicked();
            Interrupts::checkBtn2Clicked();
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

    // Encoder button handling:
    // - Use ISR flag for reliable click detection (catches short presses)
    // - Use polling for long-press timing (needs duration measurement)
    
    bool button_pressed = hw.btnEnc.isPressed();

    if (button_pressed && !_encoder_button_held) {
        // Button just pressed - start timing for long press
        _encoder_press_start = get_absolute_time();
        _encoder_button_held = true;
        // Consume any ISR flag that fired for this press
        Interrupts::checkBtnEncClicked();
    } else if (_encoder_button_held) {
        // Button is being held - check for long press threshold
        uint32_t press_duration = absolute_time_diff_us(_encoder_press_start, get_absolute_time()) / 1000;
        
        if (!button_pressed) {
            // Button released
            if (press_duration >= AppConfig::ENCODER_LONG_PRESS_MS) {
                event = EncoderEvent::LONG_PRESS;
            } else if (press_duration > 30) {  // Minimum press time (debounce)
                event = EncoderEvent::CLICK;
            }
            _encoder_button_held = false;
        }
    } else {
        // Button not held - check ISR flag for any clicks we might have missed
        // (e.g., very quick press between main loop iterations)
        if (Interrupts::checkBtnEncClicked()) {
            event = EncoderEvent::CLICK;
        }
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
        case 1:
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

void StateMachine::applyPpsVoltage() {
    LOG_INFO("Requesting PPS: %umV @ %umA", _pps_target_voltage_mv, _pps_max_current_ma);

    // Request PPS contract with the selected voltage
    bool success = pdManager.requestPpsVoltage(_pps_target_voltage_mv, _pps_max_current_ma);

    if (success) {
        LOG_INFO("PPS request sent successfully");
        hw.buzzer.playTone(1000, 50);  // Confirmation beep
    } else {
        LOG_ERROR("Failed to request PPS");
        hw.buzzer.playTone(200, 200);  // Error beep
    }
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

// ============================================================================
// EEPROM Flash Helpers
// ============================================================================

// Static callback for EEPROM progress updates
static void eepromProgressCallback(uint8_t phase, uint8_t progress, void* user_data) {
    StateMachine* sm = static_cast<StateMachine*>(user_data);
    sm->setEepromProgress(phase, progress);
}

void StateMachine::setEepromProgress(uint8_t phase, uint8_t progress) {
    _eeprom_phase = phase;
    _eeprom_progress = progress;
    if (phase == 0) {
        _eeprom_message = "Writing...";
    } else {
        _eeprom_message = "Verifying...";
    }
}

void StateMachine::startEepromCompare() {
    LOG_INFO("Starting EEPROM compare...");

    // Initialize I2C1 for EEPROM access
    if (!eepromInit()) {
        _eeprom_message = "I2C init failed";
        _eeprom_stage = 3;  // Done with failure
        _eeprom_result = false;
        return;
    }

    // Probe for device
    if (!eepromProbe()) {
        _eeprom_message = "EEPROM not found";
        _eeprom_stage = 3;  // Done with failure
        _eeprom_result = false;
        return;
    }

    // Compare EEPROM against firmware
    EepromCompareResult result = eepromCompare();

    switch (result) {
        case EepromCompareResult::IDENTICAL:
            _eeprom_message = "Config identical";
            _eeprom_stage = 3;  // Done - no need to flash
            _eeprom_result = false;  // No flash was performed
            eepromDeinit();  // Release I2C resources
            break;

        case EepromCompareResult::EMPTY:
            _eeprom_message = "EEPROM empty";
            _eeprom_stage = 1;  // Ask for confirmation
            break;

        case EepromCompareResult::DIFFERENT:
            _eeprom_message = "Different config";
            _eeprom_stage = 1;  // Ask for confirmation
            break;

        case EepromCompareResult::NO_DEVICE:
            _eeprom_message = "No EEPROM found";
            _eeprom_stage = 3;  // Done with failure
            _eeprom_result = false;
            eepromDeinit();  // Release I2C resources
            break;

        case EepromCompareResult::READ_ERROR:
        default:
            _eeprom_message = "Read error";
            _eeprom_stage = 3;  // Done with failure
            _eeprom_result = false;
            eepromDeinit();  // Release I2C resources
            break;
    }
}

void StateMachine::executeEepromFlash() {
    LOG_INFO("Starting EEPROM flash...");

    // Disable interrupts that might interfere with the long blocking operation
    // (The I2C EEPROM write is blocking with delays)

    // Execute flash with progress callback
    _eeprom_result = eepromFlash(eepromProgressCallback, this);

    // Move to done stage
    _eeprom_stage = 3;

    if (_eeprom_result) {
        _eeprom_message = "Success! Power cycle";
        // Ta-da! success melody
        hw.buzzer.playTone(880, 80);   // A5
        sleep_ms(80);
        hw.buzzer.playTone(1175, 80);  // D6
        sleep_ms(80);
        hw.buzzer.playTone(1397, 150); // F6
        LOG_INFO("EEPROM flash successful");
    } else {
        _eeprom_message = "Flash failed!";
        hw.buzzer.playTone(200, 300);  // Error beep
        LOG_ERROR("EEPROM flash failed");
    }
}
