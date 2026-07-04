#include "cli_commands.h"
#include "cli.h"
#include <cstdio>
#include <cstring>
#include <strings.h>  // strcasecmp (case-insensitive ON/OFF parsing)
#include <cstdlib>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"
#include "hardware.h"
#include "config/version.h"
#include "config/app_config.h"
#include "logic/cc_controller.h"
#include "logic/state_machine.h"
#include "logic/settings.h"
#include "logic/safety.h"
#include "logic/pd_manager.h"
#include "utils/tps_eeprom_loader.h"

// ============================================================================
// CLI Command Implementations
// ============================================================================

namespace CliCmd {

// Helper: parse ON/OFF argument, returns 1=ON, 0=OFF, -1=invalid.
// Case-insensitive: the CLI no longer force-uppercases arguments, so accept any
// casing (on/On/ON) here.
static int parseOnOff(const char* arg) {
    if (!arg) return -1;
    if (strcasecmp(arg, "ON") == 0 || strcmp(arg, "1") == 0) return 1;
    if (strcasecmp(arg, "OFF") == 0 || strcmp(arg, "0") == 0) return 0;
    return -1;
}

// Helper: scale a measurement to unsigned milli-units, clamping negatives to 0.
// Measured current/power can read slightly negative from noise near zero, and a
// negative-float -> unsigned conversion is undefined behavior. Mirrors the clamp
// the display code applies.
static unsigned toMilliUnsigned(float value) {
    if (value < 0.0f) value = 0.0f;
    return static_cast<unsigned>(value * 1000.0f);
}

// -------------------------------------------------------------------------
// Identity & System
// -------------------------------------------------------------------------

void idn(const char* /*arg*/) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s,HW%s,FW%s",
             Version::PRODUCT_NAME, Version::HARDWARE_VERSION, Version::FIRMWARE_VERSION);
    Cli::respond(buf);
}

void systStat(const char* /*arg*/) {
    AppState state = stateMachine.getState();
    const char* state_str;
    switch (state) {
        case AppState::BOOT:   state_str = "BOOT"; break;
        case AppState::MAIN:   state_str = "MAIN"; break;
        case AppState::MENU:   state_str = "MENU"; break;
        case AppState::ADJUST: state_str = "ADJUST"; break;
        case AppState::FAULT: {
            FaultType ft = stateMachine.getFaultType();
            switch (ft) {
                case FaultType::OVERCURRENT:    state_str = "FAULT:OVERCURRENT"; break;
                case FaultType::OVERTEMPERATURE: state_str = "FAULT:OVERTEMP"; break;
                case FaultType::PD_DISCONNECT:   state_str = "FAULT:PD_DISCONNECT"; break;
                default:                         state_str = "FAULT"; break;
            }
            break;
        }
        default: state_str = "UNKNOWN"; break;
    }
    Cli::respond(state_str);
}

void systUptime(const char* /*arg*/) {
    uint32_t uptime_s = to_ms_since_boot(get_absolute_time()) / 1000;
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu", uptime_s);
    Cli::respond(buf);
}

void systReboot(const char* /*arg*/) {
    Cli::respond("OK");
    sleep_ms(10);  // Allow response to transmit
    watchdog_reboot(0, 0, 0);
}

void systBootsel(const char* /*arg*/) {
    Cli::respond("OK");
    sleep_ms(10);  // Allow response to transmit
    reset_usb_boot(0, 0);
}

void systLoc(const char* /*arg*/) {
    Cli::exitRemoteMode();
    Cli::respond("OK");
}

// -------------------------------------------------------------------------
// Output Control
// -------------------------------------------------------------------------

void outpSw(const char* arg) {
    if (!arg) {
        // Query
        Cli::respond(hw.loadSwitch.read() ? "ON" : "OFF");
        return;
    }
    int val = parseOnOff(arg);
    if (val < 0) { Cli::error("INVALID_PARAM"); return; }

    // Route through the shared policy (fault gate, INA latch clear, tuning recheck)
    // so CLI and front-panel buttons behave identically.
    if (stateMachine.setLoadSwitch(val != 0) == OutputResult::FAULT_ACTIVE) {
        Cli::error("FAULT_ACTIVE");
        return;
    }
    Cli::respond("OK");
}

void outpBuck(const char* arg) {
    if (!arg) {
        // Query
        Cli::respond(hw.en17v.read() ? "ON" : "OFF");
        return;
    }
    int val = parseOnOff(arg);
    if (val < 0) { Cli::error("INVALID_PARAM"); return; }

    // Shared policy enforces the VBUS >= 18V interlock (pre-switch ADC VBUS).
    if (stateMachine.set17vBuck(val != 0) == OutputResult::NOT_AVAILABLE) {
        Cli::error("NOT_AVAILABLE");
        return;
    }
    Cli::respond("OK");
}

