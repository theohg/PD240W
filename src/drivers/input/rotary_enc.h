#pragma once

#include "pico/stdlib.h"

class RotaryEncoder {
private:
    volatile int _ticks = 0;
    uint _pinA, _pinB;
    volatile int _lastEncoded = 0;

    // Debouncing
    volatile uint64_t _last_change_time_us = 0;
    static constexpr uint32_t DEBOUNCE_TIME_US = 2000;  // 2ms debounce

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
};
