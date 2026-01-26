#include "pico/stdlib.h"
#include <stdio.h>
#include "hardware.h"
#include "interrupts.h"
#include "utils/eeprom_loader.h"
#include "utils/logging.h"
#include "config/version.h"
#include "config/app_config.h"
#include "logic/state_machine.h"
#include "logic/settings.h"
#include "logic/safety.h"
#include "logic/pd_manager.h"
#include "ui/display_manager.h"

// ============================================================================
// PD240W Power Supply - Main Application
// ============================================================================
// Phase 3: Application Logic with State Machine
//
// Architecture:
// - State machine controls application flow (BOOT -> MAIN <-> MENU <-> ADJUST, FAULT)
// - Safety module monitors temperature, voltage, current
// - PD manager handles USB-PD contract negotiation
// - Display manager renders screens based on state
// - All modules use non-blocking patterns
// ============================================================================

// Display refresh timing
static absolute_time_t next_display_update;

int main() {

    LOG_SEPARATOR();
    LOG_INFO("%s %s - %s", Version::PRODUCT_NAME, Version::PRODUCT_SUBTITLE, Version::FIRMWARE_VERSION);
    LOG_SEPARATOR();

    // =========================================================================
    // Phase 1: Hardware Initialization
    // =========================================================================

    // Initialize hardware (I2C, SPI, all drivers)
    hw.init();

    // Attempt to flash TPS26750 EEPROM if enabled
    if (!flashTps26750Eeprom()) {
        LOG_ERROR("EEPROM flashing failed! Halting.");
        while (true) {
            hw.rgbLed.setColor(255, 0, 0, 255);
            sleep_ms(500);
            hw.rgbLed.setColor(0, 0, 0, 0);
            sleep_ms(500);
        }
    }

    // Setup all GPIO interrupts (encoder, overcurrent, USB-PD)
    Interrupts::init();

    // =========================================================================
    // Phase 2: Module Initialization
    // =========================================================================

    // Initialize settings
    settings.init();

    // Initialize safety monitoring
    safety.init();

    // Initialize PD manager
    pdManager.init();

    // Initialize display manager
    displayManager.init();

    // Initialize state machine (starts in BOOT state)
    stateMachine.init();

    // Setup display refresh timer
    next_display_update = get_absolute_time();

    LOG_SEPARATOR();

    // =========================================================================
    // Main Event Loop
    // =========================================================================

    while (true) {
        // ---------------------------------------------------------------------
        // 1. Update State Machine
        // ---------------------------------------------------------------------
        // Handles encoder input, button events, and state transitions
        bool needs_display_update = stateMachine.update();

        // ---------------------------------------------------------------------
        // 2. Update Safety Module
        // ---------------------------------------------------------------------
        // Monitors temperature, voltage, and current
        SafetyStatus safety_status = safety.update();

        // Transition to FAULT state if safety issue detected
        // IMPORTANT: Skip fault transitions during BOOT state to allow boot sequence to complete
        if (safety_status == SafetyStatus::FAULT && stateMachine.getState() != AppState::BOOT) {
            if (safety.isOvertemperature()) {
                stateMachine.setFault(FaultType::OVERTEMPERATURE);
            } else if (!safety.isPdConnected()) {
                stateMachine.setFault(FaultType::PD_DISCONNECT);
            }
            needs_display_update = true;
        }

        // ---------------------------------------------------------------------
        // 3. Update PD Manager
        // ---------------------------------------------------------------------
        // Handles USB-PD interrupt processing and negotiation state
        pdManager.update();

        // ---------------------------------------------------------------------
        // 4. Update Display
        // ---------------------------------------------------------------------
        // Render at fixed rate (100ms) or when state changes
        if (needs_display_update ||
            absolute_time_diff_us(next_display_update, get_absolute_time()) >= 0) {

            displayManager.render();
            next_display_update = make_timeout_time_ms(AppConfig::DISPLAY_UPDATE_MS);
        }

        // ---------------------------------------------------------------------
        // 5. Update Hardware
        // ---------------------------------------------------------------------
        // Required for RGB LED blinking and other timed operations
        hw.update();
    }

    return 0;
}
