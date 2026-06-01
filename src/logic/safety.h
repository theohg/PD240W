#pragma once

#include <cstdint>
#include "pico/stdlib.h"

// ============================================================================
// Safety Monitoring Module
// ============================================================================
// Monitors safety-critical conditions and triggers appropriate responses.
//
// SAFETY HIERARCHY:
// 1. Overcurrent (INA228 ALERT) - Hardware ISR, immediate load disable
// 2. Overtemperature - Software polling, disable load at threshold
// 3. PD Disconnect - Software polling, disable load when VBUS lost
//
// Note: Overcurrent is handled in interrupts.cpp ISR for fastest response.
// This module handles temperature and PD monitoring in the main loop.
// ============================================================================

enum class SafetyStatus {
    OK,
    CAUTION,
    WARNING,
    FAULT
};

struct SafetyState {
    // Temperature
    float temperature_c;        // NTC thermistor (board temperature)
    float ina_temperature_c;    // INA228 die temperature
    float max_temperature_c;    // Maximum of the two temperatures
    SafetyStatus temp_status;

    // Voltage (VBUS)
    float vbus_voltage_v;
    float ina_voltage_v;
    bool pd_connected;

    // Current
    float current_a;
    bool current_overflow;
    bool overcurrent_latched;

    // Power
    float power_w;
};

class Safety {
public:
    Safety();

    // Initialize safety monitoring
    void init();

    // Main update function - call every iteration of main loop
    // Returns overall safety status
    SafetyStatus update();

    // Get current safety state
    const SafetyState& getState() const { return _state; }

    // Check specific conditions
    bool isOvertemperature() const;
    bool isOvertemperatureWarning() const;
    bool isPdConnected() const;
    bool isOvercurrentLatched() const;

    // Clear overcurrent latch (requires user action)
    void clearOvercurrentLatch();

    // Set current limit for software monitoring
    void setCurrentLimit(float limit_a);

private:
    SafetyState _state;
    float _current_limit_a;

    // Timing for periodic checks
    absolute_time_t _last_temp_check;
    absolute_time_t _last_voltage_check;

    // Temperature hysteresis
    bool _temp_caution_active;
    bool _temp_warning_active;
    bool _temp_fault_active;

    // Update individual monitors
    void updateTemperature();
    void updateVoltage();
    void updateCurrent();
};

// Global instance
extern Safety safety;
