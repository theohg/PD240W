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

    // Utility
    bool isInput() const { return is_input; }
    bool isOutput() const { return !is_input; }
};