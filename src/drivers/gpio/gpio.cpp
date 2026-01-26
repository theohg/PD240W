#include "drivers/gpio/gpio.h"

SimpleIO::SimpleIO(int p, IOMode mode) : pin(p), is_input(mode == INPUT) {
    gpio_init(pin);
    gpio_set_dir(pin, is_input ? GPIO_IN : GPIO_OUT);

    // Initialize blink state
    _is_blinking = false;
    _blink_interval_ms = 0;
    _blink_duration_ms = 0;
    _last_blink_time = 0;
    _blink_start_time = 0;
    _blink_state_on = false;
}

// Output methods - write to pin
void SimpleIO::on() {
    gpio_put(pin, true);
}

void SimpleIO::off() {
    gpio_put(pin, false);
}

void SimpleIO::toggle() {
    gpio_xor_mask(1ul << pin);
}

// Input methods - read from pin
bool SimpleIO::read() const {
    return gpio_get(pin);
}

bool SimpleIO::get() const {
    return read();  // Alias for consistency with some APIs
}

// Non-blocking blink implementation
void SimpleIO::startBlink(uint32_t interval_ms, uint32_t duration_ms) {
    if (is_input) return;  // Only works for outputs

    _blink_interval_ms = interval_ms;
    _blink_duration_ms = duration_ms;  // 0 = infinite
    _is_blinking = true;
    _blink_state_on = true;

    uint32_t now = to_ms_since_boot(get_absolute_time());
    _last_blink_time = now;
    _blink_start_time = now;

    // Start in ON state
    on();
}

void SimpleIO::stopBlink() {
    _is_blinking = false;
    _blink_duration_ms = 0;
    off();
}

void SimpleIO::update() {
    if (!_is_blinking) return;

    uint32_t now = to_ms_since_boot(get_absolute_time());

    // Check for duration expiration (if duration > 0)
    if (_blink_duration_ms > 0) {
        if ((now - _blink_start_time) >= _blink_duration_ms) {
            stopBlink();
            return;
        }
    }

    // Check for blink toggle
    if ((now - _last_blink_time) >= _blink_interval_ms) {
        _blink_state_on = !_blink_state_on;
        _last_blink_time = now;

        if (_blink_state_on) {
            on();
        } else {
            off();
        }
    }
}