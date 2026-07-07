// Host unit tests for the pure constant-current regulator in
// src/logic/cc_regulation.h — the exact control math CcController::update()
// delegates to. This is the largest block of closed-loop control in the
// firmware; a regression here drives real voltage requests at a live load with
// only the INA228 OCP as backstop, so the invariants below (never-raise-on-
// overcurrent, deadband, clamps, R-estimate seeding/decay) are pinned tightly.
//
// Representative bounds from the real contracts: PPS step 20mV, floor ~3300mV,
// ceiling up to 21000mV; AVS step 25mV, floor ~15000mV, ceiling up to 48000mV.

#include <cmath>

#include <snitch/snitch.hpp>

#include "logic/cc_regulation.h"

using namespace CcRegulation;

// Snitch ships no float matcher; a small absolute-tolerance helper covers the
// low-pass-filter checks where exact float equality is brittle across platforms.
static bool approx_eq(float a, float b, float tol = 1e-3f) {
    return std::fabs(a - b) <= tol;
}

// Common PPS bounds used across cases unless a test overrides them.
static constexpr int32_t PPS_MIN = 3300;
static constexpr uint32_t PPS_STEP = 20;

// ============================================================================
// R-estimate seeding, decay, and invalidation
// ============================================================================
TEST_CASE("cc: R-estimate seeds directly on first valid sample", "[cc]") {
    // 20V @ 2A -> 10 ohm. prev estimate 0 -> seed straight to the fresh value.
    CcStep s = computeCcStep(/*v*/ 20.0f, /*a*/ 2.0f, /*target*/ 2.0f,
                             /*prev_r*/ 0.0f, /*last_req*/ 20000,
                             /*ref*/ 20000, /*user_max*/ 20000, PPS_MIN, PPS_STEP);
    CHECK(s.r_estimate == 10.0f);
}

TEST_CASE("cc: R-estimate low-pass blends new sample at alpha", "[cc]") {
    // prev 10 ohm, fresh sample 20V/1A = 20 ohm -> 10*0.8 + 20*0.2 = 12.
    CcStep s = computeCcStep(20.0f, 1.0f, 1.0f, /*prev_r*/ 10.0f, 20000,
                             20000, 20000, PPS_MIN, PPS_STEP);
    CHECK(approx_eq(s.r_estimate, 12.0f));
}

TEST_CASE("cc: no-load current invalidates the R-estimate", "[cc]") {
    // Below NO_LOAD_CURRENT_A (0.01A) -> R estimate zeroed so we ramp back up.
    CcStep s = computeCcStep(20.0f, 0.005f, 2.0f, /*prev_r*/ 10.0f, 15000,
                             15000, 20000, PPS_MIN, PPS_STEP);
    CHECK(s.r_estimate == 0.0f);
}

TEST_CASE("cc: current in the dead zone leaves R-estimate untouched", "[cc]") {
    // Between NO_LOAD (0.01) and MIN_CURRENT_FOR_R_EST (0.05): don't refresh,
    // don't invalidate — hold the previous estimate.
    CcStep s = computeCcStep(20.0f, 0.03f, 2.0f, /*prev_r*/ 8.0f, 15000,
                             15000, 20000, PPS_MIN, PPS_STEP);
    CHECK(s.r_estimate == 8.0f);
}

TEST_CASE("cc: sub-threshold bus voltage does not refresh R-estimate", "[cc]") {
    // Current is high enough but bus voltage < MIN_VOLTAGE_FOR_R_EST (0.5V):
    // V/I is meaningless, keep the previous estimate.
    CcStep s = computeCcStep(/*v*/ 0.3f, /*a*/ 2.0f, 2.0f, /*prev_r*/ 9.0f, 15000,
                             15000, 20000, PPS_MIN, PPS_STEP);
    CHECK(s.r_estimate == 9.0f);
}

