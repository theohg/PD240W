#pragma once

#include <cstdint>

// ============================================================================
// Serial CLI Module
// ============================================================================
// SCPI-inspired text command interface over UART or USB CDC.
// Non-blocking: call update() every main loop iteration.
//
// Protocol:
//   Queries:  MEAS:VOLT?\n  →  20000\n
//   Sets:     OUTP ON\n     →  OK\n
//   Errors:   FOOBAR\n      →  ERR UNKNOWN_CMD\n
//
// Remote mode: receiving any command enters REMOTE state (front panel locked).
//   Long-press encoder or SYST:LOC returns to local control.
// ============================================================================

namespace Cli {

// Maximum command line length
constexpr uint16_t CLI_BUF_SIZE = 128;

// Initialize CLI (call once after all singletons init)
void init();

// Process incoming serial data (non-blocking, call every main loop iteration)
void update();

// Check if serial logging is enabled (checked by LOG macros)
bool isLogEnabled();

// Check if device is in remote control mode
bool isRemoteMode();

// Exit remote mode (called by encoder long-press handler)
void exitRemoteMode();

// Send a response line (appends \n)
void respond(const char* msg);

// Send an error response: "ERR <code>\n"
void error(const char* code);

}  // namespace Cli
