#include "safety.h"
#include "hardware.h"
#include "app_config.h"
#include "utils/logging.h"

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
    : _current_limit_a(5.0f)
    , _last_temp_check(nil_time)
    , _last_voltage_check(nil_time)
    , _temp_warning_active(false)
    , _temp_fault_active(false)
{
    _state.temperature_c = 25.0f;
    _state.ina_temperature_c = 25.0f;
    _state.temp_status = SafetyStatus::OK;
    _state.vbus_voltage_v = 0.0f;
    _state.pd_connected = false;
    _state.current_a = 0.0f;
    _state.overcurrent_latched = false;
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

    // Determine overall status
    if (_state.overcurrent_latched || _temp_fault_active || !_state.pd_connected) {
        overall_status = SafetyStatus::FAULT;
    } else if (_temp_warning_active) {
        overall_status = SafetyStatus::WARNING;
    }

    return overall_status;
}

// ============================================================================
// Temperature Monitoring
// ============================================================================

void Safety::updateTemperature() {
    _state.temperature_c = hw.adc.getTemperature();
    _state.ina_temperature_c = hw.powerMonitor.getTemperature();

    float warning_threshold = static_cast<float>(AppConfig::TEMP_WARNING_C);
    float shutdown_threshold = static_cast<float>(AppConfig::TEMP_SHUTDOWN_C);

    // Check for fault (with hysteresis on recovery)
    if (_state.temperature_c >= shutdown_threshold) {
        if (!_temp_fault_active) {
            _temp_fault_active = true;
            _state.temp_status = SafetyStatus::FAULT;

            // Disable load switch
            hw.loadSwitch.off();
            LOG_ERROR("OVERTEMPERATURE FAULT: %.1fC >= %.1fC - Load disabled",
                     _state.temperature_c, shutdown_threshold);
        }
    } else if (_temp_fault_active && _state.temperature_c < (shutdown_threshold - TEMP_HYSTERESIS_C)) {
        _temp_fault_active = false;
        LOG_INFO("Temperature returned to safe level: %.1fC", _state.temperature_c);
    }

    // Check for warning (with hysteresis)
    if (!_temp_fault_active) {
        if (_state.temperature_c >= warning_threshold) {
            if (!_temp_warning_active) {
                _temp_warning_active = true;
                _state.temp_status = SafetyStatus::WARNING;
                LOG_WARN("Temperature warning: %.1fC >= %.1fC",
                        _state.temperature_c, warning_threshold);
            }
        } else if (_temp_warning_active && _state.temperature_c < (warning_threshold - TEMP_HYSTERESIS_C)) {
            _temp_warning_active = false;
            _state.temp_status = SafetyStatus::OK;
            LOG_INFO("Temperature warning cleared: %.1fC", _state.temperature_c);
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
    // Consider disconnected if VBUS < 4V (below USB minimum)
    bool was_connected = _state.pd_connected;
    _state.pd_connected = (_state.vbus_voltage_v >= 4.0f);

    if (was_connected && !_state.pd_connected) {
        // Just disconnected
        LOG_WARN("USB-PD disconnected (VBUS=%.2fV)", _state.vbus_voltage_v);
        hw.loadSwitch.off();
        hw.EN_17V.off();
    } else if (!was_connected && _state.pd_connected) {
        // Just connected
        LOG_INFO("USB-PD connected (VBUS=%.2fV)", _state.vbus_voltage_v);
    }
}

// ============================================================================
// Current Monitoring
// ============================================================================

void Safety::updateCurrent() {
    // Only read current/power when switch is enabled (INA228 is post-switch)
    if (hw.loadSwitch.read()) {
        _state.current_a = hw.powerMonitor.getCurrent();
        _state.power_w = hw.powerMonitor.getPower();

        // Check if INA228 ALERT is latched (overcurrent already triggered by ISR)
        // The ALERT pin is active-low and latched
        // IMPORTANT: Only check when switch is enabled - pin goes low when switch is off
        if (!hw.overcurrentAlert.read()) {
            _state.overcurrent_latched = true;
        }
    } else {
        // Switch is off - current and power are effectively 0
        _state.current_a = 0.0f;
        _state.power_w = 0.0f;
        // Don't update overcurrent_latched when switch is off
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

bool Safety::isOvercurrentLatched() const {
    return _state.overcurrent_latched;
}

// ============================================================================
// Control
// ============================================================================

void Safety::clearOvercurrentLatch() {
    // Clear INA228 alert latch by reading DIAG_ALRT register
    hw.powerMonitor.getDiagnoseAlert();
    _state.overcurrent_latched = false;
    LOG_INFO("Overcurrent latch cleared");
}

void Safety::setCurrentLimit(float limit_a) {
    _current_limit_a = limit_a;

    // TODO: Configure INA228 alert threshold
    // For now, just store the value
    LOG_DEBUG("Current limit set to %.2fA", limit_a);
}
