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

// Forward declarations of all command handlers
static void cmd_idn(const char* arg);
static void cmd_syst_stat(const char* arg);
static void cmd_syst_uptime(const char* arg);
static void cmd_syst_reboot(const char* arg);
static void cmd_syst_bootsel(const char* arg);
static void cmd_syst_loc(const char* arg);
static void cmd_outp_sw(const char* arg);
static void cmd_outp_buck(const char* arg);
static void cmd_meas_volt(const char* arg);
static void cmd_meas_curr(const char* arg);
static void cmd_meas_pow(const char* arg);
static void cmd_meas_temp(const char* arg);
static void cmd_meas_itemp(const char* arg);
static void cmd_meas_energy(const char* arg);
static void cmd_meas_vbus(const char* arg);
static void cmd_meas_all(const char* arg);
static void cmd_pd_list(const char* arg);
static void cmd_pd_active(const char* arg);
static void cmd_pd_rev(const char* arg);
static void cmd_pd_sel(const char* arg);
static void cmd_pd_pps(const char* arg);
static void cmd_pd_avs(const char* arg);
static void cmd_curr_lim(const char* arg);
static void cmd_sett_bright(const char* arg);
static void cmd_sett_sound(const char* arg);
static void cmd_sett_autopps(const char* arg);
static void cmd_sett_autoavs(const char* arg);
static void cmd_sett_autoout(const char* arg);
static void cmd_sett_dim(const char* arg);
static void cmd_sett_save(const char* arg);
static void cmd_sett_reset(const char* arg);
static void cmd_tps_mode(const char* arg);
static void cmd_tps_garbage(const char* arg);
static void cmd_log_on(const char* arg);
static void cmd_log_off(const char* arg);

// Command table — linear scan is fine for ~35 commands
static const CommandEntry commands[] = {
    //             name           query_only  action  handler
    // Identity & Status
    {"*IDN",         true,  false, cmd_idn},
    {"SYST:STAT",    true,  false, cmd_syst_stat},
    {"SYST:UPTIME",  true,  false, cmd_syst_uptime},
    {"SYST:REBOOT",  false, true,  cmd_syst_reboot},
    {"SYST:BOOTSEL", false, true,  cmd_syst_bootsel},
    {"SYST:LOC",     false, true,  cmd_syst_loc},

    // Output control (queryable: ? returns ON/OFF)
    {"OUTP:SW",      false, false, cmd_outp_sw},
    {"OUTP:BUCK",    false, false, cmd_outp_buck},

    // Measurements
    {"MEAS:VOLT",    true,  false, cmd_meas_volt},
    {"MEAS:CURR",    true,  false, cmd_meas_curr},
    {"MEAS:POW",     true,  false, cmd_meas_pow},
    {"MEAS:TEMP",    true,  false, cmd_meas_temp},
    {"MEAS:ITEMP",   true,  false, cmd_meas_itemp},
    {"MEAS:ENERGY",  true,  false, cmd_meas_energy},
    {"MEAS:VBUS",    true,  false, cmd_meas_vbus},
    {"MEAS:ALL",     true,  false, cmd_meas_all},

    // PD Contract Management (PD:SEL/PPS/AVS are set-only actions, no query form)
    {"PD:LIST",      true,  false, cmd_pd_list},
    {"PD:ACTIVE",    true,  false, cmd_pd_active},
    {"PD:REV",       true,  false, cmd_pd_rev},
    {"PD:SEL",       false, true,  cmd_pd_sel},
    {"PD:PPS",       false, true,  cmd_pd_pps},
    {"PD:AVS",       false, true,  cmd_pd_avs},

    // Current Limit (queryable)
    {"CURR:LIM",     false, false, cmd_curr_lim},

    // Settings (queryable: ? returns current value)
    {"SETT:BRIGHT",  false, false, cmd_sett_bright},
    {"SETT:SOUND",   false, false, cmd_sett_sound},
    {"SETT:AUTOPPS", false, false, cmd_sett_autopps},
    {"SETT:AUTOAVS", false, false, cmd_sett_autoavs},
    {"SETT:AUTOOUT", false, false, cmd_sett_autoout},
    {"SETT:DIM",     false, false, cmd_sett_dim},
    {"SETT:SAVE",    false, true,  cmd_sett_save},
    {"SETT:RESET",   false, true,  cmd_sett_reset},

    // TPS26750 diagnostics / config test
    {"TPS:MODE",     true,  false, cmd_tps_mode},
    {"TPS:GARBAGE",  false, true,  cmd_tps_garbage},

    // Logging
    {"LOG:ON",       false, true,  cmd_log_on},
    {"LOG:OFF",      false, true,  cmd_log_off},
};

