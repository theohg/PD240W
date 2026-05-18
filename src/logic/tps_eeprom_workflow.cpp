#include "tps_eeprom_workflow.h"
#include "hardware.h"
#include "config/app_config.h"
#include "utils/logging.h"
#include "utils/tps_eeprom_loader.h"
#include "drivers/buzzer/buzzer.h"

// Global instance
TpsEepromWorkflow tpsEepromWorkflow;

namespace {

const Note EEPROM_FLASH_SUCCESS_MELODY[] = {
    {880, 80},
    {1175, 80},
    {1397, 150},
};

constexpr uint8_t EEPROM_FLASH_SUCCESS_MELODY_LENGTH =
    sizeof(EEPROM_FLASH_SUCCESS_MELODY) / sizeof(Note);

}  // namespace

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
    , _flash_session{}
    , _completion_pending(false)
    , _completion_ready_time{}
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
    _flash_session = {};
    _completion_pending = false;

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
            // No user input during flashing
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

bool TpsEepromWorkflow::update() {
    uint8_t previous_stage = static_cast<uint8_t>(_stage);
    uint8_t previous_phase = _phase;
    bool previous_result = _result;
    const char* previous_message = _message;

    if (_stage == TpsEepromWorkflowStage::FLASHING) {
        if (_completion_pending) {
            if (absolute_time_diff_us(_completion_ready_time, get_absolute_time()) >= 0) {
                _completion_pending = false;
                _stage = TpsEepromWorkflowStage::DONE;
                _message = "Success! Power cycle";
                hw.buzzer.playMelody(EEPROM_FLASH_SUCCESS_MELODY,
                                     EEPROM_FLASH_SUCCESS_MELODY_LENGTH);
                LOG_INFO("EEPROM flash successful");
            }
        } else {
            EepromFlashStatus status = eepromFlashStep(&_flash_session, progressCallback, this);
            if (status == EepromFlashStatus::SUCCESS) {
                _result = true;
                _phase = 1;
                _progress = 100;
                _completion_pending = true;
                _completion_ready_time = make_timeout_time_ms(220);
            } else if (status == EepromFlashStatus::ERROR) {
                _result = false;
                _stage = TpsEepromWorkflowStage::DONE;
                _message = _flash_session.error_message ? _flash_session.error_message : "Flash failed!";
                hw.buzzer.playTone(AppConfig::BEEP_ERROR_FREQ, AppConfig::BEEP_ERROR_DURATION);
                LOG_ERROR("EEPROM flash failed");
            }
        }
    }

    return previous_stage != static_cast<uint8_t>(_stage)
        || previous_phase != _phase
        || previous_result != _result
        || previous_message != _message;
}

void TpsEepromWorkflow::setProgress(uint8_t phase, uint8_t progress) {
    _phase = phase;
    _progress = progress;
    _message = (phase == 0) ? "Writing..." : "Verifying...";
}

void TpsEepromWorkflow::cleanup() {
    if (_active) {
        _flash_session = {};
        _completion_pending = false;
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

    _flash_session = {};
    _completion_pending = false;
    _result = false;

    if (!eepromFlashBegin(&_flash_session)) {
        _stage = TpsEepromWorkflowStage::DONE;
        _message = _flash_session.error_message ? _flash_session.error_message : "Flash failed!";
        hw.buzzer.playTone(AppConfig::BEEP_ERROR_FREQ, AppConfig::BEEP_ERROR_DURATION);
        LOG_ERROR("EEPROM flash failed");
        return;
    }

    setProgress(0, 0);
}
