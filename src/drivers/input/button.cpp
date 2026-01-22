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
    // 1. Read the physical pin
    bool raw_read = gpio_get(pin);
    
    // 2. Normalize logic (so true = pressed, regardless of wiring)
    bool current_state = active_low ? !raw_read : raw_read;

    // 3. Check if the physical state has changed since last poll
    if (current_state != last_flickerable_state) {
        // Reset the debounce timer
        last_debounce_time = get_absolute_time();
        last_flickerable_state = current_state;
    }

    // 4. Check if enough time has passed to consider this stable
    if (absolute_time_diff_us(last_debounce_time, get_absolute_time()) > debounce_delay_ms * 1000) {
        // If the state has indeed changed, update our steady state
        if (last_steady_state != current_state) {
            last_steady_state = current_state;
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