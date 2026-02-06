#include "state_machine.h"
#include "hardware.h"
#include "interrupts.h"
#include "app_config.h"
#include "utils/logging.h"
#include "utils/tps_eeprom_loader.h"
#include "drivers/buzzer/buzzer.h"
#include "pd_manager.h"
#include "settings.h"
#include "tps_eeprom_workflow.h"
#include "ui/display_manager.h"

// Global instance
StateMachine stateMachine;

// ============================================================================
// Boot Stage Configuration
// ============================================================================

static const char* BOOT_MESSAGES[] = {
    "",                    // 0: Logo only (melody plays)
    "Reading USB-PD...",   // 1: Reading PD contracts
    "Ready!"               // 2: Complete
};
static constexpr uint8_t BOOT_STAGE_COUNT = 3;

// Boot stage timing (cumulative milliseconds)
static const uint32_t BOOT_STAGE_TIMES[] = {
    0,                                            // 0: Logo + start melody
    (uint32_t)(AppConfig::BOOT_DURATION_MS*0.25), // 1: Read USB-PD
    (uint32_t)(AppConfig::BOOT_DURATION_MS*0.75)  // 2: Ready
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
    , _selected_settings_item(SettingsItem::FLASH_EEPROM)
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
    , _brightness_value(100)
    , _brightness_adjusting(false)
    , _screen_dimmed(false)
    , _dim_timeout_value(1)
    , _dim_timeout_adjusting(false)
    , _melody_value(1)
    , _melody_adjusting(false)
{
}

// ============================================================================
// Initialization
// ============================================================================

void StateMachine::init() {
    _state_enter_time = get_absolute_time();
    _last_activity_time = get_absolute_time();
    _last_encoder_ticks = hw.encoder.getTicks();

    // Restore saved current limit from settings
    uint32_t saved_limit = settings.getCurrentLimit();
    if (saved_limit >= AppConfig::CURRENT_LIMIT_MIN_MA && saved_limit <= AppConfig::CURRENT_LIMIT_MAX_MA) {
        _current_limit_ma = saved_limit;
    }
    // Configure INA228 hardware overcurrent alert with the current limit
    float limit_a = _current_limit_ma / 1000.0f;
    if (hw.powerMonitor.setOvercurrentLimit(limit_a, true)) {
        LOG_INFO("INA228 overcurrent alert initialized to %.3fA", limit_a);
    }

    // Play startup melody at boot (only if sounds enabled and melody != Silent)
    if (settings.isSoundsEnabled()) {
        uint8_t melody_idx = settings.getStartupMelody();
        const Note* melody = getStartupMelody(melody_idx);
        uint8_t length = getStartupMelodyLength(melody_idx);
        if (melody && length > 0) {
            hw.buzzer.playMelody(melody, length);
        }
    }

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

    // Auto-dim handling: wake up on any user activity
    if (_screen_dimmed && event != EncoderEvent::NONE) {
        // User interacted - restore brightness
        _screen_dimmed = false;
        hw.display.setBacklightBrightness(_brightness_value);
        hw.rgbLed.setBrightness(AppConfig::RGB_LED_BRIGHTNESS_NORMAL);  // Restore RGB LED brightness
        _last_activity_time = get_absolute_time();
        LOG_INFO("Screen woken from dim (encoder input)");
    }

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

    // Check for menu timeout (return to MAIN after inactivity)
    if (_state == AppState::MENU || _state == AppState::ADJUST) {
        int64_t idle_ms = absolute_time_diff_us(_last_activity_time, get_absolute_time()) / 1000;
        if (idle_ms > (int64_t)AppConfig::MENU_TIMEOUT_MS) {
            LOG_INFO("Menu timeout, returning to MAIN");
            transitionTo(AppState::MAIN);
            needs_refresh = true;
        }
    }

    // Auto-dim check: dim screen after inactivity (applies in all states except BOOT)
    if (_state != AppState::BOOT && !_screen_dimmed) {
        int64_t idle_ms = absolute_time_diff_us(_last_activity_time, get_absolute_time()) / 1000;
        uint32_t dim_timeout_ms = static_cast<uint32_t>(settings.getAutoDimMinutes()) * 60000;
        if (idle_ms > (int64_t)dim_timeout_ms) {
            _screen_dimmed = true;
            hw.display.setBacklightBrightness(AppConfig::LCD_BRIGHTNESS_DIM);
            hw.rgbLed.setBrightness(AppConfig::RGB_LED_BRIGHTNESS_DIM);  // Dim RGB LED too
            LOG_INFO("Screen auto-dimmed after %lld ms inactivity", idle_ms);
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
    // Click enters menu (long press disabled but variable kept)
    if (event == EncoderEvent::CLICK) {
        transitionTo(AppState::MENU);
    }
}

void StateMachine::handleMenuState(EncoderEvent event) {
    // Helper to play navigation beep (respects sound setting)
    auto playNavBeep = [this]() {
        if (settings.isSoundsEnabled()) {
            hw.buzzer.playTone(800, 20);
        }
    };

    switch (event) {
        case EncoderEvent::ROTATE_CW:
            // Move down in menu (with wrap-around)
            {
                int next = static_cast<int>(_selected_menu_item) + 1;
                if (next >= static_cast<int>(MenuItem::MENU_COUNT)) {
                    next = 0;  // Wrap to first item
                }
                _selected_menu_item = static_cast<MenuItem>(next);
                playNavBeep();
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
                playNavBeep();
            }
            _last_activity_time = get_absolute_time();
            break;

        case EncoderEvent::CLICK:
            // Select current menu item (no select beep - navigation sounds removed)
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

                case MenuItem::SETTINGS:
                    _selected_settings_item = SettingsItem::FLASH_EEPROM;
                    _brightness_value = settings.getLcdBrightness();
                    _brightness_adjusting = false;
                    _dim_timeout_value = settings.getAutoDimMinutes();
                    _dim_timeout_adjusting = false;
                    _melody_value = settings.getStartupMelody();
                    _melody_adjusting = false;
                    _adjust_mode = AdjustMode::SETTINGS_MENU;
                    transitionTo(AppState::ADJUST);
                    break;

                case MenuItem::ABOUT:
                    _adjust_mode = AdjustMode::ABOUT;
                    transitionTo(AppState::ADJUST);
                    break;

                case MenuItem::BACK:
                    transitionTo(AppState::MAIN);
                    break;

                default:
                    break;
            }
            _last_activity_time = get_absolute_time();
            break;

        case EncoderEvent::LONG_PRESS:
            // Long press disabled (variable kept for future use)
            break;

        default:
            break;
    }
}

void StateMachine::handleAdjustState(EncoderEvent event) {
    // Helper to play navigation beep (respects sound setting)
    auto playNavBeep = [this]() {
        if (settings.isSoundsEnabled()) {
            hw.buzzer.playTone(800, 20);
        }
    };
    
    auto playSelectBeep = [this]() {
        if (settings.isSoundsEnabled()) {
            hw.buzzer.playTone(1400, 30);
        }
    };
    
    auto playExitBeep = [this]() {
        if (settings.isSoundsEnabled()) {
            hw.buzzer.playTone(1000, 30);
        }
    };

    // About screen: click returns to menu
    if (_adjust_mode == AdjustMode::ABOUT) {
        if (event == EncoderEvent::CLICK) {
            transitionTo(AppState::MENU);
        }
        return;
    }

    // Settings submenu handling
    if (_adjust_mode == AdjustMode::SETTINGS_MENU) {
        handleSettingsMenuState(event);
        return;
    }

    // EEPROM flash mode: delegate to workflow controller
    if (_adjust_mode == AdjustMode::EEPROM_FLASH) {
        bool rotate = (event == EncoderEvent::ROTATE_CW || event == EncoderEvent::ROTATE_CCW);
        bool click = (event == EncoderEvent::CLICK);
        
        if (rotate || click) {
            _last_activity_time = get_absolute_time();
        }
        
        if (tpsEepromWorkflow.handleInput(rotate, click)) {
            // Workflow complete - return to settings menu
            _adjust_mode = AdjustMode::SETTINGS_MENU;
            displayManager.invalidate();
        }
        return;
    }

    switch (event) {
        case EncoderEvent::ROTATE_CW:
            if (_adjust_mode == AdjustMode::PDO_SELECT) {
                if (_selected_pdo_index < _num_pdos) {  // _num_pdos = Back item
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
                    _selected_pdo_index = _num_pdos;  // Wrap to Back item
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
                // If no PDOs or "Back" selected, return to menu
                if (_num_pdos == 0 || _selected_pdo_index == _num_pdos) {
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
            // Long press disabled (variable kept for future use)
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
            hw.rgbLed.setColor(LedColor::GREEN, AppConfig::RGB_LED_BRIGHTNESS_NORMAL);
            // Drain any button presses that occurred during BOOT or FAULT
            if (_previous_state == AppState::BOOT || _previous_state == AppState::FAULT) {
                Interrupts::checkBtn1Clicked();
                Interrupts::checkBtn2Clicked();
            }
            // Auto-output on boot: enable load switch after boot completes
            if (_previous_state == AppState::BOOT && settings.isAutoOutput()) {
                hw.powerMonitor.getDiagnoseAlert();  // Clear INA228 fault latch
                hw.loadSwitch.on();
                LOG_INFO("Auto-output enabled on boot");
            }
            // Restore last PDO on boot (remember last voltage)
            if (_previous_state == AppState::BOOT) {
                int8_t saved_pdo = settings.getLastPdoIndex();
                if (saved_pdo > 0) {  // 0 = default 5V, skip
                    // Load PDO list and attempt to restore
                    loadPdoList();
                    if (saved_pdo < _num_pdos) {
                        SourceCapability& pdo = s_pdo_list[saved_pdo];
                        if (pdo.is_pps && settings.getLastPpsVoltageMv() > 0) {
                            pdManager.requestPpsVoltage(settings.getLastPpsVoltageMv(), pdo.max_current_ma);
                            LOG_INFO("Restored PPS voltage: %umV", settings.getLastPpsVoltageMv());
                        } else if (!pdo.is_pps) {
                            pdManager.requestContract(pdo);
                            LOG_INFO("Restored PDO[%d]: %umV", saved_pdo, pdo.voltage_mv);
                        }
                    }
                }
            }
            break;

        case AppState::MENU:
            _selected_menu_item = MenuItem::SELECT_VOLTAGE;
            hw.rgbLed.setColor(LedColor::BLUE, AppConfig::RGB_LED_BRIGHTNESS_NORMAL);
            break;

        case AppState::ADJUST:
            hw.rgbLed.setColor(LedColor::YELLOW, AppConfig::RGB_LED_BRIGHTNESS_NORMAL);
            break;

        case AppState::FAULT:
            hw.rgbLed.setColor(LedColor::RED);
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
    int delta = _last_encoder_ticks - current_ticks;  // Inverted: physical CW = positive delta
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
            // Consume any ISR flag from release bounce to prevent double-click
            Interrupts::checkBtnEncClicked();
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
    // Check for button presses to wake from dim
    bool btn1_clicked = Interrupts::checkBtn1Clicked();
    bool btn2_clicked = Interrupts::checkBtn2Clicked();

    // Wake from dim on any button press
    if (_screen_dimmed && (btn1_clicked || btn2_clicked)) {
        _screen_dimmed = false;
        hw.display.setBacklightBrightness(_brightness_value);
        hw.rgbLed.setBrightness(AppConfig::RGB_LED_BRIGHTNESS_NORMAL);  // Restore RGB LED brightness
        _last_activity_time = get_absolute_time();
        LOG_INFO("Screen woken from dim (button press)");
    }

    // BTN1: Toggle load switch
    if (btn1_clicked) {
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
    if (btn2_clicked) {
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
        // Save selected PDO for boot restore
        settings.setLastPdoIndex(_selected_pdo_index);
        settings.requestSave();
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

    // Configure INA228 hardware overcurrent alert threshold
    float limit_a = _current_limit_ma / 1000.0f;
    if (hw.powerMonitor.setOvercurrentLimit(limit_a, true)) {
        LOG_INFO("INA228 overcurrent alert set to %.3fA", limit_a);
    } else {
        LOG_ERROR("Failed to set INA228 overcurrent alert");
    }

    // Persist to settings
    settings.setCurrentLimit(_current_limit_ma);
    settings.requestSave();

    if (settings.isSoundsEnabled()) {
        hw.buzzer.playTone(1000, 50);  // Confirmation beep
    }
}

void StateMachine::applyPpsVoltage() {
    LOG_INFO("Requesting PPS: %umV @ %umA", _pps_target_voltage_mv, _pps_max_current_ma);

    // Request PPS contract with the selected voltage
    bool success = pdManager.requestPpsVoltage(_pps_target_voltage_mv, _pps_max_current_ma);

    if (success) {
        LOG_INFO("PPS request sent successfully");
        if (settings.isSoundsEnabled()) {
            hw.buzzer.playTone(1000, 50);  // Confirmation beep
        }
        // Save PPS state for boot restore
        settings.setLastPdoIndex(_pps_pdo_index);
        settings.setLastPpsVoltageMv(_pps_target_voltage_mv);
        settings.requestSave();
    } else {
        LOG_ERROR("Failed to request PPS");
        // Error beep always plays (safety feedback)
        hw.buzzer.playTone(200, 200);
    }
}

// ============================================================================
// Settings Menu Helpers
// ============================================================================

void StateMachine::handleSettingsMenuState(EncoderEvent event) {
    // Helper to play navigation beep (respects sound setting)
    auto playNavBeep = [this]() {
        if (settings.isSoundsEnabled()) {
            hw.buzzer.playTone(800, 20);
        }
    };

    switch (event) {
        case EncoderEvent::ROTATE_CW:
            // Check if any adjustable item is in adjust mode
            if (_selected_settings_item == SettingsItem::BRIGHTNESS && _brightness_adjusting) {
                if (_brightness_value < 100) {
                    _brightness_value += 5;
                    if (_brightness_value > 100) _brightness_value = 100;
                    settings.setLcdBrightness(_brightness_value);
                    settings.requestSave();
                    hw.display.setBacklightBrightness(_brightness_value);
                }
                playNavBeep();
            } else if (_selected_settings_item == SettingsItem::DIM_TIMEOUT && _dim_timeout_adjusting) {
                if (_dim_timeout_value < 10) {
                    _dim_timeout_value++;
                    settings.setAutoDimMinutes(_dim_timeout_value);
                    settings.requestSave();
                }
                playNavBeep();
            } else if (_selected_settings_item == SettingsItem::STARTUP_MELODY && _melody_adjusting) {
                if (_melody_value < 3) {
                    _melody_value++;
                    settings.setStartupMelody(_melody_value);
                    settings.requestSave();
                    // Preview melody on change
                    const Note* melody = getStartupMelody(_melody_value);
                    uint8_t length = getStartupMelodyLength(_melody_value);
                    if (melody && length > 0) {
                        hw.buzzer.playMelody(melody, length);
                    }
                }
            } else {
                // Move down in settings menu (with wrap-around)
                int next = static_cast<int>(_selected_settings_item) + 1;
                if (next >= static_cast<int>(SettingsItem::SETTINGS_COUNT)) {
                    next = 0;
                }
                _selected_settings_item = static_cast<SettingsItem>(next);
                playNavBeep();
            }
            _last_activity_time = get_absolute_time();
            break;

        case EncoderEvent::ROTATE_CCW:
            if (_selected_settings_item == SettingsItem::BRIGHTNESS && _brightness_adjusting) {
                if (_brightness_value > 5) {
                    _brightness_value -= 5;
                } else {
                    _brightness_value = 5;
                }
                settings.setLcdBrightness(_brightness_value);
                settings.requestSave();
                hw.display.setBacklightBrightness(_brightness_value);
                playNavBeep();
            } else if (_selected_settings_item == SettingsItem::DIM_TIMEOUT && _dim_timeout_adjusting) {
                if (_dim_timeout_value > 1) {
                    _dim_timeout_value--;
                    settings.setAutoDimMinutes(_dim_timeout_value);
                    settings.requestSave();
                }
                playNavBeep();
            } else if (_selected_settings_item == SettingsItem::STARTUP_MELODY && _melody_adjusting) {
                if (_melody_value > 0) {
                    _melody_value--;
                    settings.setStartupMelody(_melody_value);
                    settings.requestSave();
                    // Preview melody on change
                    const Note* melody = getStartupMelody(_melody_value);
                    uint8_t length = getStartupMelodyLength(_melody_value);
                    if (melody && length > 0) {
                        hw.buzzer.playMelody(melody, length);
                    } else {
                        hw.buzzer.stopMelody();  // Silent selected
                    }
                }
            } else {
                // Move up in settings menu (with wrap-around)
                int prev = static_cast<int>(_selected_settings_item) - 1;
                if (prev < 0) {
                    prev = static_cast<int>(SettingsItem::SETTINGS_COUNT) - 1;
                }
                _selected_settings_item = static_cast<SettingsItem>(prev);
                playNavBeep();
            }
            _last_activity_time = get_absolute_time();
            break;

        case EncoderEvent::CLICK:
            switch (_selected_settings_item) {
                case SettingsItem::FLASH_EEPROM:
                    _adjust_mode = AdjustMode::EEPROM_FLASH;
                    displayManager.invalidate();
                    tpsEepromWorkflow.start();
                    break;

                case SettingsItem::AUTO_PPS:
                    settings.setAutoPpsEnabled(!settings.isAutoPpsEnabled());
                    settings.requestSave();
                    break;

                case SettingsItem::AUTO_OUTPUT:
                    settings.setAutoOutput(!settings.isAutoOutput());
                    settings.requestSave();
                    break;

                case SettingsItem::BRIGHTNESS:
                    _brightness_adjusting = !_brightness_adjusting;
                    break;

                case SettingsItem::DIM_TIMEOUT:
                    _dim_timeout_adjusting = !_dim_timeout_adjusting;
                    break;

                case SettingsItem::STARTUP_MELODY:
                    _melody_adjusting = !_melody_adjusting;
                    break;

                case SettingsItem::SOUNDS:
                    settings.setSoundsEnabled(!settings.isSoundsEnabled());
                    settings.requestSave();
                    break;

                case SettingsItem::BACK:
                    transitionTo(AppState::MENU);
                    break;

                default:
                    break;
            }
            _last_activity_time = get_absolute_time();
            break;

        case EncoderEvent::LONG_PRESS:
            // Long press disabled (variable kept for future use)
            break;

        default:
            break;
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
    // No valid PD contract - cap at 3A (USB BC1.2 limit)
    return AppConfig::CURRENT_LIMIT_NON_PD_MAX_MA;
}

