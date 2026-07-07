#include "cc_controller.h"
#include "config/app_config.h"
#include "config/board_config.h"
#include "hardware.h"
#include "logic/cc_regulation.h"
#include "logic/pd_manager.h"
#include "logic/settings.h"
#include "utils/logging.h"

namespace CcController {

// Internal state
static CurrentLimitMode _mode = CurrentLimitMode::OCP;
static uint32_t _target_current_ma = AppConfig::CURRENT_LIMIT_DEFAULT_MA;
static absolute_time_t _next_poll = {0};
static absolute_time_t _next_pd_request = {0};
static bool _regulating = false;

// Estimated load resistance (ohms), updated from measurements
static float _r_estimate = 0.0f;
// Last voltage we requested via PD (tracks CC's own requests)
static int32_t _last_requested_mv = 0;

namespace {

// currentLimitModeName / normalizeCurrentLimitMode now live in settings.h (shared).

void resetRegulationState() {
    _regulating = false;
    _r_estimate = 0.0f;
    _last_requested_mv = 0;
}

uint32_t getAlertLimitMa() {
    if (_mode == CurrentLimitMode::CC) {
        uint32_t ocp_ma = _target_current_ma + AppConfig::CC_SAFETY_MARGIN_MA;
        if (ocp_ma > AppConfig::CURRENT_LIMIT_MAX_MA) {
            ocp_ma = AppConfig::CURRENT_LIMIT_MAX_MA;
        }
        return ocp_ma;
    }

    return _target_current_ma;
}

void applyHardwareAlertLimit() {
    if (_mode == CurrentLimitMode::OFF) {
        hw.powerMonitor.disableOvercurrentLimit();
        return;
    }

    float limit_a = getAlertLimitMa() / 1000.0f;
    if (!hw.powerMonitor.setOvercurrentLimit(limit_a, true)) {
        LOG_ERROR("Failed to set INA228 overcurrent alert");
    }
}

}  // namespace

void init() {
    _mode = settings.getCurrentLimitMode();
    _target_current_ma = settings.getCurrentLimit();
    _next_poll = get_absolute_time();
    _next_pd_request = get_absolute_time();
    resetRegulationState();
    applyHardwareAlertLimit();

    LOG_INFO("Current limit mode restored: %s", currentLimitModeName(_mode));
}

void update() {
    if (_mode != CurrentLimitMode::CC) {
        resetRegulationState();
        return;
    }

    // Only regulate when output is on and PPS or AVS contract is active
    if (!gpio_get(Board::PIN_SWITCH_EN)) {
        resetRegulationState();
        return;
    }

    bool pps = pdManager.isPpsActive();
    bool avs = pdManager.isAvsActive();
    if (!pps && !avs) {
        resetRegulationState();
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

    // Determine user's max voltage (what they selected in the menu)
    int32_t user_max_mv;
    int32_t voltage_min_mv;
    uint32_t step_size;
    if (pps) {
        user_max_mv = (int32_t)pdManager.getPpsUserTargetMv();
        // Use the active APDO's advertised range min; fall back to the spec
        // floor only when the range is unknown (0). Hard-coding the floor would
        // wrongly clamp a contract whose range starts below it.
        uint32_t range_min = pdManager.getPpsRangeMinMv();
        voltage_min_mv = (int32_t)(range_min > 0 ? range_min : AppConfig::PPS_VOLTAGE_FLOOR_MV);
        step_size = AppConfig::PPS_VOLTAGE_STEP_MV;
    } else {
        user_max_mv = (int32_t)pdManager.getAvsUserTargetMv();
        uint32_t range_min = pdManager.getAvsRangeMinMv();
        voltage_min_mv = (int32_t)(range_min > 0 ? range_min : AppConfig::AVS_VOLTAGE_FLOOR_MV);
        step_size = AppConfig::AVS_VOLTAGE_STEP_MV;
    }
    if (user_max_mv <= 0) user_max_mv = (int32_t)pdManager.getActiveContract().voltage_mv;

    // Use our last requested voltage as reference (not measured, which lags)
    int32_t ref_mv = _last_requested_mv > 0 ? _last_requested_mv : (int32_t)pdManager.getActiveContract().voltage_mv;

    // All the regulation arithmetic (R-estimate filter, feedforward + P blend,
    // deadband, overcurrent-never-raises, clamp, align) lives in the pure,
    // host-tested CcRegulation::computeCcStep. Store back its state every poll;
    // only send when it asks and our own request rate-limit allows.
    CcRegulation::CcStep step = CcRegulation::computeCcStep(
        measured_v, measured_a, target_a, _r_estimate, _last_requested_mv,
        ref_mv, user_max_mv, voltage_min_mv, step_size);
    _r_estimate = step.r_estimate;
    _regulating = step.regulating;

    if (!step.request) {
        return;
    }

    // Rate-limit PD voltage requests (charger needs time to slew)
    if (absolute_time_diff_us(_next_pd_request, get_absolute_time()) < 0) {
        return;
    }

    int32_t new_voltage_mv = step.request_mv;

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

void setMode(CurrentLimitMode mode) {
    mode = normalizeCurrentLimitMode(mode);
    if (_mode == mode) {
        return;
    }

    _mode = mode;
    resetRegulationState();
    _next_poll = get_absolute_time();
    _next_pd_request = get_absolute_time();
    applyHardwareAlertLimit();
    settings.setCurrentLimitMode(mode);
    settings.requestSave();

    LOG_INFO("Current limit mode set to %s", currentLimitModeName(mode));
}

CurrentLimitMode getMode() {
    return _mode;
}

bool isDisabled() {
    return _mode == CurrentLimitMode::OFF;
}

void setEnabled(bool enabled) {
    setMode(enabled ? CurrentLimitMode::CC : CurrentLimitMode::OCP);
}

bool isEnabled() {
    return _mode == CurrentLimitMode::CC;
}

void setTargetCurrentMa(uint32_t target_ma) {
    if (target_ma < AppConfig::CURRENT_LIMIT_MIN_MA) {
        target_ma = AppConfig::CURRENT_LIMIT_MIN_MA;
    }
    if (target_ma > AppConfig::CURRENT_LIMIT_MAX_MA) {
        target_ma = AppConfig::CURRENT_LIMIT_MAX_MA;
    }
    _target_current_ma = target_ma;
    resetRegulationState();
    _next_poll = get_absolute_time();
    _next_pd_request = get_absolute_time();
    applyHardwareAlertLimit();
}

uint32_t getTargetCurrentMa() {
    return _target_current_ma;
}

bool isRegulating() {
    return _regulating;
}

}  // namespace CcController
