#pragma once

#include <cstdint>
#include <cstdlib>  // abs
#include "utils/pd_voltage.h"

// ============================================================================
// Programmable-contract (PPS/AVS) auto-tuning math — pure, hardware-free
// ============================================================================
// The closed-loop voltage corrector that PdManager runs on PPS and AVS
// contracts: it compares the user's target against the measured output and nudges
// the requested voltage to compensate for charger droop. This is the twin-coded
// "forgot AVS" bug territory — a single source here, exercised by
// test/test_programmable_tuning.cpp, keeps PPS and AVS honest.
//
// The functions are a faithful, semantics-preserving lift of the integer math
// that used to live inline in PdManager::serviceKeepAlive and
// PdManager::checkTuningConvergenceImmediate. The caller keeps the hardware side
// (measuring the voltage, gating on a valid reading, sending the PD request) and
// delegates the arithmetic here.
//
// Depends only on <cstdint>/<cstdlib> and utils/pd_voltage.h, so it is
// host-buildable without the Pico SDK.
// ============================================================================

namespace ProgrammableTuning {

/// @brief Converged when the target-vs-measured error is within @p threshold_mv.
/// This is the whole rule for the "immediate" convergence check, and the same
/// rule accumulateCorrection() uses to decide whether to keep nudging.
inline bool isConverged(uint32_t user_target_mv, uint32_t measured_mv, int32_t threshold_mv) {
    int32_t error = static_cast<int32_t>(user_target_mv) - static_cast<int32_t>(measured_mv);
    return abs(error) <= threshold_mv;
}

/// @brief Result of one corrector step.
struct CorrectionUpdate {
    int32_t correction_mv;  ///< New accumulated correction (unchanged when converged).
    bool converged;         ///< True when |error| <= threshold this step.
    bool adjusted;          ///< True when the correction changed (caller logs the nudge).
};

/// @brief One step of the accumulating P-corrector. When the error exceeds the
/// convergence threshold, the full error is added to the running correction and
/// clamped to ±@p max_correction_mv; otherwise the loop is declared converged and
/// the correction is left untouched. Assumes the caller has already confirmed a
/// valid measurement.
inline CorrectionUpdate accumulateCorrection(int32_t prev_correction_mv,
                                             uint32_t user_target_mv,
                                             uint32_t measured_mv,
                                             int32_t threshold_mv,
                                             int32_t max_correction_mv) {
    int32_t error = static_cast<int32_t>(user_target_mv) - static_cast<int32_t>(measured_mv);
    CorrectionUpdate u{prev_correction_mv, true, false};

    if (abs(error) > threshold_mv) {
        int32_t corrected = prev_correction_mv + error;
        if (corrected > max_correction_mv) corrected = max_correction_mv;
        if (corrected < -max_correction_mv) corrected = -max_correction_mv;
        u.correction_mv = corrected;
        u.converged = false;
        u.adjusted = true;
    }
    return u;
}

/// @brief Resolve the voltage to actually request: target + correction, clamped
/// into the APDO range (only when @p range_max_mv > 0) and aligned down to the
/// protocol step. Mirrors the tail of serviceKeepAlive's tuning block.
inline uint32_t resolveRequestVoltage(uint32_t user_target_mv,
                                      int32_t correction_mv,
                                      uint32_t range_min_mv,
                                      uint32_t range_max_mv,
                                      uint32_t step_mv) {
    int32_t adjusted = static_cast<int32_t>(user_target_mv) + correction_mv;

    if (range_max_mv > 0) {
        if (adjusted < static_cast<int32_t>(range_min_mv)) adjusted = static_cast<int32_t>(range_min_mv);
        if (adjusted > static_cast<int32_t>(range_max_mv)) adjusted = static_cast<int32_t>(range_max_mv);
    }

    return PdVoltage::alignDown(static_cast<uint32_t>(adjusted), step_mv);
}

}  // namespace ProgrammableTuning
