#include "safety.h"
#include "hardware.h"
#include "config/app_config.h"
#include "utils/logging.h"
#include "drivers/buzzer/buzzer.h"
#include "logic/state_machine.h"

// Global instance
Safety safety;

// Check intervals (ms)
static constexpr uint32_t TEMP_CHECK_INTERVAL_MS = 500;
static constexpr uint32_t VOLTAGE_CHECK_INTERVAL_MS = 100;

// Temperature hysteresis (avoid rapid toggling)
static constexpr float TEMP_HYSTERESIS_C = 2.0f;

// ============================================================================
// Constructor
// ============================================================================

Safety::Safety()
    : _last_temp_check(nil_time)
    , _last_voltage_check(nil_time)
    , _temp_caution_active(false)
    , _temp_warning_active(false)
    , _temp_fault_active(false)
    , _critical_alarm_active(false)
    , _last_led_status(SafetyStatus::OK)
    , _led_first_run(true)
{
    _state.temperature_c = 25.0f;
    _state.ina_temperature_c = 25.0f;
    _state.max_temperature_c = 25.0f;
    _state.temp_status = SafetyStatus::OK;
    _state.ina_voltage_v = 0.0f;
    _state.vbus_voltage_v = 0.0f;
    _state.pd_connected = false;
    _state.current_a = 0.0f;
    _state.current_overflow = false;
    _state.power_w = 0.0f;
}

// ============================================================================
// Initialization
// ============================================================================

void Safety::init() {
    _last_temp_check = get_absolute_time();
    _last_voltage_check = get_absolute_time();

    // Initial readings
    updateTemperature();
    updateVoltage();
    updateCurrent();

    LOG_INFO("Safety module initialized");
}

// ============================================================================
// Main Update
// ============================================================================

SafetyStatus Safety::update() {
    absolute_time_t now = get_absolute_time();
    SafetyStatus overall_status = SafetyStatus::OK;

    // Temperature check (every 500ms)
    if (absolute_time_diff_us(_last_temp_check, now) >= TEMP_CHECK_INTERVAL_MS * 1000) {
        updateTemperature();
        _last_temp_check = now;
    }

    // Voltage check (every 100ms)
    if (absolute_time_diff_us(_last_voltage_check, now) >= VOLTAGE_CHECK_INTERVAL_MS * 1000) {
        updateVoltage();
        updateCurrent();
        _last_voltage_check = now;
    }

    // Determine overall status.
    // NOTE: Overcurrent is owned entirely by the ISR path
    // (Interrupts::handleOvercurrent() -> StateMachine::setFault(OVERCURRENT)).
    // It is deliberately NOT folded in here: this status is polled every loop and
    // has no clear/acknowledge path, so a latched overcurrent bit would pin the
    // status to FAULT forever (continuous full-rate rendering, red LED stuck).
    if (_temp_fault_active || !_state.pd_connected) {
        overall_status = SafetyStatus::FAULT;
    } else if (_temp_warning_active) {
        overall_status = SafetyStatus::WARNING;
    } else if (_temp_caution_active) { 
        overall_status = SafetyStatus::CAUTION;
    }

    // Critical temperature audible alarm (75-80C range)
    // Melody repeats continuously while in critical range, stops on exit
    bool in_critical_range = _state.max_temperature_c >= static_cast<float>(AppConfig::TEMP_CRITICAL_WARNING_C)
                          && !_temp_fault_active
                          && stateMachine.getState() != AppState::FAULT;

    if (in_critical_range) {
        // Start or restart melody when it finishes playing
        if (!_critical_alarm_active || !hw.buzzer.isPlayingMelody()) {
            hw.buzzer.playMelody(CRITICAL_WARNING_ALARM, CRITICAL_WARNING_ALARM_LENGTH);
            _critical_alarm_active = true;
        }
    } else if (_critical_alarm_active) {
        // Left critical range (below 75C with hysteresis, fault at 80C, or on fault screen)
        if (_temp_fault_active ||
            stateMachine.getState() == AppState::FAULT ||
            _state.max_temperature_c < static_cast<float>(AppConfig::TEMP_CRITICAL_WARNING_C) - TEMP_HYSTERESIS_C) {
            hw.buzzer.stopMelody();
            _critical_alarm_active = false;
        }
    }

    // RGB LED indication
    // Force red while FAULT screen is displayed, even if sensor readings have cleared
    SafetyStatus led_status = overall_status;
    if (stateMachine.getState() == AppState::FAULT) {
        led_status = SafetyStatus::FAULT;
    }

    if (led_status != _last_led_status || _led_first_run) {
        switch (led_status) {
            case SafetyStatus::FAULT:
                hw.rgbLed.setColor(LedColor::RED);
                break;
            case SafetyStatus::WARNING:
                hw.rgbLed.setColor(LedColor::ORANGE);
                break;
            case SafetyStatus::CAUTION:
                hw.rgbLed.setColor(LedColor::YELLOW);
                break;
            case SafetyStatus::OK:
            default:
                break;
        }
        _last_led_status = led_status;
        _led_first_run = false;
    }

    return overall_status;
}