// ============================================================================
// The safety invariant: overcurrent NEVER raises the requested voltage
// ============================================================================
TEST_CASE("cc: overcurrent never raises voltage", "[cc][safety]") {
    // Measured current well over target: the feedforward already resolves a much
    // lower voltage (clamped to the floor), so the request drops below ref.
    CcStep s = computeCcStep(/*v*/ 5.0f, /*a*/ 4.0f, /*target*/ 1.0f,
                             /*prev_r*/ 0.0f, /*last_req*/ 5000,
                             /*ref*/ 5000, /*user_max*/ 20000, PPS_MIN, PPS_STEP);
    REQUIRE(s.request);
    CHECK(s.request_mv < 5000);       // never above ref while overcurrent
    CHECK(s.request_mv >= PPS_MIN);   // but not below the floor
}

TEST_CASE("cc: overcurrent step-down branch drops one step below ref", "[cc][safety]") {
    // Force the case where the computed voltage would exceed ref: a large R makes
    // the feedforward want to climb, but error_a > 0 must pull it to ref - step.
    // v=8V, a=3A (overcurrent vs target 1A), prev_r=50 -> v_ideal huge.
    CcStep s = computeCcStep(/*v*/ 8.0f, /*a*/ 3.0f, /*target*/ 1.0f,
                             /*prev_r*/ 50.0f, /*last_req*/ 8000,
                             /*ref*/ 8000, /*user_max*/ 20000, PPS_MIN, PPS_STEP);
    REQUIRE(s.request);
    CHECK(s.request_mv == 8000 - (int32_t)PPS_STEP);  // exactly ref - one step
}

TEST_CASE("cc: overcurrent with a high R-estimate still cannot raise", "[cc][safety]") {
    // Feedforward from a large R would push voltage way up, but error_a > 0
    // forces it back below ref. Guards the invariant against the feedforward path.
    CcStep s = computeCcStep(10.0f, 3.0f, /*target*/ 1.0f, /*prev_r*/ 50.0f,
                             /*last_req*/ 10000, /*ref*/ 10000, /*user_max*/ 20000,
                             PPS_MIN, PPS_STEP);
    REQUIRE(s.request);
    CHECK(s.request_mv <= 10000 - (int32_t)PPS_STEP);
}

// ============================================================================
// Deadband
// ============================================================================
TEST_CASE("cc: within deadband and at ceiling issues no request", "[cc]") {
    // |error| <= DEADBAND_A (0.02) AND ref >= user_max -> no adjustment.
    CcStep s = computeCcStep(20.0f, 2.01f, /*target*/ 2.0f, /*prev_r*/ 10.0f,
                             /*last_req*/ 20000, /*ref*/ 20000, /*user_max*/ 20000,
                             PPS_MIN, PPS_STEP);
    CHECK_FALSE(s.request);
}

TEST_CASE("cc: undercurrent below the ceiling raises the request", "[cc]") {
    // Below ceiling with the load drawing less than target: the deadband gate
    // (which only silences us at the top) does not apply, and the feedforward
    // asks for more voltage. v=15V @ 1.5A -> R=10; target 2.0A -> ideal ~20V.
    CcStep s = computeCcStep(15.0f, /*a*/ 1.5f, /*target*/ 2.0f, /*prev_r*/ 10.0f,
                             /*last_req*/ 15000, /*ref*/ 15000, /*user_max*/ 20000,
                             PPS_MIN, PPS_STEP);
    REQUIRE(s.request);
    CHECK(s.request_mv > 15000);  // climbs toward the ceiling
}

TEST_CASE("cc: deadband boundary is inclusive", "[cc]") {
    // error exactly == DEADBAND_A (0.02) at the ceiling -> still converged.
    CcStep s = computeCcStep(20.0f, 2.02f, 2.0f, 10.0f, 20000, 20000, 20000,
                             PPS_MIN, PPS_STEP);
    CHECK_FALSE(s.request);
    // Just past the boundary -> a request appears.
    CcStep s2 = computeCcStep(20.0f, 2.021f, 2.0f, 10.0f, 20000, 20000, 20000,
                              PPS_MIN, PPS_STEP);
    CHECK(s2.request);
}

// ============================================================================
// Clamps and step alignment
// ============================================================================
TEST_CASE("cc: request clamps to user max ceiling", "[cc]") {
    // No load -> jump to user target; must not exceed user_max.
    CcStep s = computeCcStep(0.0f, 0.0f, 2.0f, 0.0f, 15000, 15000,
                             /*user_max*/ 18000, PPS_MIN, PPS_STEP);
    REQUIRE(s.request);
    CHECK(s.request_mv == 18000);
}

