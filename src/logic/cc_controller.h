#pragma once

#include <cstdint>
#include "settings.h"
#include "pico/stdlib.h"

// ============================================================================
// Constant Current Controller
// ============================================================================
// P-controller that regulates output current by requesting PD voltage changes.
// When measured current exceeds target, requests lower voltage (and vice versa).
// Uses PPS/AVS keep-alive mechanism for voltage adjustments.
//
// Hardware OCP (INA228 ALERT) remains as safety net:
//   OCP threshold = CC target + CC_SAFETY_MARGIN_MA, capped at 5A
// ============================================================================

namespace CcController {

    // Initialize CC controller state
    void init();

    // Main update loop - call from main.cpp after pdManager.update()
    // Runs P-controller at CC_POLL_INTERVAL_MS (10ms / 100Hz)
    void update();

    // Set current limit operating mode (OFF, OCP, CC)
    void setMode(CurrentLimitMode mode);
    CurrentLimitMode getMode();
    bool isDisabled();

    // Enable/disable CC mode (compatibility wrapper for CC/OCP)
    void setEnabled(bool enabled);
    bool isEnabled();

    // Set target current in mA (clamped to CURRENT_LIMIT range)
    void setTargetCurrentMa(uint32_t target_ma);
    uint32_t getTargetCurrentMa();

    // Check if CC is actively regulating (enabled + output on + PPS/AVS active)
    bool isRegulating();

}  // namespace CcController
