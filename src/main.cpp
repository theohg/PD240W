#include "pico/stdlib.h"
#include <stdio.h>
#include "hardware.h"
#include "interrupts.h"
#include "utils/logging.h"

// ============================================================================
// PHASE 2 TEST - TPS26750 USB PD Controller
// ============================================================================

// ============================================================================
// Global State
// ============================================================================

SourceCapability contracts[13];  // TPS26750 supports up to 13 PDOs (7 SPR + 6 EPR)
uint8_t num_contracts = 0;
int8_t selected_contract = 0;    // Currently selected contract index
bool negotiation_in_progress = false;
absolute_time_t negotiation_timeout;

// ============================================================================
// Display Helper Functions
// ============================================================================

void displayHeader() {
    hw.display.fillScreen(ST7789::COLOR_BLACK);
    hw.display.drawString(10, 10, "USB PD Contract Test", ST7789::COLOR_WHITE, ST7789::COLOR_BLACK, 2);
    hw.display.drawLine(10, 40, 230, 40, ST7789::COLOR_GREEN);
}

void displayContracts() {
    // Clear contract display area
    hw.display.fillRect(10, 50, 220, 200, ST7789::COLOR_BLACK);

    if (num_contracts == 0) {
        hw.display.drawString(10, 50, "No contracts found!", ST7789::COLOR_RED, ST7789::COLOR_BLACK, 1);
        return;
    }

    // Display each contract
    int y = 50;
    for (uint8_t i = 0; i < num_contracts && i < 10; i++) {
        // Highlight selected contract
        uint16_t color = (i == selected_contract) ? ST7789::COLOR_YELLOW : ST7789::COLOR_WHITE;
        uint16_t bg = (i == selected_contract) ? ST7789::COLOR_BLUE : ST7789::COLOR_BLACK;

        // Format: "5V @ 3A" or "5-21V PPS"
        char line[32];
        if (contracts[i].is_pps) {
            snprintf(line, sizeof(line), "%u-%uV PPS %umA",
                     (unsigned)(contracts[i].min_voltage_mv / 1000),
                     (unsigned)(contracts[i].voltage_mv / 1000),
                     (unsigned)contracts[i].max_current_ma);
        } else if (contracts[i].is_avs) {
            snprintf(line, sizeof(line), "%u-%uV AVS %umA",
                     (unsigned)(contracts[i].min_voltage_mv / 1000),
                     (unsigned)(contracts[i].voltage_mv / 1000),
                     (unsigned)contracts[i].max_current_ma);
        } else {
            snprintf(line, sizeof(line), "%uV @ %umA",
                     (unsigned)(contracts[i].voltage_mv / 1000),
                     (unsigned)contracts[i].max_current_ma);
        }

        // Draw selection indicator
        if (i == selected_contract) {
            hw.display.drawString(5, y, ">", ST7789::COLOR_YELLOW, ST7789::COLOR_BLACK, 1);
        }

        hw.display.drawString(20, y, line, color, bg, 1);
        y += 15;
    }
}

void displayActiveContract() {
    // Display active contract at bottom
    hw.display.fillRect(10, 260, 220, 50, ST7789::COLOR_BLACK);
    hw.display.drawString(10, 260, "Active:", ST7789::COLOR_CYAN, ST7789::COLOR_BLACK, 1);

    uint32_t voltage_mv, current_ma;
    if (hw.pdController.getActiveContract(voltage_mv, current_ma)) {
        char line[32];
        snprintf(line, sizeof(line), "%.2fV @ %.2fA", voltage_mv / 1000.0f, current_ma / 1000.0f);
        hw.display.drawString(10, 275, line, ST7789::COLOR_GREEN, ST7789::COLOR_BLACK, 2);

        // Also display on serial
        LOG_INFO("Active contract: %.2fV @ %.2fA", voltage_mv / 1000.0f, current_ma / 1000.0f);
    } else {
        hw.display.drawString(10, 275, "Error reading!", ST7789::COLOR_RED, ST7789::COLOR_BLACK, 1);
    }
}

void displayStatus(const char* msg, uint16_t color) {
    hw.display.fillRect(10, 300, 220, 15, ST7789::COLOR_BLACK);
    hw.display.drawString(10, 300, msg, color, ST7789::COLOR_BLACK, 1);
}

// ============================================================================
// Main
// ============================================================================

