// Host unit tests for the pure overtemperature FSM in
// src/logic/thermal_monitor.h: the 3-band (caution/warning/fault) latching with
// 2 C hysteresis, edge signalling, and the display-status level. This is the
// exact logic Safety::updateTemperature delegates to.
//
// Thresholds (AppConfig::TEMP_*): caution 50 C, warning 65 C, shutdown 80 C;
// hysteresis 2 C, so a band clears only below (threshold - 2).

#include <snitch/snitch.hpp>

#include "config/app_config.h"
#include "logic/thermal_monitor.h"

using namespace ThermalMonitor;

namespace {

// Advance the FSM one sample, updating `s` in place and returning the transition.
Transition step(State& s, float temp_c) {
    Transition t = evaluate(s, temp_c);
    s = t.state;
    return t;
}

}  // namespace

// ============================================================================
// Baseline
// ============================================================================
TEST_CASE("thermal: cold start is OK with no edges", "[thermal]") {
    State s;
    Transition t = step(s, 25.0f);

    CHECK_FALSE(s.caution_active);
    CHECK_FALSE(s.warning_active);
    CHECK_FALSE(s.fault_active);
    CHECK(s.status == Level::OK);
    CHECK(levelOf(s) == Level::OK);
    CHECK_FALSE(t.entered_caution);
    CHECK_FALSE(t.entered_warning);
    CHECK_FALSE(t.entered_fault);
}

// ============================================================================
// Caution band
// ============================================================================
TEST_CASE("thermal: caution latches at 50 C", "[thermal]") {
    State s;
    Transition t = step(s, 50.0f);

    CHECK(s.caution_active);
    CHECK(s.status == Level::CAUTION);
    CHECK(levelOf(s) == Level::CAUTION);
    CHECK(t.entered_caution);
}

TEST_CASE("thermal: caution below threshold does not latch", "[thermal]") {
    State s;
    step(s, 49.9f);
    CHECK_FALSE(s.caution_active);
    CHECK(s.status == Level::OK);
}

TEST_CASE("thermal: caution hysteresis holds until below 48 C", "[thermal]") {
    State s;
    step(s, 55.0f);           // latch caution
    REQUIRE(s.caution_active);

    step(s, 48.0f);           // 48 is not < (50 - 2) => holds
    CHECK(s.caution_active);

    Transition t = step(s, 47.9f);  // now below 48 => clears
    CHECK_FALSE(s.caution_active);
    CHECK(s.status == Level::OK);
    CHECK(t.cleared_caution);
}

TEST_CASE("thermal: caution entry edge fires once", "[thermal]") {
    State s;
    Transition a = step(s, 52.0f);
    Transition b = step(s, 53.0f);
    CHECK(a.entered_caution);
    CHECK_FALSE(b.entered_caution);  // already latched
}

// ============================================================================
// Warning band
// ============================================================================
TEST_CASE("thermal: warning latches at 65 C and clears caution", "[thermal]") {
    State s;
    step(s, 55.0f);            // caution first
    REQUIRE(s.caution_active);

    Transition t = step(s, 65.0f);
    CHECK(s.warning_active);
    CHECK_FALSE(s.caution_active);   // warning outranks caution
    CHECK(s.status == Level::WARNING);
    CHECK(levelOf(s) == Level::WARNING);
    CHECK(t.entered_warning);
}

TEST_CASE("thermal: warning clears to caution on the way down", "[thermal]") {
    State s;
    step(s, 70.0f);            // warning
    REQUIRE(s.warning_active);

    step(s, 63.0f);            // 63 not < (65 - 2) => warning holds
    CHECK(s.warning_active);

    Transition t = step(s, 62.0f);  // below 63 => warning clears, caution re-latches
    CHECK_FALSE(s.warning_active);
    CHECK(t.cleared_warning);
    CHECK(s.caution_active);         // 62 >= 50
    CHECK(t.entered_caution);
    CHECK(s.status == Level::CAUTION);
}

// ============================================================================
// Fault band (shutdown)
// ============================================================================
TEST_CASE("thermal: fault latches at 80 C with entered_fault edge", "[thermal]") {
    State s;
    Transition t = step(s, 80.0f);
    CHECK(s.fault_active);
    CHECK(s.status == Level::FAULT);
    CHECK(levelOf(s) == Level::FAULT);
    CHECK(t.entered_fault);        // Safety turns this into the load-off cut
}

TEST_CASE("thermal: fault entry edge fires once", "[thermal]") {
    State s;
    Transition a = step(s, 85.0f);
    Transition b = step(s, 82.0f);
    CHECK(a.entered_fault);
    CHECK_FALSE(b.entered_fault);
}

TEST_CASE("thermal: fault hysteresis holds until below 78 C", "[thermal]") {
    State s;
    step(s, 81.0f);            // fault
    REQUIRE(s.fault_active);

    step(s, 78.0f);            // 78 not < (80 - 2) => holds
    CHECK(s.fault_active);

    Transition t = step(s, 77.0f);  // below 78 => clears
    CHECK_FALSE(s.fault_active);
    CHECK(t.cleared_fault);
}

TEST_CASE("thermal: fault ramp-down tracks the latched band (status follows level)", "[thermal]") {
    // On the way down out of fault, warning was latched during the ramp up. The
    // fixed behaviour re-derives `status` (which colours the on-screen
    // temperature) from the final flags every evaluate(), so it drops to WARNING
    // as soon as the fault clears instead of sticking at FAULT.
    State s;
    step(s, 66.0f);            // warning latched
    step(s, 80.0f);            // fault latched; warning_active stays true
    REQUIRE(s.fault_active);
    REQUIRE(s.warning_active);

    step(s, 77.0f);            // fault clears, still in warning band
    CHECK_FALSE(s.fault_active);
    CHECK(s.warning_active);   // still latched
    CHECK(s.status == Level::WARNING);    // status now follows the latched band
    CHECK(levelOf(s) == Level::WARNING);
}

// ============================================================================
// Full sweep up and back down
// ============================================================================
TEST_CASE("thermal: monotonic sweep visits every band", "[thermal]") {
    State s;
    CHECK(levelOf(step(s, 30.0f).state) == Level::OK);
    CHECK(levelOf(step(s, 55.0f).state) == Level::CAUTION);
    CHECK(levelOf(step(s, 70.0f).state) == Level::WARNING);
    CHECK(levelOf(step(s, 85.0f).state) == Level::FAULT);

    // Back down, clearing through hysteresis bands.
    step(s, 77.0f);  // fault clears (warning still latched)
    step(s, 62.0f);  // warning clears -> caution
    CHECK(levelOf(s) == Level::CAUTION);
    step(s, 40.0f);  // caution clears
    CHECK(levelOf(s) == Level::OK);
    CHECK(s.status == Level::OK);
}