// ============================================================================
// Temperature Monitoring
// ============================================================================

void Safety::updateTemperature() {
    _state.temperature_c = hw.adc.getTemperature();
    _state.ina_temperature_c = hw.powerMonitor.getTemperature();

    _state.max_temperature_c = (_state.temperature_c > _state.ina_temperature_c) ?
                         _state.temperature_c : _state.ina_temperature_c;

    float caution_threshold = static_cast<float>(AppConfig::TEMP_CAUTION_C);   // 50.0
    float warning_threshold = static_cast<float>(AppConfig::TEMP_WARNING_C);   // 65.0
    float shutdown_threshold = static_cast<float>(AppConfig::TEMP_SHUTDOWN_C); // 80.0

    // --------------------------------------------------------
    // 1. FAULT CHECK (Shutdown >= 80C)
    // --------------------------------------------------------
    if (_state.max_temperature_c >= shutdown_threshold) {
        if (!_temp_fault_active) {
            _temp_fault_active = true;
            _state.temp_status = SafetyStatus::FAULT;

            // Disable load switch
            hw.loadSwitch.off();
            LOG_ERROR("OVERTEMPERATURE FAULT: %.1fC >= %.1fC - Load disabled",
                     _state.max_temperature_c, shutdown_threshold);
        }
    } else if (_temp_fault_active && _state.max_temperature_c < (shutdown_threshold - TEMP_HYSTERESIS_C)) {
        _temp_fault_active = false;
        LOG_INFO("Temperature returned to safe level: %.1fC", _state.max_temperature_c);
    }

    // --------------------------------------------------------
    // 2. WARNING CHECK (>= 65C)
    // Only check if not in Fault
    // --------------------------------------------------------
    if (!_temp_fault_active) {
        if (_state.max_temperature_c >= warning_threshold) {
            if (!_temp_warning_active) {
                _temp_warning_active = true;
                _state.temp_status = SafetyStatus::WARNING;
                LOG_WARN("Temperature warning: %.1fC >= %.1fC",
                        _state.max_temperature_c, warning_threshold);
            }
            // Ensure lower severity state is cleared
            _temp_caution_active = false; 
        } else if (_temp_warning_active && _state.max_temperature_c < (warning_threshold - TEMP_HYSTERESIS_C)) {
            _temp_warning_active = false;
            LOG_INFO("Temperature warning cleared: %.1fC", _state.max_temperature_c);
            // Note: We don't set OK here yet; it might fall through to Caution below
        }
    }

    // --------------------------------------------------------
    // 3. CAUTION CHECK (>= 50C)
    // Only check if not in Fault AND not in Warning
    // --------------------------------------------------------
    if (!_temp_fault_active && !_temp_warning_active) {
        if (_state.max_temperature_c >= caution_threshold) {
            if (!_temp_caution_active) {
                _temp_caution_active = true;
                _state.temp_status = SafetyStatus::CAUTION;
                LOG_INFO("Temperature caution: %.1fC >= %.1fC", 
                        _state.max_temperature_c, caution_threshold);
            }
        } else if (_temp_caution_active && _state.max_temperature_c < (caution_threshold - TEMP_HYSTERESIS_C)) {
            _temp_caution_active = false;
            _state.temp_status = SafetyStatus::OK;
            LOG_INFO("Temperature caution cleared: %.1fC", _state.max_temperature_c);
        }
        
        // Ensure status is updated if we are neither Fault, Warning, nor Caution
        if (!_temp_caution_active) {
            _state.temp_status = SafetyStatus::OK;
        }
    }
}

