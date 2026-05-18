#pragma once

#include <cstdint>
#include "utils/tps_eeprom_loader.h"

// ============================================================================
// TPS26750 EEPROM Flash Workflow Controller
// ============================================================================
// Manages the TPS26750 EEPROM flashing workflow with a simple state machine.
// Extracted from state_machine.cpp for better separation of concerns.
//
// Workflow stages:
//   0 = Comparing (init, probe, compare)
//   1 = Confirm (user Yes/No selection)
//   2 = Flashing (write + verify with progress)
//   3 = Done (show result, wait for dismiss)
// ============================================================================

// Workflow stages (exposed for display manager)
enum class TpsEepromWorkflowStage : uint8_t {
    COMPARING = 0,  // Initializing and comparing
    CONFIRM = 1,    // Awaiting user confirmation
    FLASHING = 2,   // Write/verify in progress
    DONE = 3        // Complete, awaiting dismissal
};

class TpsEepromWorkflow {
public:
    TpsEepromWorkflow();

    // Start the workflow (call when entering EEPROM flash mode)
    void start();

    // Handle user input (rotate toggles Yes/No, click confirms)
    // Returns true if workflow is complete (should exit)
    bool handleInput(bool rotate, bool click);

    // Advance background work for the active workflow.
    // Returns true when the display should refresh.
    bool update();

    // Check if workflow is active
    bool isActive() const { return _active; }

    // Get current stage for display
    TpsEepromWorkflowStage getStage() const { return _stage; }

    // Get current phase (0=write, 1=verify) during FLASHING stage
    uint8_t getPhase() const { return _phase; }

    // Get progress (0-100%) during FLASHING stage
    uint8_t getProgress() const { return _progress; }

    // Get result (valid only in DONE stage)
    bool getResult() const { return _result; }

    // Get Yes/No selection state (valid only in CONFIRM stage)
    bool isConfirmYes() const { return _confirm_yes; }

    // Get status message for display
    const char* getMessage() const { return _message; }

    // Called by progress callback during flash
    void setProgress(uint8_t phase, uint8_t progress);

    // Cleanup resources (call when exiting workflow)
    void cleanup();

private:
    bool _active;
    TpsEepromWorkflowStage _stage;
    uint8_t _phase;
    uint8_t _progress;
    bool _result;
    bool _confirm_yes;
    const char* _message;
    EepromFlashSession _flash_session;
    bool _completion_pending;
    absolute_time_t _completion_ready_time;

    // Internal workflow steps
    void runCompare();
    void runFlash();
};

// Global instance
extern TpsEepromWorkflow tpsEepromWorkflow;
