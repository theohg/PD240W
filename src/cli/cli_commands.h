#pragma once

// ============================================================================
// CLI Command Handlers
// ============================================================================
// Each function handles one SCPI-style command.
// arg == nullptr means query (?), otherwise arg is the parameter string.
// ============================================================================

namespace CliCmd {

// Identity & System
void idn(const char* arg);
void systStat(const char* arg);
void systUptime(const char* arg);
void systReboot(const char* arg);
void systBootsel(const char* arg);
void systLoc(const char* arg);

// Output Control
void outpSw(const char* arg);
void outpBuck(const char* arg);

// Measurements
void measVolt(const char* arg);
void measCurr(const char* arg);
void measPow(const char* arg);
void measTemp(const char* arg);
void measItemp(const char* arg);
void measEnergy(const char* arg);
void measVbus(const char* arg);
void measAll(const char* arg);

// PD Contract Management
void pdList(const char* arg);
void pdActive(const char* arg);
void pdRev(const char* arg);
void pdSel(const char* arg);
void pdPps(const char* arg);
void pdAvs(const char* arg);

// Current Limit
void currLim(const char* arg);

// Settings
void settBright(const char* arg);
void settSound(const char* arg);
void settAutoPps(const char* arg);
void settAutoAvs(const char* arg);
void settAutoOut(const char* arg);
void settDim(const char* arg);
void settSave(const char* arg);
void settReset(const char* arg);

}  // namespace CliCmd
