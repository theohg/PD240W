#include "tps_eeprom_workflow.h"
#include "hardware.h"
#include "utils/logging.h"
#include "utils/tps_eeprom_loader.h"
#include "drivers/buzzer/buzzer.h"

// Global instance
TpsEepromWorkflow tpsEepromWorkflow;

// ============================================================================
// Progress Callback (static, forwards to instance)
// ============================================================================

static void progressCallback(uint8_t phase, uint8_t progress, void* user_data) {
    TpsEepromWorkflow* wf = static_cast<TpsEepromWorkflow*>(user_data);
    wf->setProgress(phase, progress);
}

// ============================================================================
// Constructor
// ============================================================================

TpsEepromWorkflow::TpsEepromWorkflow()
    : _active(false)
    , _stage(TpsEepromWorkflowStage::COMPARING)
    , _phase(0)
    , _progress(0)
    , _result(false)
    , _confirm_yes(false)
    , _message(nullptr)
{
}

// ============================================================================
// Public Methods
// ============================================================================

void TpsEepromWorkflow::start() {
    _active = true;
    _stage = TpsEepromWorkflowStage::COMPARING;
    _phase = 0;
    _progress = 0;
    _result = false;
    _confirm_yes = false;
    _message = "Initializing...";

    LOG_INFO("EEPROM workflow started");

    // Run comparison immediately
    runCompare();
}

bool TpsEepromWorkflow::handleInput(bool rotate, bool click) {
    switch (_stage) {
        case TpsEepromWorkflowStage::COMPARING:
            // No user input during comparison
            break;

        case TpsEepromWorkflowStage::CONFIRM:
            if (rotate) {
                _confirm_yes = !_confirm_yes;
            } else if (click) {
                if (_confirm_yes) {
                    // User confirmed - start flash
                    _stage = TpsEepromWorkflowStage::FLASHING;
                    _message = "Flashing...";
                    _progress = 0;
                    runFlash();
                } else {
                    // User cancelled
                    cleanup();
                    return true;  // Exit workflow
                }
            }
            break;

        case TpsEepromWorkflowStage::FLASHING:
            // No user input during flashing (blocking operation)
            break;

        case TpsEepromWorkflowStage::DONE:
            if (click) {
                cleanup();
                return true;  // Exit workflow
            }
            break;
    }

    return false;  // Stay in workflow
}

void TpsEepromWorkflow::setProgress(uint8_t phase, uint8_t progress) {
    _phase = phase;
    _progress = progress;
    _message = (phase == 0) ? "Writing..." : "Verifying...";
}

void TpsEepromWorkflow::cleanup() {
    if (_active) {
        eepromDeinit();
        _active = false;
        LOG_INFO("EEPROM workflow cleanup");
    }
}

// ============================================================================
// Internal Workflow Steps
// ============================================================================

void TpsEepromWorkflow::runCompare() {
    LOG_INFO("Starting EEPROM compare...");

    // Initialize I2C1 for EEPROM access
    if (!eepromInit()) {
        _message = "I2C init failed";
        _stage = TpsEepromWorkflowStage::DONE;
        _result = false;
        return;
    }

    // Probe for device
    if (!eepromProbe()) {
        _message = "EEPROM not found";
        _stage = TpsEepromWorkflowStage::DONE;
        _result = false;
        return;
    }

    // Compare EEPROM against firmware
    EepromCompareResult result = eepromCompare();

    switch (result) {
        case EepromCompareResult::IDENTICAL:
            _message = "Config identical";
            _stage = TpsEepromWorkflowStage::DONE;
            _result = false;  // No flash was performed
            eepromDeinit();   // Release I2C resources
            break;

        case EepromCompareResult::EMPTY:
            _message = "EEPROM empty";
            _stage = TpsEepromWorkflowStage::CONFIRM;
            break;

        case EepromCompareResult::DIFFERENT:
            _message = "Different config";
            _stage = TpsEepromWorkflowStage::CONFIRM;
            break;

        case EepromCompareResult::NO_DEVICE:
            _message = "No EEPROM found";
            _stage = TpsEepromWorkflowStage::DONE;
            _result = false;
            eepromDeinit();
            break;

        case EepromCompareResult::READ_ERROR:
        default:
            _message = "Read error";
            _stage = TpsEepromWorkflowStage::DONE;
            _result = false;
            eepromDeinit();
            break;
    }
}

void TpsEepromWorkflow::runFlash() {
    LOG_INFO("Starting EEPROM flash...");

    // Execute flash with progress callback
    _result = eepromFlash(progressCallback, this);

    // Move to done stage
    _stage = TpsEepromWorkflowStage::DONE;

    if (_result) {
        _message = "Success! Power cycle";
        // Success melody
        hw.buzzer.playTone(880, 80);   // A5
        sleep_ms(80);
        hw.buzzer.playTone(1175, 80);  // D6
        sleep_ms(80);
        hw.buzzer.playTone(1397, 150); // F6
        LOG_INFO("EEPROM flash successful");
    } else {
        _message = "Flash failed!";
        hw.buzzer.playTone(200, 300);  // Error beep
        LOG_ERROR("EEPROM flash failed");
    }
}
