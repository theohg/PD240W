#include "cli.h"
#include "cli_commands.h"
#include <cstdio>
#include <cstring>
#include <cctype>
#include "pico/stdlib.h"

// ============================================================================
// CLI Core — Line buffering, command dispatch, response formatting
// ============================================================================

namespace Cli {

// Line buffer
static char _buf[CLI_BUF_SIZE];
static uint16_t _buf_pos = 0;

// State
static bool _remote_mode = false;
static bool _line_overflowed = false;  // True when the current line exceeded the buffer

}  // namespace Cli (temporary close for global definition)

// Global log suppression flag (extern declared in logging.h)
bool g_cli_log_enabled = true;

namespace Cli {

// Command dispatch table entry
struct CommandEntry {
    const char* name;          // Command name (e.g., "MEAS:VOLT")
    bool is_query_only;        // True if only ? form is valid (set form -> INVALID_PARAM)
    bool is_action;            // True if there is no query form (? form -> NOT_QUERYABLE).
                               // Action commands ignore their arg and DO something, so the
                               // ? form must never reach the handler (would reboot/erase/etc).
    void (*handler)(const char* arg);  // Handler: arg=nullptr for query, arg=param for set
};

// The command table references the CliCmd:: handlers directly (their signature
// already matches CommandEntry::handler). Only LOG:ON/OFF need local wrappers
// because they toggle Cli-internal state rather than calling into CliCmd.
static void cmd_log_on(const char* arg);
static void cmd_log_off(const char* arg);

// Command table — linear scan is fine for ~35 commands
static const CommandEntry commands[] = {
    //             name           query_only  action  handler
    // Identity & Status
    {"*IDN",         true,  false, CliCmd::idn},
    {"SYST:STAT",    true,  false, CliCmd::systStat},
    {"SYST:UPTIME",  true,  false, CliCmd::systUptime},
    {"SYST:REBOOT",  false, true,  CliCmd::systReboot},
    {"SYST:BOOTSEL", false, true,  CliCmd::systBootsel},
    {"SYST:LOC",     false, true,  CliCmd::systLoc},

    // Output control (queryable: ? returns ON/OFF)
    {"OUTP:SW",      false, false, CliCmd::outpSw},
    {"OUTP:BUCK",    false, false, CliCmd::outpBuck},

    // Measurements
    {"MEAS:VOLT",    true,  false, CliCmd::measVolt},
    {"MEAS:CURR",    true,  false, CliCmd::measCurr},
    {"MEAS:POW",     true,  false, CliCmd::measPow},
    {"MEAS:TEMP",    true,  false, CliCmd::measTemp},
    {"MEAS:ITEMP",   true,  false, CliCmd::measItemp},
    {"MEAS:ENERGY",  true,  false, CliCmd::measEnergy},
    {"MEAS:VBUS",    true,  false, CliCmd::measVbus},
    {"MEAS:ALL",     true,  false, CliCmd::measAll},

    // PD Contract Management (PD:SEL/PPS/AVS are set-only actions, no query form)
    {"PD:LIST",      true,  false, CliCmd::pdList},
    {"PD:ACTIVE",    true,  false, CliCmd::pdActive},
    {"PD:REV",       true,  false, CliCmd::pdRev},
    {"PD:SEL",       false, true,  CliCmd::pdSel},
    {"PD:PPS",       false, true,  CliCmd::pdPps},
    {"PD:AVS",       false, true,  CliCmd::pdAvs},

    // Current Limit (queryable)
    {"CURR:LIM",     false, false, CliCmd::currLim},

    // Settings (queryable: ? returns current value)
    {"SETT:BRIGHT",  false, false, CliCmd::settBright},
    {"SETT:SOUND",   false, false, CliCmd::settSound},
    {"SETT:AUTOPPS", false, false, CliCmd::settAutoPps},
    {"SETT:AUTOAVS", false, false, CliCmd::settAutoAvs},
    {"SETT:AUTOOUT", false, false, CliCmd::settAutoOut},
    {"SETT:DIM",     false, false, CliCmd::settDim},
    {"SETT:SAVE",    false, true,  CliCmd::settSave},
    {"SETT:RESET",   false, true,  CliCmd::settReset},

    // TPS26750 diagnostics / config test
    {"TPS:MODE",     true,  false, CliCmd::tpsMode},
    {"TPS:GARBAGE",  false, true,  CliCmd::tpsGarbage},

    // Logging
    {"LOG:ON",       false, true,  cmd_log_on},
    {"LOG:OFF",      false, true,  cmd_log_off},
};

static constexpr int NUM_COMMANDS = sizeof(commands) / sizeof(commands[0]);

// -------------------------------------------------------------------------
// Internal helpers
// -------------------------------------------------------------------------

// Uppercase only the command token (up to the first space) in-place. Command
// matching stays case-insensitive while any argument after the space keeps its
// original case, so a future case-sensitive argument is not corrupted.
// (Case-insensitive keyword args like ON/OFF are handled by the parsers.)
static void toUpperCommandToken(char* s) {
    for (; *s && *s != ' ' && *s != '\t'; ++s) {
        if (*s >= 'a' && *s <= 'z') *s -= 32;
    }
}

// Process a complete command line
static void processCommand(char* line) {
    // Case-insensitive command matching: uppercase the command token only,
    // leaving the argument (if any) untouched.
    toUpperCommandToken(line);

    // Strip trailing whitespace
    int len = strlen(line);
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t')) {
        line[--len] = '\0';
    }

