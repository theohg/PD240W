#pragma once
#include "pico/stdlib.h"

// ============================================================================
// Interrupt Module
// ============================================================================
// Centralizes all GPIO interrupt handling for the RP2040.
//
// RP2040 LIMITATION: Only ONE gpio callback can be registered for ALL pins.
// This module provides the single callback router and individual handlers.
//
// ISR SAFETY RULES:
// - NEVER do I2C/SPI operations inside ISRs (blocking, can cause deadlocks)
// - NEVER call printf/LOG inside ISRs
// - Use volatile flags to signal main loop for deferred processing
// - Keep ISRs as short as possible
//
// EXCEPTION: Safety-critical operations (overcurrent) execute immediately.
// ============================================================================

namespace Interrupts {

// =========================================================================
// Deferred Processing Flags
// =========================================================================
// These flags are set by ISRs and checked in the main loop.
// Main loop handles the actual processing (I2C reads, logging, UI updates).

extern volatile bool overcurrentTriggered;
extern volatile bool pdInterruptPending;

// =========================================================================
// Initialization
// =========================================================================

/**
 * Initialize all GPIO interrupts.
 * Must be called after hw.init() since it uses hw.encoder.
 *
 * Sets up:
 * - Encoder A/B (quadrature decoding)
 * - Overcurrent alert (safety-critical, immediate action)
 * - USB-PD interrupt (deferred to main loop)
 */
void init();

// =========================================================================
// Main Loop Handlers
// =========================================================================
// Call these in the main loop to handle deferred interrupt processing.

/**
 * Check and handle overcurrent flag.
 * @return true if overcurrent was triggered (flag is auto-cleared)
 */
bool handleOvercurrent();

/**
 * Check and handle USB-PD interrupt flag.
 * @return true if PD interrupt was pending (flag is auto-cleared)
 */  
bool handlePdInterrupt();

bool checkBtn1Clicked();
bool checkBtn2Clicked();
bool checkBtnEncClicked();

} // namespace Interrupts
