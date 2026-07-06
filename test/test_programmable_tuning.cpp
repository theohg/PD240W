// Host unit tests for the pure PPS/AVS auto-tuning corrector in
// src/logic/programmable_tuning.h. This is the exact integer math
// PdManager::serviceKeepAlive and ::checkTuningConvergenceImmediate delegate to
// — the single source that keeps the PPS and AVS loops in lockstep.
//
// Representative constants from the two real ProgrammableContract instances
// (pd_manager.cpp): PPS uses step 20mV, threshold 12mV, max correction 500mV;
// AVS uses step 25mV, threshold 55mV, max correction 500mV.

#include <snitch/snitch.hpp>

#include "logic/programmable_tuning.h"

using namespace ProgrammableTuning;

// ============================================================================
// isConverged
// ============================================================================
TEST_CASE("isConverged: within threshold both directions", "[tuning]") {
    CHECK(isConverged(20000, 20000, 12));        // exact
    CHECK(isConverged(20000, 20012, 12));        // 12mV low, == threshold
    CHECK(isConverged(20000, 19988, 12));        // 12mV high, == threshold
    CHECK_FALSE(isConverged(20000, 20013, 12));  // 13mV low, over
    CHECK_FALSE(isConverged(20000, 19987, 12));  // 13mV high, over
}

TEST_CASE("isConverged: measurement above target still converges", "[tuning]") {
    // Error is signed but the check is on magnitude.
    CHECK(isConverged(15000, 15040, 55));        // 40mV over, within AVS threshold
    CHECK_FALSE(isConverged(15000, 15100, 55));  // 100mV over, outside
}

// ============================================================================
// accumulateCorrection — the P-corrector step
// ============================================================================
TEST_CASE("tuning: converged sample leaves correction untouched", "[tuning]") {
    // measured within threshold -> converged, no nudge, adjusted=false
    CorrectionUpdate u = accumulateCorrection(/*prev*/ 80, 20000, 19995, /*thr*/ 12, /*max*/ 500);
    CHECK(u.correction_mv == 80);
    CHECK(u.converged);
    CHECK_FALSE(u.adjusted);
}

TEST_CASE("tuning: undervoltage accumulates a positive correction", "[tuning]") {
    // Source droops 200mV low -> push the request up by the full error.
    CorrectionUpdate u = accumulateCorrection(0, 20000, 19800, 12, 500);
    CHECK(u.correction_mv == 200);
    CHECK_FALSE(u.converged);
    CHECK(u.adjusted);
}

TEST_CASE("tuning: overvoltage accumulates a negative correction", "[tuning]") {
    CorrectionUpdate u = accumulateCorrection(0, 20000, 20150, 12, 500);
    CHECK(u.correction_mv == -150);
    CHECK_FALSE(u.converged);
    CHECK(u.adjusted);
}

TEST_CASE("tuning: correction accumulates across steps", "[tuning]") {
    // Two consecutive undervoltage samples stack.
    CorrectionUpdate a = accumulateCorrection(0, 20000, 19850, 12, 500);   // +150
    CHECK(a.correction_mv == 150);
    CorrectionUpdate b = accumulateCorrection(a.correction_mv, 20000, 19900, 12, 500);  // +100
    CHECK(b.correction_mv == 250);
}

TEST_CASE("tuning: correction clamps to +max", "[tuning]") {
    // Already near the ceiling; a big error must not exceed +500.
    CorrectionUpdate u = accumulateCorrection(/*prev*/ 450, 20000, 19000, 12, 500);  // +1000 -> clamp
    CHECK(u.correction_mv == 500);
    CHECK_FALSE(u.converged);
}

TEST_CASE("tuning: correction clamps to -max", "[tuning]") {
    CorrectionUpdate u = accumulateCorrection(/*prev*/ -450, 20000, 21000, 12, 500);  // -1000 -> clamp
    CHECK(u.correction_mv == -500);
}

TEST_CASE("tuning: threshold boundary does not nudge", "[tuning]") {
    // |error| == threshold is converged (uses > for the nudge, <= for converged).
    CorrectionUpdate u = accumulateCorrection(30, 20000, 19988, 12, 500);  // error 12 == thr
    CHECK(u.correction_mv == 30);
    CHECK(u.converged);
    CHECK_FALSE(u.adjusted);
}

// ============================================================================
// resolveRequestVoltage — target + correction, clamp to range, align to step
// ============================================================================
TEST_CASE("tuning: request is target plus correction, aligned to PPS step", "[tuning]") {
    // 20000 + 200 = 20200, already 20mV-aligned.
    CHECK(resolveRequestVoltage(20000, 200, 3300, 21000, 20) == 20200);
}

TEST_CASE("tuning: request aligns down to the step", "[tuning]") {
    // 20000 + 15 = 20015 -> align down to 20mV grid -> 20000.
    CHECK(resolveRequestVoltage(20000, 15, 3300, 21000, 20) == 20000);
    // AVS 25mV grid: 15000 + 40 = 15040 -> 15025.
    CHECK(resolveRequestVoltage(15000, 40, 9000, 20000, 25) == 15025);
}

TEST_CASE("tuning: request clamps to APDO max", "[tuning]") {
    // 21000 + 500 = 21500 -> clamp to 21000 (already aligned).
    CHECK(resolveRequestVoltage(21000, 500, 3300, 21000, 20) == 21000);
}

TEST_CASE("tuning: request clamps to APDO min", "[tuning]") {
    // 3300 - 500 = 2800 -> clamp up to 3300.
    CHECK(resolveRequestVoltage(3300, -500, 3300, 21000, 20) == 3300);
}

TEST_CASE("tuning: no clamp when range_max is zero", "[tuning]") {
    // range_max==0 disables clamping; only step alignment applies.
    CHECK(resolveRequestVoltage(20000, 205, 0, 0, 20) == 20200);  // 20205 -> 20200
}

// ============================================================================
// End-to-end: a droopy charger converges over a few keep-alive cycles
// ============================================================================
TEST_CASE("tuning: PPS loop converges a 300mV droop", "[tuning]") {
    const uint32_t target = 20000;
    const int32_t thr = 12, maxc = 500;
    const uint32_t rmin = 3300, rmax = 21000, step = 20;

    int32_t correction = 0;

    // The source consistently delivers 300mV below whatever we request.
    // Model: measured = requested - 300, requested = target + correction.
    auto measured_for = [&](int32_t corr) -> uint32_t {
        uint32_t req = resolveRequestVoltage(target, corr, rmin, rmax, step);
        return req - 300;
    };

    bool converged = false;
    for (int i = 0; i < 10 && !converged; i++) {
        uint32_t measured = measured_for(correction);
        CorrectionUpdate u = accumulateCorrection(correction, target, measured, thr, maxc);
        correction = u.correction_mv;
        converged = u.converged;
    }

    CHECK(converged);
    // Converged correction should roughly cancel the 300mV droop (within a step).
    uint32_t final_req = resolveRequestVoltage(target, correction, rmin, rmax, step);
    CHECK(final_req >= 20280);
    CHECK(final_req <= 20320);
}