// -------------------------------------------------------------------------
// Measurements
// -------------------------------------------------------------------------

void measVolt(const char* /*arg*/) {
    const SafetyState& s = safety.getState();
    char buf[16];
    snprintf(buf, sizeof(buf), "%u", toMilliUnsigned(s.ina_voltage_v));
    Cli::respond(buf);
}

void measCurr(const char* /*arg*/) {
    const SafetyState& s = safety.getState();
    char buf[16];
    snprintf(buf, sizeof(buf), "%u", toMilliUnsigned(s.current_a));
    Cli::respond(buf);
}

void measPow(const char* /*arg*/) {
    const SafetyState& s = safety.getState();
    char buf[16];
    snprintf(buf, sizeof(buf), "%u", toMilliUnsigned(s.power_w));
    Cli::respond(buf);
}

void measTemp(const char* /*arg*/) {
    const SafetyState& s = safety.getState();
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", (int)(s.temperature_c * 10.0f));
    Cli::respond(buf);
}

void measItemp(const char* /*arg*/) {
    const SafetyState& s = safety.getState();
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", (int)(s.ina_temperature_c * 10.0f));
    Cli::respond(buf);
}

void measEnergy(const char* /*arg*/) {
    double charge_c = hw.powerMonitor.getCharge();
    double mah = charge_c * 1000.0 / 3.6;
    if (mah < 0.0) mah = 0.0;
    char buf[16];
    snprintf(buf, sizeof(buf), "%u", (unsigned)mah);
    Cli::respond(buf);
}

void measVbus(const char* /*arg*/) {
    float vbus_v = hw.adc.getVBUS();
    char buf[16];
    snprintf(buf, sizeof(buf), "%u", toMilliUnsigned(vbus_v));
    Cli::respond(buf);
}

void measAll(const char* /*arg*/) {
    const SafetyState& s = safety.getState();
    double charge_c = hw.powerMonitor.getCharge();
    double mah = charge_c * 1000.0 / 3.6;
    if (mah < 0.0) mah = 0.0;

    char buf[80];
    snprintf(buf, sizeof(buf), "%u,%u,%u,%d,%d,%u",
             toMilliUnsigned(s.ina_voltage_v),
             toMilliUnsigned(s.current_a),
             toMilliUnsigned(s.power_w),
             (int)(s.temperature_c * 10.0f),
             (int)(s.ina_temperature_c * 10.0f),
             (unsigned)mah);
    Cli::respond(buf);
}

// -------------------------------------------------------------------------
// PD Contract Management
// -------------------------------------------------------------------------

void pdList(const char* /*arg*/) {
    TPS26750_SourceCapability caps[AppConfig::MAX_PDO_COUNT];
    uint8_t count = pdManager.getSourceCapabilities(caps, AppConfig::MAX_PDO_COUNT);

    if (count == 0) {
        Cli::respond("NONE");
        return;
    }

    // Format: 0:5000/3000,1:9000/3000,2:PPS/3300-21000/5000,3:AVS/15000-48000/5000
    char buf[256];
    int pos = 0;

    for (uint8_t i = 0; i < count && pos < (int)sizeof(buf) - 40; i++) {
        if (i > 0) buf[pos++] = ',';

        if (caps[i].is_pps) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%u:PPS/%lu-%lu/%lu",
                           i, caps[i].min_voltage_mv, caps[i].voltage_mv, caps[i].max_current_ma);
        } else if (caps[i].is_avs) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%u:AVS/%lu-%lu/%lu",
                           i, caps[i].min_voltage_mv, caps[i].voltage_mv, caps[i].max_current_ma);
        } else {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%u:%lu/%lu",
                           i, caps[i].voltage_mv, caps[i].max_current_ma);
        }
    }
    buf[pos] = '\0';
    Cli::respond(buf);
}

void pdActive(const char* /*arg*/) {
    const ActiveContract& c = pdManager.getActiveContract();
    if (!c.valid) {
        Cli::respond("NONE");
        return;
    }

    const char* type = "FIXED";
    if (c.is_pps) type = "PPS";
    else if (c.is_avs) type = "AVS";

    char buf[48];
    snprintf(buf, sizeof(buf), "%lu,%lu,%s", c.voltage_mv, c.current_ma, type);
    Cli::respond(buf);
}

void pdRev(const char* /*arg*/) {
    const char* rev = pdManager.getPdRevision();
    Cli::respond(rev && rev[0] ? rev : "NONE");
}

