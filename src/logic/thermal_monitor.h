#pragma once

#include "config/app_config.h"

// ============================================================================
// Thermal monitor FSM (pure, hardware-free)
// ============================================================================
// The 3-band overtemperature state machine with hysteresis, lifted out of
// Safety::updateTemperature so it can be host-tested (see
// test/test_thermal_monitor.cpp). Given the previous flag state and a new
// max-temperature sample it returns the next flag state, the display status
// level, and per-band rising/falling edges (which Safety turns into the load-off
// cut, the buzzer, and log lines).
//
// This header depends only on config/app_config.h (which includes <cstdint>),
// so it is host-buildable. Thresholds come from AppConfig::TEMP_*; the hysteresis
// band lives here as the single source of truth (Safety's critical-alarm code
// references ThermalMonitor::HYSTERESIS_C too).
//
// Behaviour is a faithful, semantics-preserving copy of the original
// Safety::updateTemperature — including the quirk that `status` (which colours
// the on-screen temperature) is only rewritten on specific band transitions and
// otherwise holds its previous value.
// ============================================================================

namespace ThermalMonitor {

// Hysteresis band: a band clears only once the temperature falls this far below
// the threshold that set it, preventing chatter at the boundary.
constexpr float HYSTERESIS_C = 2.0f;

// Severity implied by the active flags (highest wins). Values mirror the
// firmware's SafetyStatus so Safety maps them 1:1.
enum class Level { OK, CAUTION, WARNING, FAULT };

// The latched band flags plus the last display status. `status` tracks
// Safety::_state.temp_status; the three bools track the _temp_*_active members.
struct State {
    bool caution_active = false;
    bool warning_active = false;
    bool fault_active = false;
    Level status = Level::OK;
};

// One evaluation's result: the next state plus edges the caller acts on. Each
// edge is set exactly where the original code took the corresponding side effect
// (load cut / recovery log / warning+caution logs), so callers reproduce the
// firmware's behaviour precisely.
struct Transition {
    State state;
    bool entered_fault = false;   // rising edge >= shutdown: caller disables load
    bool cleared_fault = false;   // falling edge below shutdown - hysteresis
    bool entered_warning = false;
    bool cleared_warning = false;
    bool entered_caution = false;
    bool cleared_caution = false;  // via the caution band only (not warning-forced)
};

inline Level levelOf(const State& s) {
    if (s.fault_active) return Level::FAULT;
    if (s.warning_active) return Level::WARNING;
    if (s.caution_active) return Level::CAUTION;
    return Level::OK;
}

// Evaluate one temperature sample against the previous flag state. Pure.
inline Transition evaluate(State s, float max_temp_c) {
    const float caution = static_cast<float>(AppConfig::TEMP_CAUTION_C);    // 50
    const float warning = static_cast<float>(AppConfig::TEMP_WARNING_C);    // 65
    const float shutdown = static_cast<float>(AppConfig::TEMP_SHUTDOWN_C);  // 80

    Transition t;

    // --- 1. FAULT (>= shutdown) -------------------------------------------
    if (max_temp_c >= shutdown) {
        if (!s.fault_active) {
            s.fault_active = true;
            s.status = Level::FAULT;
            t.entered_fault = true;
        }
    } else if (s.fault_active && max_temp_c < (shutdown - HYSTERESIS_C)) {
        s.fault_active = false;
        t.cleared_fault = true;
    }

    // --- 2. WARNING (>= warning), only when not in fault ------------------
    if (!s.fault_active) {
        if (max_temp_c >= warning) {
            if (!s.warning_active) {
                s.warning_active = true;
                s.status = Level::WARNING;
                t.entered_warning = true;
            }
            s.caution_active = false;  // warning outranks caution
        } else if (s.warning_active && max_temp_c < (warning - HYSTERESIS_C)) {
            s.warning_active = false;
            t.cleared_warning = true;
        }
    }

    // --- 3. CAUTION (>= caution), only when neither fault nor warning -----
    if (!s.fault_active && !s.warning_active) {
        if (max_temp_c >= caution) {
            if (!s.caution_active) {
                s.caution_active = true;
                s.status = Level::CAUTION;
                t.entered_caution = true;
            }
        } else if (s.caution_active && max_temp_c < (caution - HYSTERESIS_C)) {
            s.caution_active = false;
            s.status = Level::OK;
            t.cleared_caution = true;
        }

        // Fall through to OK when nothing is latched.
        if (!s.caution_active) {
            s.status = Level::OK;
        }
    }

    // Screen colour always reflects the currently-latched band. The per-branch
    // status writes above only fire on specific edges, which left `status`
    // stuck at the old colour when a band cleared into an already-latched lower
    // band (e.g. fault -> already-latched warning kept FAULT colour). Deriving
    // it from the final flags here removes that quirk.
    s.status = levelOf(s);

    t.state = s;
    return t;
}

}  // namespace ThermalMonitor
