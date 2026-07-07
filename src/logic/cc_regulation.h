#pragma once

#include <cmath>    // fabsf
#include <cstdint>
#include "utils/pd_voltage.h"

// ============================================================================
// Constant-current regulation math — pure, hardware-free
// ============================================================================
// The control math CcController::update() runs every 10ms to hold the user's
// current target by nudging the requested PPS/AVS voltage. This is the largest
// block of closed-loop control in the firmware and it drives real voltage
// requests at a live load, so it is lifted here verbatim to be host-testable
// (test/test_cc_regulation.cpp) — the caller keeps the hardware I/O (reading the
// INA228, gating on output/contract state, timing, sending the PD request).
//
// The arithmetic is a faithful, semantics-preserving lift of the body of
// CcController::update() (the R-estimate filter, Ohm's-law feedforward + 50/50
// blend, proportional correction, deadband, the "never raise voltage while
// overcurrent" safety rule, range clamps, and step alignment).
//
// Depends only on <cmath>/<cstdint> and utils/pd_voltage.h, so it is
// host-buildable without the Pico SDK.
// ============================================================================

namespace CcRegulation {

// Tuning constants — kept here (not in AppConfig) because they are intrinsic to
// this control law and are exercised as a unit by the host tests.
constexpr float MIN_CURRENT_FOR_R_EST = 0.05f;  ///< Below this, don't refresh R (V/I too noisy).
constexpr float NO_LOAD_CURRENT_A     = 0.01f;  ///< Below this, treat as open circuit.
constexpr float MIN_VOLTAGE_FOR_R_EST = 0.5f;   ///< Below this bus voltage, R estimate is meaningless.
constexpr float R_ESTIMATE_ALPHA      = 0.2f;   ///< Low-pass weight for a fresh R sample.
constexpr float MIN_VALID_R_OHMS      = 0.1f;   ///< Below this R, fall back to proportional-only.
constexpr float DEADBAND_A            = 0.02f;  ///< No adjustment when |error| within this band.
constexpr float KP_MV_PER_A           = 200.0f; ///< Proportional gain on top of the feedforward.
constexpr float KP_FALLBACK_MV_PER_A  = 500.0f; ///< Proportional gain with no valid R estimate.

/// @brief Result of one regulation step.
struct CcStep {
    float r_estimate;    ///< Updated load-resistance estimate; store back unconditionally.
    bool regulating;     ///< True when CC has actively pulled the request below the user max.
    bool request;        ///< True when a PD voltage request should be issued this step.
    int32_t request_mv;  ///< The voltage to request when @ref request is true.
};

/// @brief One step of the CC voltage regulator.
///
/// Mirrors CcController::update() from the R-estimate update through the
/// skip-if-unchanged decision. The caller resolves @p ref_mv (last requested
/// voltage, or the active contract voltage on the first step) and @p user_max_mv
/// (the menu-selected ceiling) from hardware/PD state, then stores back
/// @ref CcStep::r_estimate and @ref CcStep::regulating, and — only when
/// @ref CcStep::request is set and its own request rate-limit allows — sends
/// @ref CcStep::request_mv.
///
/// @param measured_v        Bus voltage from the INA228 (volts).
/// @param measured_a        Load current from the INA228 (amps).
/// @param target_a          User current target (amps).
/// @param prev_r_estimate   Previous load-resistance estimate (ohms).
/// @param last_requested_mv Voltage CC last requested (0 before the first request).
/// @param ref_mv            Reference voltage: last requested, else active contract.
/// @param user_max_mv       Ceiling: the user-selected PPS/AVS voltage.
/// @param voltage_min_mv    Floor: the active APDO range min (or spec floor).
/// @param step_mv           Protocol step (20mV PPS / 25mV AVS) for alignment.
inline CcStep computeCcStep(float measured_v, float measured_a, float target_a,
                            float prev_r_estimate, int32_t last_requested_mv,
                            int32_t ref_mv, int32_t user_max_mv,
                            int32_t voltage_min_mv, uint32_t step_mv) {
    CcStep s{prev_r_estimate, false, false, last_requested_mv};

    float error_a = measured_a - target_a;  // positive = overcurrent

    // CC is "regulating" only when it has actively lowered voltage below user target.
    s.regulating = (last_requested_mv > 0 && last_requested_mv < user_max_mv);

    // Update resistance estimate from current measurement (R = V/I).
    if (measured_a > MIN_CURRENT_FOR_R_EST && measured_v > MIN_VOLTAGE_FOR_R_EST) {
        float r_new = measured_v / measured_a;
        if (s.r_estimate <= 0.0f) {
            s.r_estimate = r_new;
        } else {
            // Low-pass filter for stability.
            s.r_estimate = s.r_estimate * (1.0f - R_ESTIMATE_ALPHA) + r_new * R_ESTIMATE_ALPHA;
        }
    } else if (measured_a < NO_LOAD_CURRENT_A) {
        // No load: invalidate R estimate so we ramp back to user voltage.
        s.r_estimate = 0.0f;
    }

    // Deadband: don't adjust if error is small enough AND already at user target
    // voltage. When undercurrent and below user max, keep trying to reach max.
    if (fabsf(error_a) <= DEADBAND_A && ref_mv >= user_max_mv) {
        return s;  // request stays false
    }

    // Calculate target voltage.
    int32_t new_voltage_mv;
    if (measured_a < NO_LOAD_CURRENT_A) {
        // No load detected: jump directly to user target voltage.
        new_voltage_mv = user_max_mv;
    } else if (s.r_estimate > MIN_VALID_R_OHMS) {
        // Ohm's law feedforward: V_ideal = I_target * R_estimated.
        int32_t v_ideal_mv = static_cast<int32_t>(target_a * s.r_estimate * 1000.0f);
        // Blend feedforward with current reference for stability (50/50).
        new_voltage_mv = (v_ideal_mv + ref_mv) / 2;
        // Small proportional correction on top.
        new_voltage_mv += static_cast<int32_t>(-error_a * KP_MV_PER_A);
    } else {
        // No valid R estimate: conservative proportional steps from reference.
        new_voltage_mv = ref_mv + static_cast<int32_t>(-error_a * KP_FALLBACK_MV_PER_A);
    }

    // Safety: if overcurrent, never increase voltage.
    if (error_a > 0 && new_voltage_mv > ref_mv) {
        new_voltage_mv = ref_mv - static_cast<int32_t>(step_mv);
    }

    // Clamp to valid range.
    if (new_voltage_mv < voltage_min_mv) new_voltage_mv = voltage_min_mv;
    if (new_voltage_mv > user_max_mv) new_voltage_mv = user_max_mv;

    new_voltage_mv = static_cast<int32_t>(PdVoltage::alignDown(
        static_cast<uint32_t>(new_voltage_mv), step_mv));

    s.request_mv = new_voltage_mv;

    // Skip if voltage hasn't changed from last request.
    if (new_voltage_mv == last_requested_mv) {
        return s;  // request stays false
    }

    s.request = true;
    return s;
}

}  // namespace CcRegulation
