#include "interrupts.h"
#include "hardware.h"
#include "board_config.h"

namespace Interrupts {

// =========================================================================
// Deferred Processing Flags (volatile for ISR-main loop communication)
// =========================================================================
volatile bool overcurrentTriggered = false;
volatile bool pdInterruptPending = false;

// =========================================================================
// Individual ISR Handlers
// =========================================================================

// SAFETY-CRITICAL: Overcurrent protection - executes immediately
// This is an exception to the "no hardware ops in ISR" rule because
// cutting power cannot wait for main loop (component damage risk)
static void isrOvercurrent(uint gpio, uint32_t events) {
    hw.loadSwitch.off();           // Cut power FIRST - safety critical
    overcurrentTriggered = true;   // Signal main loop for logging/UI
}

// USB-PD interrupt handler (TPS26750 INT pin)
// Just sets flag - I2C read happens in main loop
static void isrUsbPd(uint gpio, uint32_t events) {
    pdInterruptPending = true;
}

// =========================================================================
// GPIO Callback Router
// =========================================================================
// RP2040 allows only ONE gpio callback for ALL pins.
// This function routes interrupts to the appropriate handler.

static void gpioCallback(uint gpio, uint32_t events) {
    // Encoder (quadrature decoding - timing sensitive, GPIO only)
    if (hw.encoder.isMyPin(gpio)) {
        hw.encoder.handleISR(gpio, events);
    }

    // Overcurrent (safety-critical - immediate action)
    if (gpio == Board::PIN_SWITCH_EN_READ) {
        isrOvercurrent(gpio, events);
    }

    // USB-PD interrupt (deferred to main loop)
    if (gpio == Board::PIN_USB_PD_IRQ) {
        isrUsbPd(gpio, events);
    }
}

// =========================================================================
// Initialization
// =========================================================================

void init() {
    // Register the single GPIO callback (RP2040 requirement)
    // Only the FIRST call to gpio_set_irq_enabled_with_callback registers the callback.
    // Subsequent calls to gpio_set_irq_enabled just enable interrupts on additional pins.
    gpio_set_irq_enabled_with_callback(
        Board::PIN_ENC_A,
        GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE,
        true,
        &gpioCallback
    );

    // Enable interrupts on additional pins (callback already registered)
    gpio_set_irq_enabled(Board::PIN_ENC_B, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true);
    gpio_set_irq_enabled(Board::PIN_SWITCH_EN_READ, GPIO_IRQ_EDGE_FALL, true);  // Overcurrent (active low)
    gpio_set_irq_enabled(Board::PIN_USB_PD_IRQ, GPIO_IRQ_EDGE_FALL, true);      // PD interrupt (active low)
}

// =========================================================================
// Main Loop Handlers
// =========================================================================

bool handleOvercurrent() {
    if (overcurrentTriggered) {
        overcurrentTriggered = false;
        return true;
    }
    return false;
}

bool handlePdInterrupt() {
    if (pdInterruptPending) {
        pdInterruptPending = false;
        return true;
    }
    return false;
}

} // namespace Interrupts
