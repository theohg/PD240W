#pragma once
#include "pico/stdlib.h" 

// Enum to make pull-up/down configuration readable
enum ButtonPull {
    PULL_UP,
    PULL_DOWN,
    PULL_NONE
};

class Button {
    uint pin;
    uint32_t debounce_delay_ms;
    bool active_low;
    // State tracking for debounce
    bool last_steady_state;             // The last stable state (pressed/not pressed)
    bool last_flickerable_state;        // The raw state from the previous read
    absolute_time_t last_debounce_time; // Timestamp of the last state change
    // State tracking for edge detection (each button needs its own)
    bool was_pressed_for_click;         // Previous state for isClicked() edge detection
public:
    /**
     * @param p: The GPIO pin number
     * @param pull_config: PULL_UP (default for buttons to GND), PULL_DOWN, or PULL_NONE
     * @param debounce_ms: Time in ms to wait for signal stability (default 50ms)
     * @param active_low: True if button connects to GND (default), False if to VCC
     */
    Button(uint p, ButtonPull pull_config = PULL_UP, uint32_t debounce_ms = 50, bool active_low = true);

    // Returns true if the button is currently held down (stable)
    bool isPressed();
    
    // Returns true ONLY ONCE when the button is first pressed (rising edge)
    // Useful for toggling things so they don't toggle 100 times a second.
    bool isClicked();
};