TEST_CASE("cc: request clamps to the voltage floor", "[cc]") {
    // Massive overcurrent with a low ref drives below the floor -> clamp up.
    CcStep s = computeCcStep(3.4f, 5.0f, /*target*/ 0.1f, 0.0f,
                             /*last_req*/ 3320, /*ref*/ 3320, /*user_max*/ 20000,
                             /*floor*/ PPS_MIN, PPS_STEP);
    REQUIRE(s.request);
    CHECK(s.request_mv >= PPS_MIN);
}

TEST_CASE("cc: request is aligned down to the protocol step", "[cc]") {
    // AVS 25mV grid: exercise alignment on the no-load jump.
    const int32_t AVS_MIN = 15000;
    const uint32_t AVS_STEP = 25;
    CcStep s = computeCcStep(0.0f, 0.0f, 1.0f, 0.0f, /*last_req*/ 19000,
                             /*ref*/ 19000, /*user_max*/ 20013, AVS_MIN, AVS_STEP);
    REQUIRE(s.request);
    CHECK(s.request_mv % (int32_t)AVS_STEP == 0);
    CHECK(s.request_mv == 20000);  // no-load jumps to user_max 20013, aligned down to 20000
}

// ============================================================================
// Skip-if-unchanged and the regulating flag
// ============================================================================
TEST_CASE("cc: no request when the aligned voltage equals the last request", "[cc]") {
    // No load -> target 20000, last_req already 20000 -> nothing to send.
    CcStep s = computeCcStep(0.0f, 0.0f, 2.0f, 0.0f, /*last_req*/ 20000,
                             20000, /*user_max*/ 20000, PPS_MIN, PPS_STEP);
    CHECK_FALSE(s.request);
}

TEST_CASE("cc: regulating is true only when pulled below the user max", "[cc]") {
    CcStep below = computeCcStep(15.0f, 2.0f, 2.0f, 7.5f, /*last_req*/ 15000,
                                 15000, /*user_max*/ 20000, PPS_MIN, PPS_STEP);
    CHECK(below.regulating);

    CcStep at_max = computeCcStep(20.0f, 2.0f, 2.0f, 10.0f, /*last_req*/ 20000,
                                  20000, /*user_max*/ 20000, PPS_MIN, PPS_STEP);
    CHECK_FALSE(at_max.regulating);

    CcStep never = computeCcStep(20.0f, 2.0f, 2.0f, 10.0f, /*last_req*/ 0,
                                 20000, /*user_max*/ 20000, PPS_MIN, PPS_STEP);
    CHECK_FALSE(never.regulating);  // last_req == 0: hasn't requested yet
}

// ============================================================================
// End-to-end: a resistive load converges to the current target
// ============================================================================
TEST_CASE("cc: converges a resistive load to the current target", "[cc]") {
    // Fixed 12-ohm load. Target 1.5A -> ideal 18.0V. Start at the 20V ceiling
    // (overcurrent) and let the loop settle. Model: measured_a = req_v / 12.
    const float R = 12.0f;
    const float target_a = 1.5f;
    const int32_t user_max = 20000;
    const int32_t floor = PPS_MIN;
    const uint32_t step = PPS_STEP;

    int32_t last_req = user_max;      // start pinned at the ceiling
    float r_est = 0.0f;
    int32_t req_v = user_max;         // currently applied voltage

    bool settled = false;
    for (int i = 0; i < 40 && !settled; i++) {
        float measured_v = req_v / 1000.0f;
        float measured_a = measured_v / R;
        int32_t ref = last_req;
        CcStep s = computeCcStep(measured_v, measured_a, target_a, r_est, last_req,
                                 ref, user_max, floor, step);
        r_est = s.r_estimate;
        if (s.request) {
            last_req = s.request_mv;
            req_v = s.request_mv;
        }
        // Settled once current is within the deadband of target.
        settled = (measured_a > target_a - DEADBAND_A) && (measured_a < target_a + DEADBAND_A);
    }

    REQUIRE(settled);
    // Final applied voltage should be near the ideal 18.0V (within a few steps).
    CHECK(req_v >= 17800);
    CHECK(req_v <= 18200);
}
