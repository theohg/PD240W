#include "drivers/input/button.h"

Button::Button(uint p, ButtonPull pull_config, uint32_t debounce_ms, bool active_low_mode) {
    pin = p;
    debounce_delay_ms = debounce_ms;
    active_low = active_low_mode;

    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);

    // Configure Internal Resistors
    if (pull_config == PULL_UP) {
        gpio_pull_up(pin);
    } else if (pull_config == PULL_DOWN) {
        gpio_pull_down(pin);
    } else {
        gpio_disable_pulls(pin);
    }

    // Initialize state
    // If active low, "true" (pressed) means gpio is false (low)
    bool initial_read = gpio_get(pin);
    
    // Normalize state: if active low, invert the read so 'true' always means 'pressed'
    bool logic_state = active_low ? !initial_read : initial_read;

    last_steady_state = logic_state;
    last_flickerable_state = logic_state;
    last_debounce_time = get_absolute_time();
    was_pressed_for_click = false;  // Initialize click detection state
}

bool Button::isPressed() {
    bool raw_read = gpio_get(pin);
    bool current_state = active_low ? !raw_read : raw_read;
    uint64_t now = get_absolute_time();

    // Check if the physical state is different from what we think the steady state is
    if (current_state != last_steady_state) {
        
        // Only accept this change if enough time has passed since the LAST change
        // This effectively "locks out" the noise after a valid transition.
        if (absolute_time_diff_us(last_debounce_time, now) > debounce_delay_ms * 1000) {
            
            last_steady_state = current_state;
            last_debounce_time = now; // Record the time of this valid change
        }
    }

    return last_steady_state;
}

// Simple edge detection wrapper
bool Button::isClicked() {
    bool is_now_pressed = isPressed();

    // Detect rising edge (button just pressed)
    if (is_now_pressed && !was_pressed_for_click) {
        was_pressed_for_click = true;
        return true; // Just pressed
    } else if (!is_now_pressed) {
        was_pressed_for_click = false; // Reset when released
    }

    return false;
}