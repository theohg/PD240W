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

    // Read current state
    int MSB = gpio_get(_pinA);
    int LSB = gpio_get(_pinB);

    int encoded = (MSB << 1) | LSB;
    int sum = (_lastEncoded << 2) | encoded;

    // Determine direction based on state transitions
    // CW (clockwise)
    if(sum == 0b1101 || sum == 0b0100 || sum == 0b0010 || sum == 0b1011) {
        _ticks++;
        _last_change_time_us = current_time_us;  // Update debounce time on valid transition
    }
    // CCW (counter-clockwise)
    else if(sum == 0b1110 || sum == 0b0111 || sum == 0b0001 || sum == 0b1000) {
        _ticks--;
        _last_change_time_us = current_time_us;  // Update debounce time on valid transition
    }

    _lastEncoded = encoded;
}