// ============================================================================
// Voltage Monitoring
// ============================================================================

void Safety::updateVoltage() {
    // Read VBUS from ADC (PRE-switch voltage measurement)
    // This is the actual input voltage regardless of switch state
    _state.vbus_voltage_v = hw.adc.getVBUS();

    // Read VBUS from INA228 (post-switch voltage measurement)
    _state.ina_voltage_v = hw.powerMonitor.getBusVoltage();

    // Check PD connection status
    // Consider disconnected if VBUS < 3.3V (below USB minimum)
    bool was_connected = _state.pd_connected;
    _state.pd_connected = (_state.vbus_voltage_v >= 3.3f);

    if (was_connected && !_state.pd_connected) {
        // Just disconnected
        LOG_WARN("USB-PD disconnected (VBUS=%.2fV)", _state.vbus_voltage_v);
        hw.loadSwitch.off();
        hw.en17v.off();
    } else if (!was_connected && _state.pd_connected) {
        // Just connected
        LOG_INFO("USB-PD connected (VBUS=%.2fV)", _state.vbus_voltage_v);
    }

    // Auto-disable 17V buck if VBUS drops below minimum (e.g., new contract < 18V)
    // This handles the case where user enables 17V at 20V, then negotiates a lower voltage
    if (hw.en17v.read()) {
        uint32_t vbus_mv = static_cast<uint32_t>(_state.vbus_voltage_v * 1000.0f);
        if (vbus_mv < AppConfig::MIN_VBUS_FOR_17V_MV) {
            hw.en17v.off();
            LOG_WARN("17V buck auto-disabled: VBUS=%.1fV < 18V", _state.vbus_voltage_v);
            hw.buzzer.playTone(AppConfig::BEEP_ERROR_FREQ, AppConfig::BEEP_WARNING_DURATION);
        }
    }
}

// ============================================================================
// Current Monitoring
// ============================================================================

void Safety::updateCurrent() {
    // Only read current/power when switch is enabled (INA228 is post-switch)
    if (hw.loadSwitch.read()) {
        _state.current_a = hw.powerMonitor.getCurrent();
        _state.current_overflow = hw.powerMonitor.hasMathOverflow();
        _state.power_w = hw.powerMonitor.getPower();

        // Overcurrent detection/latching is handled by the hardware ALERT ISR
        // (interrupts.cpp), which disables the load switch immediately and raises
        // an OVERCURRENT fault through the state machine. We intentionally do NOT
        // latch it here — see the note in update() about the missing clear path.
    } else {
        // Switch is off - current and power are effectively 0
        _state.current_a = 0.0f;
        _state.current_overflow = false;
        _state.power_w = 0.0f;
    }
}

// ============================================================================
// Status Queries
// ============================================================================

bool Safety::isOvertemperature() const {
    return _temp_fault_active;
}

bool Safety::isOvertemperatureWarning() const {
    return _temp_warning_active;
}

bool Safety::isPdConnected() const {
    return _state.pd_connected;
}