    if (len == 0) return;

    // Check if it's a query (ends with ?)
    bool is_query = (line[len - 1] == '?');

    // Find command name vs argument
    // For queries: "MEAS:VOLT?" -> name="MEAS:VOLT", arg=nullptr
    // For sets:    "OUTP:SW ON" -> name="OUTP:SW", arg="ON"
    // For actions: "SYST:REBOOT" -> name="SYST:REBOOT", arg=""(empty)
    char* cmd_name = line;
    const char* arg = nullptr;

    if (is_query) {
        line[len - 1] = '\0';  // Remove ?
    } else {
        // Find first space separating command from argument
        char* space = strchr(line, ' ');
        if (space) {
            *space = '\0';
            arg = space + 1;
            // Skip leading whitespace in argument
            while (*arg == ' ' || *arg == '\t') arg++;
        } else {
            arg = "";  // No argument (action command)
        }
    }

    // Search command table
    for (int i = 0; i < NUM_COMMANDS; i++) {
        if (strcmp(cmd_name, commands[i].name) == 0) {
            // Only a recognized command enters remote mode, so line noise on the
            // UART can't lock the front panel. Set before dispatch so SYST:LOC's
            // handler (which exits remote mode) still wins.
            _remote_mode = true;

            if (is_query) {
                if (commands[i].is_action) {
                    // Action-only command: reject the query form instead of
                    // executing it (e.g. "SYST:REBOOT?", "TPS:GARBAGE?").
                    error("NOT_QUERYABLE");
                } else {
                    // Query form: pass nullptr
                    commands[i].handler(nullptr);
                }
            } else if (commands[i].is_query_only) {
                error("INVALID_PARAM");
            } else {
                commands[i].handler(arg);
            }
            return;
        }
    }

    error("UNKNOWN_CMD");
}

// -------------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------------

void init() {
    _buf_pos = 0;
    _line_overflowed = false;
    g_cli_log_enabled = true;
    _remote_mode = false;
}

void update() {
    int ch;
    while ((ch = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
        if (ch == '\n' || ch == '\r') {
            if (_line_overflowed) {
                // The line exceeded the buffer: discard it entirely instead of
                // executing a truncated (and potentially different) command.
                error("LINE_TOO_LONG");
                _line_overflowed = false;
                _buf_pos = 0;
            } else if (_buf_pos > 0) {
                _buf[_buf_pos] = '\0';
                processCommand(_buf);
                _buf_pos = 0;
            }
        } else if (_buf_pos < CLI_BUF_SIZE - 1) {
            _buf[_buf_pos++] = static_cast<char>(ch);
        } else {
            // Buffer full: flag overflow and drop remaining chars until newline.
            _line_overflowed = true;
        }
    }
}

bool isLogEnabled() {
    return g_cli_log_enabled;
}

bool isRemoteMode() {
    return _remote_mode;
}

void exitRemoteMode() {
    _remote_mode = false;
}

void respond(const char* msg) {
    printf("%s\n", msg);
}

void error(const char* code) {
    printf("ERR %s\n", code);
}

// =========================================================================
// Command handler implementations
// =========================================================================
// LOG:ON/OFF toggle Cli-internal logging state, so they stay here. Every other
// command in the table references its CliCmd:: handler directly.

static void cmd_log_on(const char* /*arg*/)       { g_cli_log_enabled = true; respond("OK"); }
static void cmd_log_off(const char* /*arg*/)      { g_cli_log_enabled = false; respond("OK"); }

}  // namespace Cli