void pdSel(const char* arg) {
    if (!arg || !*arg) { Cli::error("INVALID_PARAM"); return; }

    char* end;
    long index = strtol(arg, &end, 10);
    if (*end != '\0') { Cli::error("INVALID_PARAM"); return; }

    TPS26750_SourceCapability caps[AppConfig::MAX_PDO_COUNT];
    uint8_t count = pdManager.getSourceCapabilities(caps, AppConfig::MAX_PDO_COUNT);

    if (index < 0 || index >= count) {
        Cli::error("INVALID_INDEX");
        return;
    }

    bool ok;
    if (caps[index].is_pps) {
        ok = pdManager.requestPpsVoltage(caps[index].voltage_mv, caps[index].max_current_ma,
                                         (int8_t)index);
    } else if (caps[index].is_avs) {
        ok = pdManager.requestAvsVoltage(caps[index].voltage_mv, caps[index].max_current_ma,
                                         (int8_t)index);
    } else {
        ok = pdManager.requestFixedVoltage(caps[index].voltage_mv, caps[index].max_current_ma);
    }

    if (ok) {
        settings.setLastPdoIndex(static_cast<int8_t>(index));
        settings.setLastContractType(caps[index].is_avs ? SavedStartupContractType::AVS :
                                     caps[index].is_pps ? SavedStartupContractType::PPS :
                                     SavedStartupContractType::FIXED);
        settings.setLastRequestedVoltageMv(caps[index].voltage_mv);
        settings.setLastContractRange((caps[index].is_pps || caps[index].is_avs) ? caps[index].min_voltage_mv : caps[index].voltage_mv,
                                      caps[index].voltage_mv);
        settings.requestSave();
        Cli::respond("OK");
    } else {
        Cli::error("NOT_AVAILABLE");
    }
}

void pdPps(const char* arg) {
    if (!arg || !*arg) { Cli::error("INVALID_PARAM"); return; }

    if (!pdManager.isPpsActive()) {
        Cli::error("NOT_AVAILABLE");
        return;
    }

    char* end;
    long voltage_mv = strtol(arg, &end, 10);
    if (*end != '\0') { Cli::error("INVALID_PARAM"); return; }

    const ActiveContract& c = pdManager.getActiveContract();
    if (voltage_mv < (long)c.programmable_min_mv || voltage_mv > (long)c.programmable_max_mv) {
        Cli::error("OUT_OF_RANGE");
        return;
    }

    if (pdManager.requestPpsVoltage(voltage_mv, c.current_ma, pdManager.getActivePdoIndex())) {
        settings.setLastPdoIndex(pdManager.getActivePdoIndex());
        settings.setLastContractType(SavedStartupContractType::PPS);
        settings.setLastRequestedVoltageMv(static_cast<uint32_t>(voltage_mv));
        settings.setLastContractRange(pdManager.getPpsRangeMinMv(), pdManager.getPpsRangeMaxMv());
        settings.requestSave();
        Cli::respond("OK");
    } else {
        Cli::error("NOT_AVAILABLE");
    }
}

void pdAvs(const char* arg) {
    if (!arg || !*arg) { Cli::error("INVALID_PARAM"); return; }

    if (!pdManager.isAvsActive()) {
        Cli::error("NOT_AVAILABLE");
        return;
    }

    char* end;
    long voltage_mv = strtol(arg, &end, 10);
    if (*end != '\0') { Cli::error("INVALID_PARAM"); return; }

    const ActiveContract& c = pdManager.getActiveContract();
    if (voltage_mv < (long)c.programmable_min_mv || voltage_mv > (long)c.programmable_max_mv) {
        Cli::error("OUT_OF_RANGE");
        return;
    }

    if (pdManager.requestAvsVoltage(voltage_mv, c.current_ma, pdManager.getActivePdoIndex())) {
        settings.setLastPdoIndex(pdManager.getActivePdoIndex());
        settings.setLastContractType(SavedStartupContractType::AVS);
        settings.setLastRequestedVoltageMv(static_cast<uint32_t>(voltage_mv));
        settings.setLastContractRange(pdManager.getAvsRangeMinMv(), pdManager.getAvsRangeMaxMv());
        settings.requestSave();
        Cli::respond("OK");
    } else {
        Cli::error("NOT_AVAILABLE");
    }
}

// -------------------------------------------------------------------------
// Current Limit
// -------------------------------------------------------------------------

void currLim(const char* arg) {
    if (!arg) {
        // Query
        char buf[16];
        snprintf(buf, sizeof(buf), "%lu", settings.getCurrentLimit());
        Cli::respond(buf);
        return;
    }

    char* end;
    long val = strtol(arg, &end, 10);
    if (*end != '\0') { Cli::error("INVALID_PARAM"); return; }

    if (val < (long)AppConfig::CURRENT_LIMIT_MIN_MA || val > (long)AppConfig::CURRENT_LIMIT_MAX_MA) {
        Cli::error("OUT_OF_RANGE");
        return;
    }

    // Round to step size
    uint32_t limit = ((uint32_t)val / AppConfig::CURRENT_LIMIT_STEP_MA)
                     * AppConfig::CURRENT_LIMIT_STEP_MA;
    if (limit < AppConfig::CURRENT_LIMIT_MIN_MA)
        limit = AppConfig::CURRENT_LIMIT_MIN_MA;

    stateMachine.setCurrentLimitMa(limit);
    settings.setCurrentLimit(limit);
    CcController::setTargetCurrentMa(limit);
    settings.requestSave();
    Cli::respond("OK");
}

