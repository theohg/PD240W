#include "drivers/input/rotary_enc.h"
#include "hardware/gpio.h"

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

void RotaryEncoder::handleISR(uint gpio, uint32_t events) {
    // Debouncing: Check if enough time has passed since last change
    uint64_t current_time_us = time_us_64();
    if (current_time_us - _last_change_time_us < DEBOUNCE_TIME_US) {
        return;  // Ignore this transition (too fast, likely bounce)
    }
    _last_change_time_us = current_time_us;

    // Read current state of both pins
    uint8_t currentState = (gpio_get(_pinA) << 1) | gpio_get(_pinB);

    // Skip if state hasn't actually changed (noise)
    if (currentState == _lastState) {
        return;
    }

    // State machine for quadrature decoding
    // Only count on specific transitions for 1 tick per detent
    // Gray code sequence: 00 -> 01 -> 11 -> 10 -> 00 (CW)
    //                     00 -> 10 -> 11 -> 01 -> 00 (CCW)

    // Combine last and current state for transition detection
    uint8_t transition = (_lastState << 2) | currentState;

    // CW transitions: 00->01, 01->11, 11->10, 10->00
    // Only count on the 11->10 transition (one tick per detent)
    if (transition == 0b1110) {  // 11 -> 10
        _ticks++;
    }
    // CCW transitions: 00->10, 10->11, 11->01, 01->00
    // Only count on the 11->01 transition (one tick per detent)
    else if (transition == 0b1101) {  // 11 -> 01
        _ticks--;
    }

    _lastState = currentState;
}
