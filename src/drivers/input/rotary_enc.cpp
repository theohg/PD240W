#include "drivers/input/rotary_enc.h"
#include "hardware/gpio.h"

// =============================================================================
// Quadrature Direction Lookup Table
// =============================================================================
// Index = (lastState << 2) | currentState
// Value = direction: +1 = CW, -1 = CCW, 0 = invalid/same
//
// Gray code sequence:
//   CW:  00 → 01 → 11 → 10 → 00
//   CCW: 00 → 10 → 11 → 01 → 00
//
static const int8_t DIRECTION_TABLE[16] = {
     0,  // 0b0000: 00→00 (no change)
    +1,  // 0b0001: 00→01 (CW)
    -1,  // 0b0010: 00→10 (CCW)
     0,  // 0b0011: 00→11 (invalid - skipped state)
    -1,  // 0b0100: 01→00 (CCW)
     0,  // 0b0101: 01→01 (no change)
     0,  // 0b0110: 01→10 (invalid - skipped state)
    +1,  // 0b0111: 01→11 (CW)
    +1,  // 0b1000: 10→00 (CW)
     0,  // 0b1001: 10→01 (invalid - skipped state)
     0,  // 0b1010: 10→10 (no change)
    -1,  // 0b1011: 10→11 (CCW)
     0,  // 0b1100: 11→00 (invalid - skipped state)
    -1,  // 0b1101: 11→01 (CCW)
    +1,  // 0b1110: 11→10 (CW)
     0,  // 0b1111: 11→11 (no change)
};

RotaryEncoder::RotaryEncoder(uint pinA, uint pinB) : _pinA(pinA), _pinB(pinB) {
}

void RotaryEncoder::init() {
    gpio_init(_pinA);
    gpio_set_dir(_pinA, GPIO_IN);
    gpio_disable_pulls(_pinA);

    gpio_init(_pinB);
    gpio_set_dir(_pinB, GPIO_IN);
    gpio_disable_pulls(_pinB);

    // Initialize state from current pin values
    _lastState = (gpio_get(_pinA) << 1) | gpio_get(_pinB);
    _last_tick_time_us = time_us_64();
}

bool RotaryEncoder::isMyPin(uint gpio) const {
    return (gpio == _pinA || gpio == _pinB);
}

int RotaryEncoder::getTicks() const {
    return _ticks;
}

void RotaryEncoder::reset() {
    _ticks = 0;
}

uint32_t RotaryEncoder::getVelocityMultiplier() const {
    // Returns 1-25 based on rotation speed
    // Fast rotation (< 30ms between ticks) = high multiplier
    // Slow rotation (> 200ms between ticks) = multiplier of 1
    
    uint32_t interval_ms = _tick_interval_us / 1000;
    
    if (interval_ms > 200) return 1;      // Very slow: fine adjustment (1x)
    if (interval_ms > 100) return 2;      // Slow: small steps (2x)
    if (interval_ms > 60)  return 5;      // Medium: moderate steps (5x)
    if (interval_ms > 30)  return 10;     // Fast: larger steps (10x)
    return 25;                             // Very fast: coarse adjustment (25x)
}

void RotaryEncoder::handleISR(uint gpio, uint32_t events) {
    uint64_t current_time_us = time_us_64();

    // Read current state of both pins
    uint8_t currentState = (gpio_get(_pinA) << 1) | gpio_get(_pinB);

    // Skip if state hasn't actually changed (spurious interrupt)
    if (currentState == _lastState) {
        return;
    }

    // Debounce: ignore transitions during bounce period
    // But still update state to track position!
    if (current_time_us - _last_transition_time_us < DEBOUNCE_TIME_US) {
        _lastState = currentState;
        return;
    }
    _last_transition_time_us = current_time_us;

    // =========================================================================
    // Full-Cycle Accumulator Decoding
    // =========================================================================
    // This approach accumulates sub-steps and only reports a tick when a
    // complete cycle is detected. This provides:
    //   - Immunity to bounce (wrong direction cancels out)
    //   - Immunity to partial movements (pressing on knob)
    //   - No assumption about detent position
    //   - Exactly 1 tick per physical detent click
    //
    // Most encoders with detents have 1 detent per full electrical cycle
    // (4 state transitions). We accumulate direction and report at ±4.
    // =========================================================================

    uint8_t transition = (_lastState << 2) | currentState;
    int8_t direction = DIRECTION_TABLE[transition];

    _lastState = currentState;

    // Invalid transition (noise, or skipped state) - ignore
    if (direction == 0) {
        return;
    }

    // Accumulate sub-position
    _subPosition += direction;

    // Clamp to prevent overflow on erratic input
    if (_subPosition > 4) _subPosition = 4;
    if (_subPosition < -4) _subPosition = -4;

    // Report tick when full cycle completed
    if (_subPosition >= 4) {
        _ticks++;
        _subPosition = 0;
        // Update velocity tracking
        _tick_interval_us = current_time_us - _last_tick_time_us;
        _last_tick_time_us = current_time_us;
    } 
    else if (_subPosition <= -4) {
        _ticks--;
        _subPosition = 0;
        // Update velocity tracking
        _tick_interval_us = current_time_us - _last_tick_time_us;
        _last_tick_time_us = current_time_us;
    }
}
