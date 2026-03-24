#include "cc_controller.h"
#include "config/app_config.h"
#include "config/board_config.h"
#include "hardware.h"
#include "logic/pd_manager.h"
#include "logic/settings.h"
#include "utils/logging.h"

namespace CcController {

// Internal state
static bool _enabled = false;
static uint32_t _target_current_ma = AppConfig::CURRENT_LIMIT_DEFAULT_MA;
static absolute_time_t _next_poll = {0};
static absolute_time_t _next_pd_request = {0};
static bool _regulating = false;

void init() {
    _enabled = settings.isCcModeEnabled();
    _target_current_ma = settings.getCurrentLimit();
    _next_poll = get_absolute_time();
    _next_pd_request = get_absolute_time();
    _regulating = false;
}

void update() {
    if (!_enabled) {
        _regulating = false;
        return;
    }

    // Only regulate when output is on and PPS or AVS contract is active
    if (!gpio_get(Board::PIN_SWITCH_EN)) {
        _regulating = false;
        return;
    }

    bool pps = pdManager.isPpsActive();
    bool avs = pdManager.isAvsActive();
    if (!pps && !avs) {
        _regulating = false;
        return;
    }

    // Poll at CC_POLL_INTERVAL_MS (10ms / 100Hz)
    if (absolute_time_diff_us(_next_poll, get_absolute_time()) < 0) {
        return;
    }
    _next_poll = make_timeout_time_ms(AppConfig::CC_POLL_INTERVAL_MS);

    _regulating = true;

    // Read actual current from INA228
    float measured_a = hw.powerMonitor.getCurrent();
    float target_a = _target_current_ma / 1000.0f;
    float error_a = measured_a - target_a;

    // Only adjust if error is significant (>50mA deadband to avoid oscillation)
    constexpr float DEADBAND_A = 0.01f;
    if (error_a > DEADBAND_A || error_a < -DEADBAND_A) {
        // Rate-limit PD voltage requests
        if (absolute_time_diff_us(_next_pd_request, get_absolute_time()) < 0) {
            return;
        }

        // P-controller: step voltage proportional to error
        // Overcurrent → reduce voltage, undercurrent → increase voltage
        int32_t step_mv = 0;
        if (error_a > 0) {
            // Too much current → decrease voltage
            step_mv = -(int32_t)AppConfig::CC_VOLTAGE_STEP_MV;
        } else {
            // Too little current → increase voltage
            step_mv = (int32_t)AppConfig::CC_VOLTAGE_STEP_MV;
        }

        // Get current contract voltage and compute new target
        const ActiveContract& contract = pdManager.getActiveContract();
        int32_t new_voltage_mv = (int32_t)contract.voltage_mv + step_mv;

        // Clamp to valid range
        if (pps) {
            uint32_t user_target = pdManager.getPpsUserTargetMv();
            if (new_voltage_mv < 3300) new_voltage_mv = 3300;  // PPS minimum
            if (user_target > 0 && new_voltage_mv > (int32_t)user_target) {
                new_voltage_mv = (int32_t)user_target;
            }
            // Round to PPS 20mV steps
            new_voltage_mv = (new_voltage_mv / AppConfig::PPS_VOLTAGE_STEP_MV) * AppConfig::PPS_VOLTAGE_STEP_MV;

            // Request new PPS voltage (using raw TPS26750 request, not pdManager.requestPpsVoltage
            // which would reset tuning state)
            hw.pdController.requestPPSProfile((uint32_t)new_voltage_mv, contract.current_ma);
        } else if (avs) {
            uint32_t user_target = pdManager.getAvsUserTargetMv();
            if (new_voltage_mv < 15000) new_voltage_mv = 15000;  // AVS minimum
            if (user_target > 0 && new_voltage_mv > (int32_t)user_target) {
                new_voltage_mv = (int32_t)user_target;
            }
            // Round to AVS 25mV steps
            new_voltage_mv = (new_voltage_mv / AppConfig::AVS_VOLTAGE_STEP_MV) * AppConfig::AVS_VOLTAGE_STEP_MV;

            hw.pdController.requestAVSProfile((uint32_t)new_voltage_mv, contract.current_ma);
        }

        _next_pd_request = make_timeout_time_ms(AppConfig::CC_PD_REQUEST_COOLDOWN_MS);
    }
}

void setEnabled(bool enabled) {
    _enabled = enabled;
    if (!enabled) {
        _regulating = false;
    }
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