int main() {
    LOG_SEPARATOR();
    LOG_INFO("PD240W Phase 2 - TPS26750 Test");
    LOG_SEPARATOR();

    // Initialize hardware
    hw.init();

    // Setup all GPIO interrupts (encoder, overcurrent, USB-PD)
    Interrupts::init();

    // Initialize display
    displayHeader();
    displayStatus("Initializing...", ST7789::COLOR_YELLOW);

    // Wait for TPS26750 to enter APP mode (may take time after power-up)
    char mode[5];
    const int MAX_MODE_RETRIES = 50;  // 50 * 100ms = 5 seconds max
    bool in_app_mode = false;

    for (int i = 0; i < MAX_MODE_RETRIES; i++) {
        if (hw.pdController.getMode(mode)) {
            LOG_INFO("TPS26750 Mode: %s (attempt %d)", mode, i + 1);

            // Check if in APP mode (mode string starts with "APP")
            if (mode[0] == 'A' && mode[1] == 'P' && mode[2] == 'P') {
                in_app_mode = true;
                break;
            }

            // Still booting (PTCH/BOOT mode) - wait and retry
            char statusMsg[32];
            snprintf(statusMsg, sizeof(statusMsg), "Waiting for PD... (%d)", i + 1);
            displayStatus(statusMsg, ST7789::COLOR_YELLOW);
            sleep_ms(100);
        } else {
            LOG_ERROR("Failed to read TPS26750 MODE register!");
            displayStatus("TPS26750 Error!", ST7789::COLOR_RED);
            while (1) { tight_loop_contents(); }  // Halt on error
        }
    }

    if (!in_app_mode) {
        LOG_ERROR("TPS26750 failed to enter APP mode (stuck in %s)", mode);
        displayStatus("PD Not Ready!", ST7789::COLOR_RED);
        // Continue anyway - user can see the issue
    } else {
        // Give time for initial PD negotiation with charger
        displayStatus("Negotiating PD...", ST7789::COLOR_YELLOW);
        sleep_ms(500);
    }

    // Get available contracts
    LOG_INFO("Reading available contracts...");
    num_contracts = hw.pdController.getSourceCapabilities(contracts, 13);
    LOG_INFO("Found %u contracts:", num_contracts);

    for (uint8_t i = 0; i < num_contracts; i++) {
        if (contracts[i].is_pps) {
            LOG_INFO("  [%u] %u-%uV PPS @ %umA", i,
                     contracts[i].min_voltage_mv / 1000,
                     contracts[i].voltage_mv / 1000,
                     contracts[i].max_current_ma);
        } else if (contracts[i].is_avs) {
            LOG_INFO("  [%u] %u-%uV AVS @ %umA", i,
                     contracts[i].min_voltage_mv / 1000,
                     contracts[i].voltage_mv / 1000,
                     contracts[i].max_current_ma);
        } else {
            LOG_INFO("  [%u] %uV @ %umA", i,
                     contracts[i].voltage_mv / 1000,
                     contracts[i].max_current_ma);
        }
    }

    // Display contracts on screen
    displayContracts();
    displayActiveContract();
    displayStatus("Use encoder to select", ST7789::COLOR_GREEN);

    // LED status
    hw.rgbLed.setColor(0, 255, 0, 50); // Green - ready

    LOG_SEPARATOR();
    LOG_INFO("Test Controls:");
    LOG_INFO("  Encoder: Select contract");
    LOG_INFO("  BTN1: Request selected contract");
    LOG_INFO("  Encoder Button: Toggle output switch");
    LOG_SEPARATOR();

    // Encoder tracking
    int last_encoder_ticks = 0;
    hw.encoder.reset();

    // Main loop
    while (true) {
        // ===== Encoder: Select Contract =====
        int current_ticks = hw.encoder.getTicks();
        if (current_ticks != last_encoder_ticks && !negotiation_in_progress) {
            int delta = current_ticks - last_encoder_ticks;
            last_encoder_ticks = current_ticks;

            selected_contract += delta;

            // Wrap around
            if (selected_contract < 0) selected_contract = num_contracts - 1;
            if (selected_contract >= num_contracts) selected_contract = 0;

            LOG_DEBUG("Selected contract: %d", selected_contract);
            displayContracts();
        }

        // ===== BTN1: Request Selected Contract =====
        if (hw.btn1.isClicked() && !negotiation_in_progress) {
            LOG_INFO("Requesting contract [%d]...", selected_contract);
            displayStatus("Negotiating...", ST7789::COLOR_YELLOW);
            hw.rgbLed.setColor(255, 255, 0, 100); // Yellow - negotiating

            bool success = false;

            if (contracts[selected_contract].is_pps) {
                // Request PPS at minimum voltage (for safety)
                success = hw.pdController.requestPPSProfile(
                    contracts[selected_contract].min_voltage_mv,
                    contracts[selected_contract].max_current_ma
                );
            } else if (contracts[selected_contract].is_avs) {
                // Request AVS at minimum voltage (for safety)
                success = hw.pdController.requestAVSProfile(
                    contracts[selected_contract].min_voltage_mv,
                    contracts[selected_contract].max_current_ma
                );
            } else {
                // Request fixed contract
                success = hw.pdController.requestFixedProfile(
                    contracts[selected_contract].voltage_mv,
                    contracts[selected_contract].max_current_ma
                );
            }

            if (success) {
                negotiation_in_progress = true;
                negotiation_timeout = make_timeout_time_ms(2000); // 2s timeout
            } else {
                LOG_ERROR("Failed to request contract!");
                displayStatus("Request failed!", ST7789::COLOR_RED);
                hw.rgbLed.setColor(255, 0, 0, 100); // Red - error
            }
        }

        // ===== Check Negotiation Status =====
        if (negotiation_in_progress) {
            // Check for timeout
            if (absolute_time_diff_us(get_absolute_time(), negotiation_timeout) < 0) {
                LOG_WARN("Negotiation timeout!");
                displayStatus("Timeout!", ST7789::COLOR_RED);
                negotiation_in_progress = false;
                hw.rgbLed.setColor(255, 0, 0, 100); // Red
            } else {
                // Check for new contract event
                uint8_t events[11] = {0};
                if (hw.pdController.readInterrupts(events)) {
                    if (hw.pdController.isInterruptSet(events, 12)) { // NEW_CONTRACT_AS_SINK
                        LOG_INFO("New contract negotiated!");
                        displayStatus("Success!", ST7789::COLOR_GREEN);
                        displayActiveContract();
                        negotiation_in_progress = false;
                        hw.rgbLed.setColor(0, 255, 0, 100); // Green

                        // Clear interrupt
                        uint8_t clear_mask[11] = {0};
                        clear_mask[1] = (1 << 4); // Bit 12
                        hw.pdController.clearInterrupts(clear_mask);
                    }
                }
            }
        }

        // ===== Encoder Button: Toggle Output Switch =====
        if (hw.btnEnc.isClicked()) {
            // Clear INA228 fault latch
            hw.powerMonitor.getDiagnoseAlert();

            bool current_state = hw.loadSwitch.read();
            if (current_state) {
                hw.loadSwitch.off();
                LOG_INFO("Output DISABLED");
            } else {
                hw.loadSwitch.on();
                LOG_INFO("Output ENABLED");
            }
        }

        // ===== Handle Overcurrent Flag (deferred logging/UI) =====
        if (Interrupts::handleOvercurrent()) {
            hw.rgbLed.setColor(255, 0, 0, 255);  // Red
            LOG_ERROR("OVERCURRENT DETECTED - LOAD DISABLED");
            displayStatus("OVERCURRENT!", ST7789::COLOR_RED);
        }

        // ===== Handle USB-PD Interrupt Flag =====
        if (Interrupts::handlePdInterrupt()) {
            // Read interrupt events from TPS26750 (safe here - main loop context)
            uint8_t events[11] = {0};
            if (hw.pdController.readInterrupts(events)) {
                LOG_DEBUG("PD Interrupt received, events[0]=0x%02X events[1]=0x%02X", events[0], events[1]);

                // Check for relevant events and handle them
                // Bit 12: NEW_CONTRACT_AS_SINK
                if (hw.pdController.isInterruptSet(events, 12)) {
                    LOG_INFO("PD: New contract negotiated (interrupt)");
                    displayActiveContract();

                    // Clear the interrupt
                    uint8_t clear_mask[11] = {0};
                    clear_mask[1] = (1 << 4);  // Bit 12
                    hw.pdController.clearInterrupts(clear_mask);
                }

                // TODO: Handle other TPS26750 interrupt events as needed
                // See TPS26750 datasheet INT_EVENT1 register for full list
            }
        }

        // ===== Update hardware (RGB LED, debug LED, etc.) =====
        hw.update();
    }

    return 0;
}
