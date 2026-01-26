#pragma once
#include "pico/stdlib.h"

// Enum to make IO mode configuration readable
enum IOMode {
    INPUT,
    OUTPUT,
};

class SimpleIO {
    const uint pin;
    bool is_input;

    // Non-blocking blink state
    bool _is_blinking;
    uint32_t _blink_interval_ms;
    uint32_t _blink_duration_ms;
    uint32_t _last_blink_time;
    uint32_t _blink_start_time;
    bool _blink_state_on;

public:
    /**
     * @param p: The GPIO pin number
     * @param mode: INPUT or OUTPUT mode
     */
    SimpleIO(int p, IOMode mode);

    // Output methods
    void on();
    void off();
    void toggle();

    // Input methods
    bool read() const;       // Read current pin state
    bool get() const;        // Alias for read() for consistency

    // Non-blocking blink (output only)
    /**
     * Start blinking non-blocking
     * @param interval_ms Duration of ON and OFF states in milliseconds
     * @param duration_ms Total blink duration (0 = infinite)
     */
    void startBlink(uint32_t interval_ms, uint32_t duration_ms = 0);

    /** Stop blinking and turn off */
    void stopBlink();

    /** Must be called in main loop to handle blinking */
    void update();

    /** Check if currently blinking */
    bool isBlinking() const { return _is_blinking; }

    // Utility
    bool isInput() const { return is_input; }
    bool isOutput() const { return !is_input; }
};