static constexpr int NUM_COMMANDS = sizeof(commands) / sizeof(commands[0]);

// -------------------------------------------------------------------------
// Internal helpers
// -------------------------------------------------------------------------

// Uppercase a string in-place
static void toUpper(char* s) {
    for (; *s; ++s) {
        if (*s >= 'a' && *s <= 'z') *s -= 32;
    }
}

// Process a complete command line
static void processCommand(char* line) {
    // Uppercase the line for case-insensitive matching
    toUpper(line);

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
// Defined here in cli.cpp — they call into cli_commands.cpp functions

// Wrappers that delegate to the detailed implementations in cli_commands.cpp
static void cmd_idn(const char* arg)         { CliCmd::idn(arg); }
static void cmd_syst_stat(const char* arg)    { CliCmd::systStat(arg); }
static void cmd_syst_uptime(const char* arg)  { CliCmd::systUptime(arg); }
static void cmd_syst_reboot(const char* arg)  { CliCmd::systReboot(arg); }
static void cmd_syst_bootsel(const char* arg) { CliCmd::systBootsel(arg); }
static void cmd_syst_loc(const char* arg)     { CliCmd::systLoc(arg); }
static void cmd_outp_sw(const char* arg)      { CliCmd::outpSw(arg); }
static void cmd_outp_buck(const char* arg)    { CliCmd::outpBuck(arg); }
static void cmd_meas_volt(const char* arg)    { CliCmd::measVolt(arg); }
static void cmd_meas_curr(const char* arg)    { CliCmd::measCurr(arg); }
static void cmd_meas_pow(const char* arg)     { CliCmd::measPow(arg); }
static void cmd_meas_temp(const char* arg)    { CliCmd::measTemp(arg); }
static void cmd_meas_itemp(const char* arg)   { CliCmd::measItemp(arg); }
static void cmd_meas_energy(const char* arg)  { CliCmd::measEnergy(arg); }
static void cmd_meas_vbus(const char* arg)    { CliCmd::measVbus(arg); }
static void cmd_meas_all(const char* arg)     { CliCmd::measAll(arg); }
static void cmd_pd_list(const char* arg)      { CliCmd::pdList(arg); }
static void cmd_pd_active(const char* arg)    { CliCmd::pdActive(arg); }
static void cmd_pd_rev(const char* arg)       { CliCmd::pdRev(arg); }
static void cmd_pd_sel(const char* arg)       { CliCmd::pdSel(arg); }
static void cmd_pd_pps(const char* arg)       { CliCmd::pdPps(arg); }
static void cmd_pd_avs(const char* arg)       { CliCmd::pdAvs(arg); }
static void cmd_curr_lim(const char* arg)     { CliCmd::currLim(arg); }
static void cmd_sett_bright(const char* arg)  { CliCmd::settBright(arg); }
static void cmd_sett_sound(const char* arg)   { CliCmd::settSound(arg); }
static void cmd_sett_autopps(const char* arg) { CliCmd::settAutoPps(arg); }
static void cmd_sett_autoavs(const char* arg) { CliCmd::settAutoAvs(arg); }
static void cmd_sett_autoout(const char* arg) { CliCmd::settAutoOut(arg); }
static void cmd_sett_dim(const char* arg)     { CliCmd::settDim(arg); }
static void cmd_sett_save(const char* arg)    { CliCmd::settSave(arg); }
static void cmd_sett_reset(const char* arg)   { CliCmd::settReset(arg); }
static void cmd_tps_mode(const char* arg)     { CliCmd::tpsMode(arg); }
static void cmd_tps_garbage(const char* arg)  { CliCmd::tpsGarbage(arg); }
static void cmd_log_on(const char* arg)       { g_cli_log_enabled = true; respond("OK"); }
static void cmd_log_off(const char* arg)      { g_cli_log_enabled = false; respond("OK"); }

}  // namespace Cli
