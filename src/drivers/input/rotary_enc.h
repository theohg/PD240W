#pragma once

#include "pico/stdlib.h"

class RotaryEncoder {
private:
    volatile int _ticks = 0;
    uint _pinA, _pinB;
    volatile uint8_t _lastState = 0;

    // Sub-position accumulator for full-cycle counting
    // Accumulates ±1 for each valid transition, reports tick at ±4 (full cycle)
    volatile int8_t _subPosition = 0;

    // Debounce timer - 3ms to exceed the 2ms max bounce spec
    volatile uint64_t _last_transition_time_us = 0;
    static constexpr uint32_t DEBOUNCE_TIME_US = 3000;  // 3ms debounce (spec says 2ms max bounce)

    // Velocity tracking for acceleration
    volatile uint64_t _last_tick_time_us = 0;
    volatile uint32_t _tick_interval_us = 1000000;  // Time between last two ticks (default 1s = slow)

public:
    RotaryEncoder(uint pinA, uint pinB);

    /** Initialize GPIOs */
    void init();

    /** Call this from the global GPIO ISR */
    void handleISR(uint gpio, uint32_t events);

    /** Get current tick count */
    int getTicks() const;

    /** Reset tick count to zero */
    void reset();

    /** Helper to check if the interrupt belongs to this encoder */
    bool isMyPin(uint gpio) const;

    /** Get velocity-based multiplier for acceleration (1-25 based on rotation speed) */
    uint32_t getVelocityMultiplier() const;
};