// -------------------------------------------------------------------------
// TPS26750 diagnostics / config test
// -------------------------------------------------------------------------

void tpsMode(const char* /*arg*/) {
    char mode[5] = {0};
    if (hw.pdController.getMode(mode)) {
        Cli::respond(mode);  // "APP ", "PTCH", "BOOT", ...
    } else {
        Cli::error("READ_FAILED");
    }
}

void tpsGarbage(const char* /*arg*/) {
    // Blank the EEPROM so the TPS rejects it and boots into PTCH on next power
    // cycle, letting the RP2040 boot-time patch push take over.
    //
    // eepromEraseAll() is blocking (tens of ms) and stalls safety/PD/CC
    // monitoring for its full duration. Disable both outputs first so nothing is
    // powered through the load switch / 17V buck while monitoring is frozen.
    hw.loadSwitch.off();
    hw.en17v.off();

    if (eepromEraseAll()) {
        Cli::respond("OK");
    } else {
        Cli::error("ERASE_FAILED");
    }
}

// -------------------------------------------------------------------------
// Settings
// -------------------------------------------------------------------------

void settBright(const char* arg) {
    if (!arg) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", settings.getLcdBrightness());
        Cli::respond(buf);
        return;
    }

    char* end;
    long val = strtol(arg, &end, 10);
    if (*end != '\0') { Cli::error("INVALID_PARAM"); return; }

    if (val < AppConfig::LCD_BRIGHTNESS_MIN || val > AppConfig::LCD_BRIGHTNESS_MAX) {
        Cli::error("OUT_OF_RANGE");
        return;
    }

    settings.setLcdBrightness(static_cast<uint8_t>(val));
    hw.display.setBacklightBrightness(static_cast<uint8_t>(val));
    settings.requestSave();
    Cli::respond("OK");
}

void settSound(const char* arg) {
    if (!arg) {
        Cli::respond(settings.isSoundsEnabled() ? "ON" : "OFF");
        return;
    }
    int val = parseOnOff(arg);
    if (val < 0) { Cli::error("INVALID_PARAM"); return; }
    settings.setSoundsEnabled(val);
    settings.requestSave();
    Cli::respond("OK");
}

void settAutoPps(const char* arg) {
    if (!arg) {
        Cli::respond(settings.isAutoPpsEnabled() ? "ON" : "OFF");
        return;
    }
    int val = parseOnOff(arg);
    if (val < 0) { Cli::error("INVALID_PARAM"); return; }
    settings.setAutoPpsEnabled(val);
    settings.requestSave();
    Cli::respond("OK");
}

void settAutoAvs(const char* arg) {
    if (!arg) {
        Cli::respond(settings.isAutoAvsEnabled() ? "ON" : "OFF");
        return;
    }
    int val = parseOnOff(arg);
    if (val < 0) { Cli::error("INVALID_PARAM"); return; }
    settings.setAutoAvsEnabled(val);
    settings.requestSave();
    Cli::respond("OK");
}

void settAutoOut(const char* arg) {
    if (!arg) {
        Cli::respond(settings.isAutoOutput() ? "ON" : "OFF");
        return;
    }
    int val = parseOnOff(arg);
    if (val < 0) { Cli::error("INVALID_PARAM"); return; }
    settings.setAutoOutput(val);
    settings.requestSave();
    Cli::respond("OK");
}

void settDim(const char* arg) {
    if (!arg) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", settings.getAutoDimMinutes());
        Cli::respond(buf);
        return;
    }

    char* end;
    long val = strtol(arg, &end, 10);
    if (*end != '\0') { Cli::error("INVALID_PARAM"); return; }

    if (val < AppConfig::AUTO_DIM_MIN_MINUTES || val > AppConfig::AUTO_DIM_MAX_MINUTES) {
        Cli::error("OUT_OF_RANGE");
        return;
    }

    settings.setAutoDimMinutes(static_cast<uint8_t>(val));
    settings.requestSave();
    Cli::respond("OK");
}

void settSave(const char* /*arg*/) {
    settings.saveToFlash();
    Cli::respond("OK");
}

void settReset(const char* /*arg*/) {
    settings.resetToDefaults();
    settings.saveToFlash();
    Cli::respond("OK");
}

}  // namespace CliCmd
