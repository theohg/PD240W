#include "drivers/gpio/gpio.h"

SimpleIO::SimpleIO(int p, IOMode mode) : pin(p), is_input(mode == INPUT) {
    gpio_init(pin);
    gpio_set_dir(pin, is_input ? GPIO_IN : GPIO_OUT);
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