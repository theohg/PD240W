#include "interrupts.h"
#include "hardware.h"
#include "config/board_config.h"

namespace Interrupts {

// =========================================================================
// Deferred Processing Flags (volatile for ISR-main loop communication)
// =========================================================================
volatile bool overcurrentTriggered = false;
volatile bool pdInterruptPending = false;
volatile bool btn1Triggered = false;
volatile bool btn2Triggered = false;
volatile bool btnEncTriggered = false;

// ISR Debounce Timers (keep track of last valid press time)
volatile uint64_t last_btn1_time = 0;
volatile uint64_t last_btn2_time = 0;
volatile uint64_t last_btn_enc_time = 0;

// Debounce threshold in Microseconds (15ms is usually perfect for clicks)
constexpr uint64_t BTN_DEBOUNCE_US = 15000;

// =========================================================================
// Individual ISR Handlers
// =========================================================================

// SAFETY-CRITICAL: Overcurrent protection - executes immediately
// This is an exception to the "no hardware ops in ISR" rule because
// cutting power cannot wait for main loop (component damage risk)
//
// IMPORTANT: The alert pin goes low when the switch is disabled (no current flow).
// We must only trigger overcurrent if the switch was supposed to be ON.
static void isrOvercurrent(uint gpio, uint32_t events) {
    // Only trigger if switch is currently enabled (or was just enabled)
    // Reading GPIO directly is safe in ISR
    if (gpio_get(Board::PIN_SWITCH_EN)) {
        hw.loadSwitch.off();           // Cut power FIRST - safety critical
        overcurrentTriggered = true;   // Signal main loop for logging/UI
    }
    // If switch is off, ignore the alert - it's just the switch being disabled
}

// USB-PD interrupt handler (TPS26750 INT pin)
// Just sets flag - I2C read happens in main loop
static void isrUsbPd(uint gpio, uint32_t events) {
    pdInterruptPending = true;
}

inline bool handleButtonISR(volatile uint64_t &last_time) {
    uint64_t now = to_us_since_boot(get_absolute_time());
    if ((now - last_time) > BTN_DEBOUNCE_US) {
        last_time = now;
        return true;
    }
    return false;
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
    if (gpio == Board::PIN_SWITCH_EN_READ) { isrOvercurrent(gpio, events); }

    // USB-PD interrupt (deferred to main loop)
    if (gpio == Board::PIN_USB_PD_IRQ) { isrUsbPd(gpio, events); }

    if (events & GPIO_IRQ_EDGE_FALL) {
        if (gpio == Board::PIN_BTN_1) {
            if (handleButtonISR(last_btn1_time)) btn1Triggered = true;
        }
        else if (gpio == Board::PIN_BTN_2) {
            if (handleButtonISR(last_btn2_time)) btn2Triggered = true;
        }
        else if (gpio == Board::PIN_ENC_BTN) {
            if (handleButtonISR(last_btn_enc_time)) btnEncTriggered = true;
        }
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
    gpio_set_irq_enabled(Board::PIN_BTN_1, GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_enabled(Board::PIN_BTN_2, GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_enabled(Board::PIN_ENC_BTN, GPIO_IRQ_EDGE_FALL, true);
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

bool checkBtn1Clicked() {
    if (btn1Triggered) {
        btn1Triggered = false; // Clear flag
        return true;
    }
    return false;
}

bool checkBtn2Clicked() {
    if (btn2Triggered) {
        btn2Triggered = false;
        return true;
    }
    return false;
}

bool checkBtnEncClicked() {
    if (btnEncTriggered) {
        btnEncTriggered = false;
        return true;
    }
    return false;
}

} // namespace Interrupts
