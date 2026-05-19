#include "cc_controller.h"
#include "config/app_config.h"
#include "config/board_config.h"
#include "hardware.h"
#include "logic/pd_manager.h"
#include "logic/settings.h"
#include "utils/logging.h"

#include <cmath>

namespace CcController {

// Internal state
static bool _enabled = false;
static uint32_t _target_current_ma = AppConfig::CURRENT_LIMIT_DEFAULT_MA;
static absolute_time_t _next_poll = {0};
static absolute_time_t _next_pd_request = {0};
static bool _regulating = false;

// Estimated load resistance (ohms), updated from measurements
static float _r_estimate = 0.0f;
// Last voltage we requested via PD (tracks CC's own requests)
static int32_t _last_requested_mv = 0;

void init() {
    _enabled = settings.isCcModeEnabled();
    _target_current_ma = settings.getCurrentLimit();
    _next_poll = get_absolute_time();
    _next_pd_request = get_absolute_time();
    _regulating = false;
    _r_estimate = 0.0f;
    _last_requested_mv = 0;

    // If CC was enabled from settings, configure OCP threshold with safety margin
    if (_enabled) {
        uint32_t ocp_ma = _target_current_ma + AppConfig::CC_SAFETY_MARGIN_MA;
        if (ocp_ma > AppConfig::CURRENT_LIMIT_MAX_MA) {
            ocp_ma = AppConfig::CURRENT_LIMIT_MAX_MA;
        }
        hw.powerMonitor.setOvercurrentLimit(ocp_ma / 1000.0f, true);
        LOG_INFO("CC mode restored from settings, OCP=%.2fA", ocp_ma / 1000.0f);
    }
}

void update() {
    if (!_enabled) {
        _regulating = false;
        _r_estimate = 0.0f;
        _last_requested_mv = 0;
        return;
    }

    // Only regulate when output is on and PPS or AVS contract is active
    if (!gpio_get(Board::PIN_SWITCH_EN)) {
        _regulating = false;
        _r_estimate = 0.0f;
        _last_requested_mv = 0;
        return;
    }

    bool pps = pdManager.isPpsActive();
    bool avs = pdManager.isAvsActive();
    if (!pps && !avs) {
        _regulating = false;
        _r_estimate = 0.0f;
        _last_requested_mv = 0;
        return;
    }

    // Poll at CC_POLL_INTERVAL_MS (10ms / 100Hz)
    if (absolute_time_diff_us(_next_poll, get_absolute_time()) < 0) {
        return;
    }
    _next_poll = make_timeout_time_ms(AppConfig::CC_POLL_INTERVAL_MS);

    // Read actual current and voltage from INA228
    float measured_a = hw.powerMonitor.getCurrent();
    float measured_v = hw.powerMonitor.getBusVoltage();
    float target_a = _target_current_ma / 1000.0f;
    float error_a = measured_a - target_a;  // positive = overcurrent

    // Determine user's max voltage (what they selected in the menu)
    int32_t user_max_mv;
    int32_t voltage_min_mv;
    uint32_t step_size;
    if (pps) {
        user_max_mv = (int32_t)pdManager.getPpsUserTargetMv();
        voltage_min_mv = 3300;
        step_size = AppConfig::PPS_VOLTAGE_STEP_MV;
    } else {
        user_max_mv = (int32_t)pdManager.getAvsUserTargetMv();
        voltage_min_mv = 15000;
        step_size = AppConfig::AVS_VOLTAGE_STEP_MV;
    }
    if (user_max_mv <= 0) user_max_mv = (int32_t)pdManager.getActiveContract().voltage_mv;

    // Use our last requested voltage as reference (not measured, which lags)
    int32_t ref_mv = _last_requested_mv > 0 ? _last_requested_mv : (int32_t)pdManager.getActiveContract().voltage_mv;

    // CC is "regulating" only when it has actively lowered voltage below user target
    _regulating = (_last_requested_mv > 0 && _last_requested_mv < user_max_mv);

    // Update resistance estimate from current measurement (R = V/I)
    constexpr float MIN_CURRENT_FOR_R_EST = 0.05f;
    if (measured_a > MIN_CURRENT_FOR_R_EST && measured_v > 0.5f) {
        float r_new = measured_v / measured_a;
        if (_r_estimate <= 0.0f) {
            _r_estimate = r_new;
        } else {
            // Low-pass filter (alpha=0.2 for stability)
            _r_estimate = _r_estimate * 0.8f + r_new * 0.2f;
        }
    } else if (measured_a < 0.01f) {
        // No load: invalidate R estimate so we ramp back to user voltage
        _r_estimate = 0.0f;
    }

    // Deadband: don't adjust if error is small enough AND already at user target voltage
    // When undercurrent and below user max, keep trying to reach max voltage
    constexpr float DEADBAND_A = 0.02f;
    if (fabsf(error_a) <= DEADBAND_A && ref_mv >= user_max_mv) {
        return;
    }

    // Rate-limit PD voltage requests (charger needs time to slew)
    if (absolute_time_diff_us(_next_pd_request, get_absolute_time()) < 0) {
        return;
    }

    // Calculate target voltage
    int32_t new_voltage_mv;

    if (measured_a < 0.01f) {
        // No load detected: jump directly to user target voltage
        new_voltage_mv = user_max_mv;
    } else if (_r_estimate > 0.1f) {
        // Ohm's law feedforward: V_ideal = I_target * R_estimated
        int32_t v_ideal_mv = (int32_t)(target_a * _r_estimate * 1000.0f);

        // Blend feedforward with current reference for stability (50/50)
        new_voltage_mv = (v_ideal_mv + ref_mv) / 2;

        // Small proportional correction on top
        constexpr float KP_MV_PER_A = 200.0f;
        new_voltage_mv += (int32_t)(-error_a * KP_MV_PER_A);
    } else {
        // No valid R estimate: conservative proportional steps from reference
        constexpr float KP_FALLBACK_MV_PER_A = 500.0f;
        new_voltage_mv = ref_mv + (int32_t)(-error_a * KP_FALLBACK_MV_PER_A);
    }

    // Safety: if overcurrent, never increase voltage
    if (error_a > 0 && new_voltage_mv > ref_mv) {
        new_voltage_mv = ref_mv - (int32_t)step_size;
    }

    // Clamp to valid range
    if (new_voltage_mv < voltage_min_mv) new_voltage_mv = voltage_min_mv;
    if (new_voltage_mv > user_max_mv) new_voltage_mv = user_max_mv;

    // Round to PD spec step boundary
    new_voltage_mv = (new_voltage_mv / (int32_t)step_size) * (int32_t)step_size;

    // Skip if voltage hasn't changed from last request
    if (new_voltage_mv == _last_requested_mv) {
        return;
    }

    // Send PD voltage request, passing the stored APDO bounds so the driver
    // constrains its fallback window to the correct range.
    bool success = false;
    if (pps) {
        success = hw.pdController.requestPPSProfile((uint32_t)new_voltage_mv,
                      pdManager.getActiveContract().current_ma,
                      pdManager.getPpsRangeMinMv(), pdManager.getPpsRangeMaxMv());
    } else {
        success = hw.pdController.requestAVSProfile((uint32_t)new_voltage_mv,
                      pdManager.getActiveContract().current_ma,
                      pdManager.getAvsRangeMinMv(), pdManager.getAvsRangeMaxMv());
    }

    if (success) {
        _last_requested_mv = new_voltage_mv;
        pdManager.setCcKeepAliveVoltage((uint32_t)new_voltage_mv);
    }

    _next_pd_request = make_timeout_time_ms(AppConfig::CC_PD_REQUEST_COOLDOWN_MS);
}

void setEnabled(bool enabled) {
    _enabled = enabled;
    if (!enabled) {
        _regulating = false;
    }
    _r_estimate = 0.0f;
    _last_requested_mv = 0;
    settings.setCcModeEnabled(enabled);
    settings.requestSave();

    // Update OCP threshold based on mode
    if (enabled) {
        // CC mode: set OCP margin above target
        uint32_t ocp_ma = _target_current_ma + AppConfig::CC_SAFETY_MARGIN_MA;
        if (ocp_ma > AppConfig::CURRENT_LIMIT_MAX_MA) {
            ocp_ma = AppConfig::CURRENT_LIMIT_MAX_MA;
        }
        hw.powerMonitor.setOvercurrentLimit(ocp_ma / 1000.0f, true);
    } else {
        // OCP mode: set to exact current limit
        hw.powerMonitor.setOvercurrentLimit(_target_current_ma / 1000.0f, true);
    }

    LOG_INFO("CC mode %s, OCP=%.2fA", enabled ? "enabled" : "disabled",
             hw.powerMonitor.getOvercurrentLimit());
}

bool isEnabled() {
    return _enabled;
}

void setTargetCurrentMa(uint32_t target_ma) {
    if (target_ma < AppConfig::CURRENT_LIMIT_MIN_MA) {
        target_ma = AppConfig::CURRENT_LIMIT_MIN_MA;
    }
    if (target_ma > AppConfig::CURRENT_LIMIT_MAX_MA) {
        target_ma = AppConfig::CURRENT_LIMIT_MAX_MA;
    }
    _target_current_ma = target_ma;

    // Update OCP threshold
    if (_enabled) {
        uint32_t ocp_ma = target_ma + AppConfig::CC_SAFETY_MARGIN_MA;
        if (ocp_ma > AppConfig::CURRENT_LIMIT_MAX_MA) {
            ocp_ma = AppConfig::CURRENT_LIMIT_MAX_MA;
        }
        hw.powerMonitor.setOvercurrentLimit(ocp_ma / 1000.0f, true);
    }
}

uint32_t getTargetCurrentMa() {
    return _target_current_ma;
}

bool isRegulating() {
    return _regulating;
}

}  // namespace CcController
