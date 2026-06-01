#include "state_machine.h"
#include "hardware.h"
#include "interrupts.h"
#include "config/app_config.h"
#include "utils/logging.h"
#include "utils/pd_voltage.h"
#include "drivers/buzzer/buzzer.h"
#include "pd_manager.h"
#include "settings.h"
#include "tps_eeprom_workflow.h"
#include "cc_controller.h"
#include "ui/display_manager.h"
#include "cli/cli.h"

namespace {

const char* appStateName(AppState state) {
    switch (state) {
        case AppState::BOOT: return "BOOT";
        case AppState::MAIN: return "MAIN";
        case AppState::MENU: return "MENU";
        case AppState::ADJUST: return "ADJUST";
        case AppState::FAULT: return "FAULT";
    }

    return "UNKNOWN";
}

uint32_t getInitialProgrammableTargetMv(bool resume_current_target,
                                        uint32_t user_target_mv,
                                        uint32_t live_voltage_mv,
                                        uint32_t min_voltage_mv,
                                        uint32_t max_voltage_mv,
                                        uint32_t step_mv) {
    uint32_t target_mv = (min_voltage_mv + max_voltage_mv) / 2;

    if (resume_current_target) {
        target_mv = (user_target_mv > 0) ? user_target_mv : live_voltage_mv;
    }

    target_mv = PdVoltage::alignDown(target_mv, step_mv);
    if (target_mv < min_voltage_mv) {
        target_mv = min_voltage_mv;
    }
    if (target_mv > max_voltage_mv) {
        target_mv = max_voltage_mv;
    }

    return target_mv;
}

CurrentLimitMode nextCurrentLimitMode(CurrentLimitMode mode) {
    switch (mode) {
        case CurrentLimitMode::OCP: return CurrentLimitMode::CC;
        case CurrentLimitMode::CC: return CurrentLimitMode::OFF;
        case CurrentLimitMode::OFF: return CurrentLimitMode::OCP;
    }

    return CurrentLimitMode::OCP;
}

const char* currentLimitModeName(CurrentLimitMode mode) {
    switch (mode) {
        case CurrentLimitMode::OFF: return "OFF";
        case CurrentLimitMode::OCP: return "OCP";
        case CurrentLimitMode::CC: return "CC";
    }

    return "OCP";
}

}  // namespace

// Global instance
StateMachine stateMachine;

// ============================================================================
// Boot Stage Configuration
// ============================================================================

static const char* BOOT_MESSAGES[] = {
    "",                      // 0: Logo only (melody plays)
    "Reading USB-PD...",     // 1: Discovering PDOs with timeout
    "Negotiating...",        // 2: Requesting startup contract
    "Ready!"                 // 3: Complete
};
static constexpr uint8_t BOOT_STAGE_COUNT = 4;

// Storage for PDO list (shared with display)
static SourceCapability s_pdo_list[AppConfig::MAX_PDO_COUNT];

// ============================================================================
// Constructor
// ============================================================================

StateMachine::StateMachine()
    : _state(AppState::BOOT)
    , _previous_state(AppState::BOOT)
    , _state_enter_time(nil_time)
    , _last_activity_time(nil_time)
    , _encoder_press_start(nil_time)
    , _encoder_button_held(false)
    , _boot_stage(0)
    , _boot_pdos_found(false)
    , _boot_contract_requested(false)
    , _boot_contract_complete(false)
    , _boot_retry_after_epr(false)
    , _boot_epr_probed(false)
    , _boot_epr_probe_time(nil_time)
    , _boot_ready_time(nil_time)
    , _selected_menu_item(MenuItem::SELECT_VOLTAGE)
    , _selected_settings_item(SettingsItem::FLASH_EEPROM)
    , _selected_pdo_index(0)
    , _num_pdos(0)
    , _adjust_mode(AdjustMode::NONE)
    , _current_limit_ma(AppConfig::CURRENT_LIMIT_DEFAULT_MA)
    , _adjust_original_value(0)
    , _fault_type(FaultType::NONE)
    , _fault_measured_value(0.0f)
    , _fault_limit_value(0.0f)
    , _last_encoder_ticks(0)
    , _encoder_delta(0)
    , _pps_target_voltage_mv(0)
    , _pps_min_voltage_mv(0)
    , _pps_max_voltage_mv(0)
    , _pps_max_current_ma(0)
    , _pps_pdo_index(0)
    , _avs_target_voltage_mv(0)
    , _avs_min_voltage_mv(0)
    , _avs_max_voltage_mv(0)
    , _avs_max_current_ma(0)
    , _avs_pdo_index(0)
    , _brightness_value(100)
    , _brightness_adjusting(false)
    , _screen_dimmed(false)
    , _dim_timeout_value(1)
    , _dim_timeout_adjusting(false)
    , _melody_value(1)
    , _melody_adjusting(false)
    , _contract_mode_value(2)
    , _contract_mode_adjusting(false)
    , _energy_display_mwh(false)
    , _boot_neg_start(nil_time)
{}

// ============================================================================
// Initialization
// ============================================================================

void StateMachine::init() {
    _state_enter_time = get_absolute_time();
    _last_activity_time = get_absolute_time();
    _last_encoder_ticks = hw.encoder.getTicks();

    // Restore saved current limit from settings
    uint32_t saved_limit = settings.getCurrentLimit();
    if (saved_limit >= AppConfig::CURRENT_LIMIT_MIN_MA && saved_limit <= AppConfig::CURRENT_LIMIT_MAX_MA) {
        setCurrentLimitMa(saved_limit);
    }

    // Restore energy display mode from settings
    _energy_display_mwh = (settings.getEnergyDisplayMode() != 0);

    // Play startup melody at boot (only if sounds enabled and melody != Silent)
    if (settings.isSoundsEnabled()) {
        uint8_t melody_idx = settings.getStartupMelody();
        const Note* melody = getStartupMelody(melody_idx);
        uint8_t length = getStartupMelodyLength(melody_idx);
        if (melody && length > 0) {
            hw.buzzer.playMelody(melody, length);
        }
    }

    LOG_INFO("State machine initialized, starting BOOT sequence");
}

// ============================================================================
// Main Update Loop
// ============================================================================

bool StateMachine::update() {
    bool needs_refresh = false;

    // Handle output buttons (BTN1, BTN2) in all states except BOOT and FAULT
    // Output must remain disabled during boot-up for safety
    // During FAULT, outputs are disabled and must not be toggled
    if (_state != AppState::BOOT && _state != AppState::FAULT) {
        handleOutputButtons();
    }

    // Check for overcurrent (handled immediately by ISR, but we need to update state)
    if (Interrupts::handleOvercurrent()) {
        setFault(FaultType::OVERCURRENT);
        needs_refresh = true;
    }

    // Read encoder event
    EncoderEvent event = readEncoderEvent();

    // Auto-dim handling: wake up on any user activity
    if (_screen_dimmed && event != EncoderEvent::NONE) {
        // User interacted - restore brightness
        _screen_dimmed = false;
        hw.display.setBacklightBrightness(_brightness_value);
        hw.rgbLed.setBrightness(AppConfig::RGB_LED_BRIGHTNESS_NORMAL);  // Restore RGB LED brightness
        _last_activity_time = get_absolute_time();
        LOG_INFO("Screen woken from dim (encoder input)");
    }

    // State-specific handling
    switch (_state) {
        case AppState::BOOT:
            handleBootState();
            needs_refresh = true;  // Boot always animates
            break;

        case AppState::MAIN:
            handleMainState(event);
            break;

        case AppState::MENU:
            handleMenuState(event);
            if (event != EncoderEvent::NONE) needs_refresh = true;
            break;

        case AppState::ADJUST:
            handleAdjustState(event);
            if (_adjust_mode == AdjustMode::EEPROM_FLASH && tpsEepromWorkflow.update()) {
                needs_refresh = true;
            }
            if (event != EncoderEvent::NONE) needs_refresh = true;
            break;

        case AppState::FAULT:
            handleFaultState(event);
            break;
    }

    // Check for menu timeout (return to MAIN after inactivity)
    if (_state == AppState::MENU || _state == AppState::ADJUST) {
        int64_t idle_ms = absolute_time_diff_us(_last_activity_time, get_absolute_time()) / 1000;
        if (idle_ms > (int64_t)AppConfig::MENU_TIMEOUT_MS) {
            LOG_INFO("Menu timeout, returning to MAIN");
            transitionTo(AppState::MAIN);
            needs_refresh = true;
        }
    }

    // Auto-dim check: dim screen after inactivity (applies in all states except BOOT)
    if (_state != AppState::BOOT) {
        int64_t idle_ms = absolute_time_diff_us(_last_activity_time, get_absolute_time()) / 1000;
        uint8_t dim_minutes = settings.getAutoDimMinutes();

        if (dim_minutes == 0) {
            if (_screen_dimmed) {
                _screen_dimmed = false;
                hw.display.setBacklightBrightness(settings.getLcdBrightness());
                hw.rgbLed.setBrightness(AppConfig::RGB_LED_BRIGHTNESS_NORMAL);
                LOG_INFO("Screen auto-dim disabled while dimmed; restoring brightness");
            }
        } else if (!_screen_dimmed) {
            uint32_t dim_timeout_ms = static_cast<uint32_t>(dim_minutes) * 60000;
            if (idle_ms > (int64_t)dim_timeout_ms) {
                _screen_dimmed = true;
                hw.display.setBacklightBrightness(AppConfig::LCD_BRIGHTNESS_DIM);
                hw.rgbLed.setBrightness(AppConfig::RGB_LED_BRIGHTNESS_DIM);  // Dim RGB LED too
                LOG_INFO("Screen auto-dimmed after %lld ms inactivity", idle_ms);
            }
        }
    }

    return needs_refresh;
}

// ============================================================================
// State Handlers
// ============================================================================

void StateMachine::handleBootState() {
    uint32_t elapsed_ms = absolute_time_diff_us(_state_enter_time, get_absolute_time()) / 1000;

    // Stage 0 -> 1: Show logo briefly, then start PDO discovery
    if (_boot_stage == 0 && elapsed_ms >= AppConfig::BOOT_MIN_DISPLAY_MS / 2) {
        _boot_stage = 1;  // Start PDO discovery
    }

    // Stage 1: Wait for PDOs with timeout
    if (_boot_stage == 1) {
        if (pdManager.waitForPdos(AppConfig::BOOT_PDO_TIMEOUT_MS)) {
            // PDO discovery finished (found PDOs or timed out)
            _boot_pdos_found = pdManager.hasPdos();
            if (_boot_pdos_found) {
                _num_pdos = pdManager.getSourceCapabilities(s_pdo_list, AppConfig::MAX_PDO_COUNT);
                LOG_INFO("Boot: Found %d PDOs", _num_pdos);
                _boot_stage = 2;  // Move to negotiation stage
            } else {
                LOG_INFO("Boot: No PDOs found (non-PD charger or timeout)");
                _boot_contract_requested = true;
                _boot_contract_complete = true;
                _boot_stage = 3;  // No PD source: skip negotiation/EPR probe and finish boot
            }
        }
    }

    // Stage 2: Negotiate startup contract if PDOs available
    if (_boot_stage == 2 && !_boot_contract_requested) {
        if (_boot_pdos_found) {
            // Initiate startup contract negotiation based on settings
            if (pdManager.negotiateStartupContract()) {
                LOG_INFO("Boot: Startup contract negotiation initiated");
            } else {
                _boot_retry_after_epr =
                    (settings.getStartupNegotiationMode() == StartupContractMode::LAST_USED &&
                     pdManager.shouldRetryStartupContractAfterEpr());
                if (_boot_retry_after_epr) {
                    LOG_INFO("Boot: Deferring startup contract until EPR PDOs arrive");
                } else {
                    LOG_INFO("Boot: No startup contract needed (keeping default)");
                }
                _boot_contract_complete = true;
            }
        } else {
            // No PDOs, nothing to negotiate
            _boot_contract_complete = true;
        }
        _boot_contract_requested = true;
    }

    // Wait for negotiation to complete (if in progress)
    if (_boot_stage == 2 && _boot_contract_requested && !_boot_contract_complete) {
        // Check negotiation state
        NegotiationState neg_state = pdManager.getNegotiationState();
        if (neg_state == NegotiationState::SUCCESS ||
            neg_state == NegotiationState::FAILED ||
            neg_state == NegotiationState::TIMEOUT ||
            neg_state == NegotiationState::IDLE) {
            _boot_contract_complete = true;
            pdManager.refreshActiveContract();
            LOG_INFO("Boot: Contract negotiation complete (state=%d)", static_cast<int>(neg_state));
            const ActiveContract& contract = pdManager.getActiveContract();
            if (contract.valid) {
                LOG_INFO("Boot: Active contract after negotiation = %umV @ %umA (%s)",
                         contract.voltage_mv,
                         contract.current_ma,
                         contract.is_avs ? "AVS" : (contract.is_pps ? "PPS" : "FIXED"));
            }
        }

        // Timeout fallback for negotiation
        if (is_nil_time(_boot_neg_start)) {
            _boot_neg_start = get_absolute_time();
        }
        uint32_t neg_elapsed = absolute_time_diff_us(_boot_neg_start, get_absolute_time()) / 1000;
        if (neg_elapsed >= AppConfig::BOOT_NEGOTIATION_TIMEOUT_MS) {
            _boot_contract_complete = true;
            pdManager.refreshActiveContract();
            LOG_WARN("Boot: Contract negotiation timeout");
            const ActiveContract& contract = pdManager.getActiveContract();
            if (contract.valid) {
                LOG_WARN("Boot: Active contract at timeout = %umV @ %umA (%s)",
                         contract.voltage_mv,
                         contract.current_ma,
                         contract.is_avs ? "AVS" : (contract.is_pps ? "PPS" : "FIXED"));
            }
        }
    }

    // Stage 2 -> 3: After negotiation, probe EPR and refresh PDOs before Ready
    if (_boot_stage == 2 && _boot_contract_complete) {
        if (!_boot_epr_probed) {
            // Send EPR probe (like entering voltage menu) to discover AVS/EPR PDOs
            pdManager.probeEpr();
            _boot_epr_probe_time = get_absolute_time();
            _boot_epr_probed = true;
        }

        uint32_t epr_elapsed = absolute_time_diff_us(_boot_epr_probe_time, get_absolute_time()) / 1000;
        bool is_highest_mode = (settings.getStartupNegotiationMode() == StartupContractMode::HIGHEST_VOLTAGE);

        // Poll for EPR PDOs every ~150ms during the wait window
        static uint32_t last_epr_poll_ms = 0;
        static bool epr_pdos_found = false;
        if (epr_elapsed >= AppConfig::BOOT_EPR_POLL_INTERVAL_MS && epr_elapsed - last_epr_poll_ms >= AppConfig::BOOT_EPR_POLL_INTERVAL_MS) {
            last_epr_poll_ms = epr_elapsed;
            pdManager.invalidatePdoCache();
            _num_pdos = pdManager.getSourceCapabilities(s_pdo_list, AppConfig::MAX_PDO_COUNT);
            pdManager.refreshActiveContract();

            // Check if EPR/AVS PDOs have arrived. SPR AVS has voltage_mv == 20000 (==
            // EPR_SPR_MAX_MV) so the strict > comparison excludes it; EPR fixed PDOs
            // (28/36/48 V) and EPR AVS (max > 20 V) satisfy it.
            if (!epr_pdos_found) {
                for (uint8_t i = 0; i < _num_pdos; i++) {
                    if (s_pdo_list[i].voltage_mv > AppConfig::EPR_SPR_MAX_MV) {
                        epr_pdos_found = true;
                        LOG_INFO("Boot: EPR PDOs found after %ums: %d PDOs", epr_elapsed, _num_pdos);
                        break;
                    }
                }
            }

            if (epr_pdos_found && _boot_retry_after_epr) {
                LOG_INFO("Boot: Retrying deferred startup contract after EPR discovery");
                _boot_retry_after_epr = false;

                if (pdManager.negotiateStartupContract()) {
                    _boot_contract_complete = false;
                    _boot_neg_start = nil_time;
                    return;
                }

                LOG_WARN("Boot: Deferred startup contract retry did not start");
            }

            // EPR found but not in highest-voltage mode: proceed immediately
            // (contract was already explicitly negotiated in stage 2)
            if (epr_pdos_found && !is_highest_mode) {
                last_epr_poll_ms = 0;
                epr_pdos_found = false;
                _boot_stage = 3;
            }
        }

        // Highest voltage + EPR: wait for TPS26750 auto-negotiation to settle
        if (epr_pdos_found && is_highest_mode && _boot_stage == 2) {
            pdManager.refreshActiveContract();
            const auto& contract = pdManager.getActiveContract();
            if (contract.valid && contract.voltage_mv > AppConfig::EPR_SPR_MAX_MV) {
                LOG_INFO("Boot: Contract settled at %umV after %ums", contract.voltage_mv, epr_elapsed);
                last_epr_poll_ms = 0;
                epr_pdos_found = false;
                _boot_stage = 3;
            }
        }

        // Timeouts: non-EPR chargers get shorter timeout, EPR needs more time for contract settlement
        uint32_t timeout_ms = epr_pdos_found ? AppConfig::BOOT_EPR_CONTRACT_TIMEOUT_MS : AppConfig::BOOT_EPR_TIMEOUT_MS;
        if (_boot_stage == 2 && epr_elapsed >= timeout_ms) {
            if (_boot_retry_after_epr) {
                LOG_INFO("Boot: EPR probe finished without a direct match, retrying startup restore with fallback search");
                _boot_retry_after_epr = false;
                if (pdManager.negotiateStartupContract(false)) {
                    _boot_contract_complete = false;
                    _boot_neg_start = nil_time;
                    last_epr_poll_ms = 0;
                    epr_pdos_found = false;
                    return;
                }
            }

            pdManager.invalidatePdoCache();
            _num_pdos = pdManager.getSourceCapabilities(s_pdo_list, AppConfig::MAX_PDO_COUNT);
            pdManager.refreshActiveContract();
            LOG_INFO("Boot: EPR probe timeout, proceeding with %d PDOs", _num_pdos);
            last_epr_poll_ms = 0;
            epr_pdos_found = false;
            _boot_stage = 3;
        }
    }

    // Stage 3: Show "Ready!" briefly, then transition to MAIN
    if (_boot_stage >= 3) {
        if (!pdManager.isRequestedContractSatisfied()) {
            _boot_ready_time = nil_time;
        } else {
        if (is_nil_time(_boot_ready_time)) {
            _boot_ready_time = get_absolute_time();
        }
        uint32_t ready_elapsed = absolute_time_diff_us(_boot_ready_time, get_absolute_time()) / 1000;
        if (ready_elapsed >= AppConfig::BOOT_READY_DELAY_MS) {
            transitionTo(AppState::MAIN);
            return;
        }
        }
    }

    // Ultimate fallback: prevent infinite boot
    if (elapsed_ms >= AppConfig::BOOT_DURATION_MS) {
        if (pdManager.hasPendingRequestedContract()) {
            if (elapsed_ms < AppConfig::BOOT_PENDING_CONTRACT_TIMEOUT_MS) {
                return;
            }
            LOG_WARN("Boot: Extended startup-contract timeout reached; transitioning to MAIN with output held off");
        } else {
            LOG_WARN("Boot: Fallback timeout reached, transitioning to MAIN");
        }
        transitionTo(AppState::MAIN);
    }
}

void StateMachine::handleMainState(EncoderEvent event) {
    // Click enters menu
    if (event == EncoderEvent::CLICK) {
        transitionTo(AppState::MENU);
    }
    // Long press: quick voltage adjust when in PPS or AVS mode
    else if (event == EncoderEvent::LONG_PRESS) {
        const ActiveContract& contract = pdManager.getActiveContract();
        if (contract.valid && contract.is_pps && pdManager.isPpsActive()) {
            // Use the authoritative active PDO index when available (avoids first-match
            // ambiguity when overlapping PPS APDOs exist, e.g. 3.3-11V and 3.3-16V).
            SourceCapability caps[AppConfig::MAX_PDO_COUNT];
            uint8_t count = pdManager.getSourceCapabilities(caps, AppConfig::MAX_PDO_COUNT);
            int8_t active_idx = pdManager.getActivePdoIndex();
            uint8_t selected = 0;
            bool found = false;
            if (active_idx >= 0 && active_idx < count && caps[active_idx].is_pps) {
                selected = (uint8_t)active_idx;
                found = true;
            } else {
                for (uint8_t i = 0; i < count; i++) {
                    if (caps[i].is_pps &&
                        contract.voltage_mv >= caps[i].min_voltage_mv &&
                        contract.voltage_mv <= caps[i].voltage_mv) {
                        selected = i;
                        found = true;
                        break;
                    }
                }
            }
            if (found) {
                _pps_pdo_index = selected;
                _pps_min_voltage_mv = caps[selected].min_voltage_mv;
                _pps_max_voltage_mv = caps[selected].voltage_mv;
                _pps_max_current_ma = caps[selected].max_current_ma;
                _pps_target_voltage_mv = getInitialProgrammableTargetMv(
                    true,
                    pdManager.getPpsUserTargetMv(),
                    contract.voltage_mv,
                    _pps_min_voltage_mv,
                    _pps_max_voltage_mv,
                    AppConfig::PPS_VOLTAGE_STEP_MV);
                _adjust_mode = AdjustMode::PPS_VOLTAGE;
                transitionTo(AppState::ADJUST);
                LOG_INFO("Quick PPS voltage adjust: %u-%umV (current %umV)",
                         _pps_min_voltage_mv, _pps_max_voltage_mv, _pps_target_voltage_mv);
            }
        } else if (contract.valid && contract.is_avs && pdManager.isAvsActive()) {
            // Find AVS PDO that covers current voltage and set up adjustment
            SourceCapability caps[AppConfig::MAX_PDO_COUNT];
            uint8_t count = pdManager.getSourceCapabilities(caps, AppConfig::MAX_PDO_COUNT);
            int8_t active_idx = pdManager.getActivePdoIndex();
            uint8_t selected = 0;
            bool found = false;
            if (active_idx >= 0 && active_idx < count && caps[active_idx].is_avs) {
                selected = (uint8_t)active_idx;
                found = true;
            } else {
                for (uint8_t i = 0; i < count; i++) {
                    if (caps[i].is_avs &&
                        contract.voltage_mv >= caps[i].min_voltage_mv &&
                        contract.voltage_mv <= caps[i].voltage_mv) {
                        selected = i;
                        found = true;
                        break;
                    }
                }
            }
            if (found) {
                _avs_pdo_index = selected;
                _avs_min_voltage_mv = caps[selected].min_voltage_mv;
                _avs_max_voltage_mv = caps[selected].voltage_mv;
                _avs_max_current_ma = caps[selected].max_current_ma;
                _avs_target_voltage_mv = getInitialProgrammableTargetMv(
                    true,
                    pdManager.getAvsUserTargetMv(),
                    contract.voltage_mv,
                    _avs_min_voltage_mv,
                    _avs_max_voltage_mv,
                    AppConfig::AVS_VOLTAGE_STEP_MV);
                _adjust_mode = AdjustMode::AVS_VOLTAGE;
                transitionTo(AppState::ADJUST);
                LOG_INFO("Quick AVS voltage adjust: %u-%umV (current %umV)",
                         _avs_min_voltage_mv, _avs_max_voltage_mv, _avs_target_voltage_mv);
            }
        } else {
            // No PPS/AVS active: toggle energy display mode (mAh ↔ mWh)
            _energy_display_mwh = !_energy_display_mwh;
            settings.setEnergyDisplayMode(_energy_display_mwh ? 1 : 0);
            settings.requestSave();
            if (settings.isSoundsEnabled()) {
                hw.buzzer.playTone(AppConfig::BEEP_NAV_FREQ, AppConfig::BEEP_NAV_DURATION);
            }
            LOG_INFO("Energy display: %s", _energy_display_mwh ? "mWh" : "mAh");
        }
    }
}

void StateMachine::handleMenuState(EncoderEvent event) {
    // Helper to play navigation beep (respects sound setting)
    auto playNavBeep = [this]() {
        if (settings.isSoundsEnabled()) {
            hw.buzzer.playTone(AppConfig::BEEP_NAV_FREQ, AppConfig::BEEP_NAV_DURATION);
        }
    };

    switch (event) {
        case EncoderEvent::ROTATE_CW:
            // Move down in menu (with wrap-around)
            {
                int next = static_cast<int>(_selected_menu_item) + 1;
                if (next >= static_cast<int>(MenuItem::MENU_COUNT)) {
                    next = 0;  // Wrap to first item
                }
                _selected_menu_item = static_cast<MenuItem>(next);
                playNavBeep();
            }
            _last_activity_time = get_absolute_time();
            break;

        case EncoderEvent::ROTATE_CCW:
            // Move up in menu (with wrap-around)
            {
                int prev = static_cast<int>(_selected_menu_item) - 1;
                if (prev < 0) {
                    prev = static_cast<int>(MenuItem::MENU_COUNT) - 1;  // Wrap to last item
                }
                _selected_menu_item = static_cast<MenuItem>(prev);
                playNavBeep();
            }
            _last_activity_time = get_absolute_time();
            break;

        case EncoderEvent::CLICK:
            // Select current menu item (no select beep - navigation sounds removed)
            switch (_selected_menu_item) {
                case MenuItem::SELECT_VOLTAGE:
                    pdManager.probeEpr();
                    sleep_ms(AppConfig::EPR_PROBE_DELAY_MS); // Brief blocking wait for charger to respond with EPR caps
                    pdManager.invalidatePdoCache(); // Force an absolute reload of the _pdo_cache
                    loadPdoList();
                    _adjust_mode = AdjustMode::PDO_SELECT;
                    transitionTo(AppState::ADJUST);
                    break;

                case MenuItem::CURRENT_LIMIT:
                    _adjust_original_value = _current_limit_ma;
                    // Clamp current value to effective max (contract may have changed)
                    {
                        uint32_t max_ma = getEffectiveMaxCurrentMa();
                        if (_current_limit_ma > max_ma) {
                            _current_limit_ma = max_ma;
                        }
                    }
                    _adjust_mode = AdjustMode::CURRENT_LIMIT;
                    transitionTo(AppState::ADJUST);
                    break;

                case MenuItem::SETTINGS:
                    _selected_settings_item = SettingsItem::FLASH_EEPROM;
                    _brightness_value = settings.getLcdBrightness();
                    _brightness_adjusting = false;
                    _dim_timeout_value = settings.getAutoDimMinutes();
                    _dim_timeout_adjusting = false;
                    _melody_value = settings.getStartupMelody();
                    _melody_adjusting = false;
                    _contract_mode_value = settings.getStartupNegotiation();
                    _contract_mode_adjusting = false;
                    _adjust_mode = AdjustMode::SETTINGS_MENU;
                    transitionTo(AppState::ADJUST);
                    break;

                case MenuItem::ABOUT:
                    _adjust_mode = AdjustMode::ABOUT;
                    transitionTo(AppState::ADJUST);
                    break;

                case MenuItem::ABOUT_CHARGER:
                    pdManager.probeEpr();
                    sleep_ms(AppConfig::EPR_PROBE_DELAY_MS);
                    pdManager.invalidatePdoCache();
                    pdManager.refreshChargerIdentity();
                    _adjust_mode = AdjustMode::ABOUT_CHARGER;
                    transitionTo(AppState::ADJUST);
                    break;

                case MenuItem::BACK:
                    transitionTo(AppState::MAIN);
                    break;

                default:
                    break;
            }
            _last_activity_time = get_absolute_time();
            break;

        case EncoderEvent::LONG_PRESS:
            // Long press disabled (variable kept for future use)
            break;

        default:
            break;
    }
}

void StateMachine::handleAdjustState(EncoderEvent event) {
    // Helper to play navigation beep (respects sound setting)
    auto playNavBeep = [this]() {
        if (settings.isSoundsEnabled()) {
            hw.buzzer.playTone(AppConfig::BEEP_NAV_FREQ, AppConfig::BEEP_NAV_DURATION);
        }
    };
    
    auto playSelectBeep = [this]() {
        if (settings.isSoundsEnabled()) {
            hw.buzzer.playTone(AppConfig::BEEP_SELECT_FREQ, AppConfig::BEEP_SELECT_DURATION);
        }
    };
    
    auto playExitBeep = [this]() {
        if (settings.isSoundsEnabled()) {
            hw.buzzer.playTone(AppConfig::BEEP_EXIT_FREQ, AppConfig::BEEP_EXIT_DURATION);
        }
    };

    // About screens: click returns to menu
    if (_adjust_mode == AdjustMode::ABOUT || _adjust_mode == AdjustMode::ABOUT_CHARGER) {
        if (event != EncoderEvent::NONE) {
            _last_activity_time = get_absolute_time();
        }
        if (event == EncoderEvent::CLICK) {
            transitionTo(AppState::MENU);
        }
        return;
    }

    // Settings submenu handling
    if (_adjust_mode == AdjustMode::SETTINGS_MENU) {
        handleSettingsMenuState(event);
        return;
    }

    // EEPROM flash mode: delegate to workflow controller
    if (_adjust_mode == AdjustMode::EEPROM_FLASH) {
        bool rotate = (event == EncoderEvent::ROTATE_CW || event == EncoderEvent::ROTATE_CCW);
        bool click = (event == EncoderEvent::CLICK);
        
        if (rotate || click) {
            _last_activity_time = get_absolute_time();
        }
        
        if (tpsEepromWorkflow.handleInput(rotate, click)) {
            // Workflow complete - return to settings menu
            _adjust_mode = AdjustMode::SETTINGS_MENU;
            displayManager.invalidate();
        }
        return;
    }

    switch (event) {
        case EncoderEvent::ROTATE_CW:
            if (_adjust_mode == AdjustMode::PDO_SELECT) {
                // Clamp index to valid range in case PDO count changed
                if (_selected_pdo_index > _num_pdos) {
                    _selected_pdo_index = _num_pdos;
                }
                if (_selected_pdo_index < _num_pdos) {
                    _selected_pdo_index++;
                } else {
                    _selected_pdo_index = 0;  // Wrap to first
                }
            } else if (_adjust_mode == AdjustMode::CURRENT_LIMIT) {
                if (!CcController::isDisabled()) {
                    // Velocity-based acceleration for current limit (smaller range: 0-5A)
                    uint32_t velocity_mult = (hw.encoder.getVelocityMultiplier() + AppConfig::CURRENT_LIMIT_VELOCITY_DIV - 1)
                                             / AppConfig::CURRENT_LIMIT_VELOCITY_DIV;
                    if (velocity_mult < 1) velocity_mult = 1;
                    uint32_t step = AppConfig::CURRENT_LIMIT_STEP_MA * velocity_mult;
                    uint32_t max_ma = getEffectiveMaxCurrentMa();
                    if (_current_limit_ma + step <= max_ma) {
                        _current_limit_ma += step;
                    } else {
                        _current_limit_ma = max_ma;
                    }
                }
            } else if (_adjust_mode == AdjustMode::PPS_VOLTAGE) {
                // PPS voltage: Use velocity-based acceleration (larger range: up to 21V)
                uint32_t velocity_mult = hw.encoder.getVelocityMultiplier() * AppConfig::PPS_VELOCITY_MULT;
                uint32_t step = AppConfig::PPS_VOLTAGE_STEP_MV * velocity_mult;
                if (_pps_target_voltage_mv + step <= _pps_max_voltage_mv) {
                    _pps_target_voltage_mv += step;
                } else {
                    _pps_target_voltage_mv = _pps_max_voltage_mv;
                }
                _pps_target_voltage_mv = PdVoltage::alignDown(_pps_target_voltage_mv,
                                                              AppConfig::PPS_VOLTAGE_STEP_MV);
            } else if (_adjust_mode == AdjustMode::AVS_VOLTAGE) {
                // AVS voltage: Use velocity-based acceleration (wider range: 15-48V)
                uint32_t velocity_mult = hw.encoder.getVelocityMultiplier() * AppConfig::AVS_VELOCITY_MULT;
                uint32_t step = AppConfig::AVS_VOLTAGE_STEP_MV * velocity_mult;
                if (_avs_target_voltage_mv + step <= _avs_max_voltage_mv) {
                    _avs_target_voltage_mv += step;
                } else {
                    _avs_target_voltage_mv = _avs_max_voltage_mv;
                }
                _avs_target_voltage_mv = PdVoltage::alignDown(_avs_target_voltage_mv,
                                                              AppConfig::AVS_VOLTAGE_STEP_MV);
            }
            _last_activity_time = get_absolute_time();
            break;

        case EncoderEvent::ROTATE_CCW:
            if (_adjust_mode == AdjustMode::PDO_SELECT) {
                // Clamp index to valid range in case PDO count changed
                if (_selected_pdo_index > _num_pdos) {
                    _selected_pdo_index = _num_pdos;
                }
                if (_selected_pdo_index > 0) {
                    _selected_pdo_index--;
                } else {
                    _selected_pdo_index = _num_pdos;  // Wrap to Back item
                }
            } else if (_adjust_mode == AdjustMode::CURRENT_LIMIT) {
                if (!CcController::isDisabled()) {
                    // Velocity-based acceleration for current limit (smaller range: 0-5A)
                    uint32_t velocity_mult = (hw.encoder.getVelocityMultiplier() + AppConfig::CURRENT_LIMIT_VELOCITY_DIV - 1)
                                             / AppConfig::CURRENT_LIMIT_VELOCITY_DIV;
                    if (velocity_mult < 1) velocity_mult = 1;
                    uint32_t step = AppConfig::CURRENT_LIMIT_STEP_MA * velocity_mult;
                    if (_current_limit_ma > AppConfig::CURRENT_LIMIT_MIN_MA + step) {
                        _current_limit_ma -= step;
                    } else {
                        _current_limit_ma = AppConfig::CURRENT_LIMIT_MIN_MA;
                    }
                }
            } else if (_adjust_mode == AdjustMode::PPS_VOLTAGE) {
                // PPS voltage: Use velocity-based acceleration (larger range: up to 21V)
                uint32_t velocity_mult = hw.encoder.getVelocityMultiplier() * AppConfig::PPS_VELOCITY_MULT;
                uint32_t step = AppConfig::PPS_VOLTAGE_STEP_MV * velocity_mult;
                if (_pps_target_voltage_mv >= _pps_min_voltage_mv + step) {
                    _pps_target_voltage_mv -= step;
                } else {
                    _pps_target_voltage_mv = _pps_min_voltage_mv;
                }
                _pps_target_voltage_mv = PdVoltage::alignDown(_pps_target_voltage_mv,
                                                              AppConfig::PPS_VOLTAGE_STEP_MV);
            } else if (_adjust_mode == AdjustMode::AVS_VOLTAGE) {
                // AVS voltage: Use velocity-based acceleration (wider range: 15-48V)
                uint32_t velocity_mult = hw.encoder.getVelocityMultiplier() * AppConfig::AVS_VELOCITY_MULT;
                uint32_t step = AppConfig::AVS_VOLTAGE_STEP_MV * velocity_mult;
                if (_avs_target_voltage_mv >= _avs_min_voltage_mv + step) {
                    _avs_target_voltage_mv -= step;
                } else {
                    _avs_target_voltage_mv = _avs_min_voltage_mv;
                }
                _avs_target_voltage_mv = PdVoltage::alignDown(_avs_target_voltage_mv,
                                                              AppConfig::AVS_VOLTAGE_STEP_MV);
            }
            _last_activity_time = get_absolute_time();
            break;

        case EncoderEvent::CLICK:
            // Confirm selection
            if (_adjust_mode == AdjustMode::PDO_SELECT) {
                // If no PDOs or "Back" selected, return to menu
                if (_num_pdos == 0 || _selected_pdo_index == _num_pdos) {
                    if (settings.isSoundsEnabled()) {
                        hw.buzzer.playTone(AppConfig::BEEP_EXIT_FREQ, AppConfig::BEEP_EXIT_DURATION);  // Exit beep
                    }
                    transitionTo(AppState::MENU);
                    _last_activity_time = get_absolute_time();
                    break;
                }
                // Check if selected PDO is PPS - if so, enter voltage adjustment mode
                if (_selected_pdo_index >= 0 && _selected_pdo_index < _num_pdos) {
                    SourceCapability& pdo = s_pdo_list[_selected_pdo_index];
                    const ActiveContract& contract = pdManager.getActiveContract();
                    int8_t active_pdo_index = pdManager.getActivePdoIndex();
                    if (pdo.is_pps) {
                        // Enter PPS voltage adjustment mode
                        _pps_pdo_index = _selected_pdo_index;
                        _pps_min_voltage_mv = pdo.min_voltage_mv;
                        _pps_max_voltage_mv = pdo.voltage_mv;
                        _pps_max_current_ma = pdo.max_current_ma;
                        bool resume_active_pps = contract.valid && contract.is_pps && pdManager.isPpsActive() &&
                            ((active_pdo_index >= 0 && active_pdo_index == _selected_pdo_index) ||
                             (active_pdo_index < 0 &&
                              contract.voltage_mv >= _pps_min_voltage_mv &&
                              contract.voltage_mv <= _pps_max_voltage_mv));
                        _pps_target_voltage_mv = getInitialProgrammableTargetMv(
                            resume_active_pps,
                            pdManager.getPpsUserTargetMv(),
                            contract.voltage_mv,
                            _pps_min_voltage_mv,
                            _pps_max_voltage_mv,
                            AppConfig::PPS_VOLTAGE_STEP_MV);
                        _adjust_mode = AdjustMode::PPS_VOLTAGE;
                        LOG_INFO("Entering PPS voltage adjustment: %u-%umV", _pps_min_voltage_mv, _pps_max_voltage_mv);
                        // Force display redraw since we changed mode within same state
                        displayManager.invalidate();
                    } else if (pdo.is_avs) {
                        // Enter AVS voltage adjustment mode
                        _avs_pdo_index = _selected_pdo_index;
                        _avs_min_voltage_mv = pdo.min_voltage_mv;
                        _avs_max_voltage_mv = pdo.voltage_mv;
                        _avs_max_current_ma = pdo.max_current_ma;
                        bool resume_active_avs = contract.valid && contract.is_avs && pdManager.isAvsActive() &&
                            ((active_pdo_index >= 0 && active_pdo_index == _selected_pdo_index) ||
                             (active_pdo_index < 0 &&
                              contract.voltage_mv >= _avs_min_voltage_mv &&
                              contract.voltage_mv <= _avs_max_voltage_mv));
                        _avs_target_voltage_mv = getInitialProgrammableTargetMv(
                            resume_active_avs,
                            pdManager.getAvsUserTargetMv(),
                            contract.voltage_mv,
                            _avs_min_voltage_mv,
                            _avs_max_voltage_mv,
                            AppConfig::AVS_VOLTAGE_STEP_MV);
                        _adjust_mode = AdjustMode::AVS_VOLTAGE;
                        LOG_INFO("Entering AVS voltage adjustment: %u-%umV", _avs_min_voltage_mv, _avs_max_voltage_mv);
                        displayManager.invalidate();
                    } else {
                        // Fixed or AVS - request immediately
                        requestSelectedPdo();
                        transitionTo(AppState::MENU);
                    }
                }
            } else if (_adjust_mode == AdjustMode::CURRENT_LIMIT) {
                applyCurrentLimit();
                transitionTo(AppState::MENU);
            } else if (_adjust_mode == AdjustMode::PPS_VOLTAGE) {
                applyPpsVoltage();
                pdManager.checkTuningConvergenceImmediate();
                // Return to MAIN if quick-adjust from main screen, else MENU
                transitionTo(_previous_state == AppState::MAIN ? AppState::MAIN : AppState::MENU);
            } else if (_adjust_mode == AdjustMode::AVS_VOLTAGE) {
                applyAvsVoltage();
                pdManager.checkTuningConvergenceImmediate();
                // Return to MAIN if quick-adjust from main screen, else MENU
                transitionTo(_previous_state == AppState::MAIN ? AppState::MAIN : AppState::MENU);
            }
            _last_activity_time = get_absolute_time();
            break;

        case EncoderEvent::LONG_PRESS:
            // Long press: go back without applying (for PPS/AVS voltage adjust)
            if (_adjust_mode == AdjustMode::PPS_VOLTAGE || _adjust_mode == AdjustMode::AVS_VOLTAGE) {
                transitionTo(_previous_state == AppState::MAIN ? AppState::MAIN : AppState::MENU);
            }
            break;

        default:
            break;
    }
}

void StateMachine::handleFaultState(EncoderEvent event) {
    // Click to acknowledge fault and return to main
    if (event == EncoderEvent::CLICK) {
        LOG_INFO("Fault acknowledged");
        _fault_type = FaultType::NONE;
        transitionTo(AppState::MAIN);
    }
}

// ============================================================================
// State Transitions
// ============================================================================

void StateMachine::transitionTo(AppState new_state) {
    if (_state == new_state) return;

    LOG_INFO("State transition: %s -> %s", appStateName(_state), appStateName(new_state));
    if (_state == AppState::BOOT && new_state == AppState::MAIN) {
        LOG_SEPARATOR();
    }

    _previous_state = _state;
    _state = new_state;
    _state_enter_time = get_absolute_time();
    _last_activity_time = get_absolute_time();

    // State entry actions
    switch (new_state) {
        case AppState::BOOT:
            _boot_stage = 0;
            _boot_pdos_found = false;
            _boot_contract_requested = false;
            _boot_contract_complete = false;
            _boot_retry_after_epr = false;
            _boot_ready_time = nil_time;
            _boot_neg_start = nil_time;
            break;

        case AppState::MAIN:
            _adjust_mode = AdjustMode::NONE;
            pdManager.refreshActiveContract();  // Ensure fresh contract data for display
            hw.rgbLed.setColor(LedColor::BLUE, AppConfig::RGB_LED_BRIGHTNESS_NORMAL);
            // Drain any button presses that occurred during BOOT or FAULT
            if (_previous_state == AppState::BOOT || _previous_state == AppState::FAULT) {
                Interrupts::checkBtn1Clicked();
                Interrupts::checkBtn2Clicked();
            }
            // Auto-output on boot: enable load switch after boot completes
            if (_previous_state == AppState::BOOT && settings.isAutoOutput()) {
                if (pdManager.isRequestedContractSatisfied()) {
                    hw.powerMonitor.getDiagnoseAlert();  // Clear INA228 fault latch
                    hw.loadSwitch.on();
                    LOG_INFO("Auto-output enabled on boot");
                } else {
                    LOG_WARN("Auto-output suppressed on boot: startup contract unresolved");
                }
            }
            // Note: Startup contract negotiation is now done during BOOT state
            // No need to restore PDO here as it's handled by negotiateStartupContract()
            break;

        case AppState::MENU:
            // Only reset cursor when entering from MAIN screen
            // When returning from ADJUST, keep cursor on the previously selected item
            if (_previous_state != AppState::ADJUST) {
                _selected_menu_item = MenuItem::SELECT_VOLTAGE;
            }
            hw.rgbLed.setColor(LedColor::MAGENTA, AppConfig::RGB_LED_BRIGHTNESS_NORMAL);
            break;

        case AppState::ADJUST:
            hw.rgbLed.setColor(LedColor::YELLOW, AppConfig::RGB_LED_BRIGHTNESS_NORMAL);
            break;

        case AppState::FAULT:
            hw.rgbLed.setColor(LedColor::RED);
            hw.buzzer.playTone(AppConfig::BEEP_FAULT_FREQ, AppConfig::BEEP_FAULT_DURATION);  // Alert beep
            // Drain button ISR flags to prevent stale presses after acknowledgment
            Interrupts::checkBtn1Clicked();
            Interrupts::checkBtn2Clicked();
            break;
    }
}

// ============================================================================
// Input Processing
// ============================================================================

EncoderEvent StateMachine::readEncoderEvent() {
    EncoderEvent event = EncoderEvent::NONE;

    // In REMOTE mode, only allow long-press (to exit remote)
    // Drain all other encoder events so they don't queue up
    if (Cli::isRemoteMode()) {
        // Still read ticks to keep encoder tracking in sync
        _last_encoder_ticks = hw.encoder.getTicks();
        _encoder_delta = 0;

        // Only process button for long-press detection
        bool button_pressed = hw.btnEnc.isPressed();

        if (button_pressed && !_encoder_button_held) {
            _encoder_press_start = get_absolute_time();
            _encoder_button_held = true;
            Interrupts::checkBtnEncClicked();
        } else if (_encoder_button_held && !button_pressed) {
            uint32_t press_duration = absolute_time_diff_us(_encoder_press_start, get_absolute_time()) / 1000;
            if (press_duration >= AppConfig::ENCODER_LONG_PRESS_MS) {
                Cli::exitRemoteMode();
                LOG_INFO("Remote mode exited (encoder long press)");
            }
            _encoder_button_held = false;
            Interrupts::checkBtnEncClicked();
        } else if (!_encoder_button_held) {
            Interrupts::checkBtnEncClicked();  // Drain ISR flags
        }

        return EncoderEvent::NONE;
    }

    // Check encoder rotation
    int current_ticks = hw.encoder.getTicks();
    int delta = _last_encoder_ticks - current_ticks;  // Inverted: physical CW = positive delta
    _encoder_delta = delta;  // Store for acceleration (used by current limit adjust)

    if (delta > 0) {
        event = EncoderEvent::ROTATE_CW;
        _last_encoder_ticks = current_ticks;
    } else if (delta < 0) {
        event = EncoderEvent::ROTATE_CCW;
        _last_encoder_ticks = current_ticks;
    }

    // Encoder button handling:
    // - Use ISR flag for reliable click detection (catches short presses)
    // - Use polling for long-press timing (needs duration measurement)
    
    bool button_pressed = hw.btnEnc.isPressed();

    if (button_pressed && !_encoder_button_held) {
        // Button just pressed - start timing for long press
        _encoder_press_start = get_absolute_time();
        _encoder_button_held = true;
        // Consume any ISR flag that fired for this press
        Interrupts::checkBtnEncClicked();
    } else if (_encoder_button_held) {
        // Button is being held - check for long press threshold
        uint32_t press_duration = absolute_time_diff_us(_encoder_press_start, get_absolute_time()) / 1000;
        
        if (!button_pressed) {
            // Button released
            if (press_duration >= AppConfig::ENCODER_LONG_PRESS_MS) {
                event = EncoderEvent::LONG_PRESS;
            } else if (press_duration > AppConfig::ENCODER_MIN_PRESS_MS) {
                event = EncoderEvent::CLICK;
            }
            _encoder_button_held = false;
            // Consume any ISR flag from release bounce to prevent double-click
            Interrupts::checkBtnEncClicked();
        }
    } else {
        // Button not held - check ISR flag for any clicks we might have missed
        // (e.g., very quick press between main loop iterations)
        if (Interrupts::checkBtnEncClicked()) {
            event = EncoderEvent::CLICK;
        }
    }

    return event;
}

void StateMachine::handleOutputButtons() {
    // In REMOTE mode, drain button ISR flags but don't act on them
    if (Cli::isRemoteMode()) {
        Interrupts::checkBtn1Clicked();
        Interrupts::checkBtn2Clicked();
        return;
    }

    // Check for button presses to wake from dim
    bool btn1_clicked = Interrupts::checkBtn1Clicked();
    bool btn2_clicked = Interrupts::checkBtn2Clicked();

    // Wake from dim on any button press
    if (_screen_dimmed && (btn1_clicked || btn2_clicked)) {
        _screen_dimmed = false;
        hw.display.setBacklightBrightness(_brightness_value);
        hw.rgbLed.setBrightness(AppConfig::RGB_LED_BRIGHTNESS_NORMAL);  // Restore RGB LED brightness
        _last_activity_time = get_absolute_time();
        LOG_INFO("Screen woken from dim (button press)");
    }

    // BTN1: Toggle load switch
    if (btn1_clicked) {
        // Clear INA228 fault latch before enabling
        hw.powerMonitor.getDiagnoseAlert();

        bool current_state = hw.loadSwitch.read();
        if (current_state) {
            hw.loadSwitch.off();
            LOG_INFO("Load switch DISABLED (BTN1)");
        } else {
            hw.loadSwitch.on();
            LOG_INFO("Load switch ENABLED (BTN1)");
        }
        // Recheck tuning convergence immediately (measurement source changed)
        pdManager.checkTuningConvergenceImmediate();
    }

    // BTN2: Context-dependent action
    if (btn2_clicked) {
        if (_state == AppState::ADJUST && _adjust_mode == AdjustMode::CURRENT_LIMIT) {
            CurrentLimitMode next_mode = nextCurrentLimitMode(CcController::getMode());
            CcController::setMode(next_mode);
            if (settings.isSoundsEnabled()) {
                hw.buzzer.playTone(AppConfig::BEEP_CONFIRM_FREQ, AppConfig::BEEP_CONFIRM_DURATION);
            }
            LOG_INFO("Current limit mode toggled to %s (BTN2)", currentLimitModeName(next_mode));
        } else if (_state == AppState::MAIN) {
            // Toggle 17V buck (only if VBUS > 18V, only from MAIN screen)
            float vbus_mv = hw.powerMonitor.getBusVoltage() * 1000.0f;

            if (vbus_mv >= AppConfig::MIN_VBUS_FOR_17V_MV) {
                bool current_state = hw.EN_17V.read();
                if (current_state) {
                    hw.EN_17V.off();
                    LOG_INFO("17V buck DISABLED (BTN2)");
                } else {
                    hw.EN_17V.on();
                    LOG_INFO("17V buck ENABLED (BTN2)");
                }
            } else {
                LOG_WARN("Cannot enable 17V buck: VBUS=%.1fV < 18V", vbus_mv / 1000.0f);
                hw.buzzer.playTone(AppConfig::BEEP_ERROR_FREQ, AppConfig::BEEP_ERROR_DURATION / 2);
            }
        }
    }
}

// ============================================================================
// Fault Handling
// ============================================================================

void StateMachine::setFault(FaultType fault) {
    if (_state == AppState::FAULT && fault == _fault_type) {
        return;  // Already in this fault state
    }

    _fault_type = fault;

    // Store fault values for display
    switch (fault) {
        case FaultType::OVERCURRENT:
            _fault_measured_value = hw.powerMonitor.getCurrent();
            _fault_limit_value = _current_limit_ma / 1000.0f;
            LOG_ERROR("FAULT: Overcurrent - %.2fA (limit %.2fA)",
                     _fault_measured_value, _fault_limit_value);
            break;

        case FaultType::OVERTEMPERATURE:
            _fault_measured_value = hw.adc.getTemperature();
            _fault_limit_value = static_cast<float>(AppConfig::TEMP_SHUTDOWN_C);
            LOG_ERROR("FAULT: Overtemperature - %.1fC (limit %.1fC)",
                     _fault_measured_value, _fault_limit_value);
            break;

        case FaultType::PD_DISCONNECT:
            LOG_ERROR("FAULT: USB-PD disconnected");
            break;

        default:
            break;
    }

    // Disable load switch on any fault
    hw.loadSwitch.off();

    transitionTo(AppState::FAULT);
}

// ============================================================================
// Boot Sequence Helpers
// ============================================================================

uint8_t StateMachine::getBootProgress() const {
    if (_state != AppState::BOOT) return 100;

    // Progress based on current boot stage
    // Stage 0: 0-25%  (logo/melody)
    // Stage 1: 25-50% (PDO discovery)
    // Stage 2: 50-90% (negotiation)
    // Stage 3: 100%   (ready)
    switch (_boot_stage) {
        case 0: return 10;
        case 1: return 35;
        case 2: return 70;
        case 3: return 100;
        default: return 100;
    }
}

const char* StateMachine::getBootStageMessage() const {
    if (_boot_stage < BOOT_STAGE_COUNT) {
        return BOOT_MESSAGES[_boot_stage];
    }
    return "";
}

// ============================================================================
// PDO Management Helpers
// ============================================================================

void StateMachine::loadPdoList() {
    // Reload PDOs first so the cache is valid before refreshActiveContract() runs its
    // warm-reset detection (which needs the cache to identify PPS/AVS contracts).
    _num_pdos = pdManager.getSourceCapabilities(s_pdo_list, AppConfig::MAX_PDO_COUNT);
    pdManager.refreshActiveContract();  // Update active contract after PDO cache is valid
    _selected_pdo_index = 0;

    LOG_INFO("Loaded %d PDOs from charger", _num_pdos);
}

void StateMachine::requestSelectedPdo() {
    if (_selected_pdo_index < 0 || _selected_pdo_index >= _num_pdos) {
        LOG_ERROR("Invalid PDO index: %d", _selected_pdo_index);
        return;
    }

    SourceCapability& pdo = s_pdo_list[_selected_pdo_index];

    LOG_INFO("Requesting PDO[%d]: %umV @ %umA (PPS=%d, AVS=%d)",
             _selected_pdo_index, pdo.voltage_mv, pdo.max_current_ma,
             pdo.is_pps, pdo.is_avs);

    // Route through PD manager for proper negotiation tracking
    bool success = pdManager.requestContract(pdo);

    if (success) {
        LOG_INFO("PDO request sent successfully");
        if (settings.isSoundsEnabled()) {
            hw.buzzer.playTone(AppConfig::BEEP_CONFIRM_FREQ, AppConfig::BEEP_CONFIRM_DURATION);  // Confirmation beep
        }
        // Save selected PDO for boot restore (only in Last Used mode to reduce flash wear)
        if (settings.getStartupNegotiationMode() == StartupContractMode::LAST_USED) {
            saveStartupContractSnapshot(pdo, _selected_pdo_index, pdo.voltage_mv);
            settings.requestSave();
        }
    } else {
        LOG_ERROR("Failed to request PDO");
        hw.buzzer.playTone(AppConfig::BEEP_ERROR_FREQ, AppConfig::BEEP_ERROR_DURATION);  // Error beep
    }
}

void StateMachine::saveStartupContractSnapshot(const SourceCapability& pdo,
                                               int8_t pdo_index,
                                               uint32_t requested_voltage_mv) {
    SavedStartupContractType contract_type = SavedStartupContractType::FIXED;
    uint32_t range_min_mv = pdo.voltage_mv;
    uint32_t range_max_mv = pdo.voltage_mv;

    if (pdo.is_avs) {
        contract_type = SavedStartupContractType::AVS;
        range_min_mv = pdo.min_voltage_mv;
    } else if (pdo.is_pps) {
        contract_type = SavedStartupContractType::PPS;
        range_min_mv = pdo.min_voltage_mv;
    }

    settings.setLastPdoIndex(pdo_index);
    settings.setLastContractType(contract_type);
    settings.setLastRequestedVoltageMv(requested_voltage_mv > 0 ? requested_voltage_mv : pdo.voltage_mv);
    settings.setLastContractRange(range_min_mv, range_max_mv);
}

bool StateMachine::saveCurrentContractSnapshot() {
    const ActiveContract& contract = pdManager.getActiveContract();
    if (!contract.valid) {
        LOG_WARN("Cannot snapshot startup contract: no active PD contract");
        return false;
    }

    if (contract.is_pps) {
        settings.setLastPdoIndex(pdManager.getActivePdoIndex());
        settings.setLastContractType(SavedStartupContractType::PPS);
        settings.setLastRequestedVoltageMv(pdManager.getPpsUserTargetMv() > 0 ?
                                           pdManager.getPpsUserTargetMv() :
                                           contract.voltage_mv);
        settings.setLastContractRange(pdManager.getPpsRangeMinMv(), pdManager.getPpsRangeMaxMv());
        return true;
    }

    if (contract.is_avs) {
        settings.setLastPdoIndex(pdManager.getActivePdoIndex());
        settings.setLastContractType(SavedStartupContractType::AVS);
        settings.setLastRequestedVoltageMv(pdManager.getAvsUserTargetMv() > 0 ?
                                           pdManager.getAvsUserTargetMv() :
                                           contract.voltage_mv);
        settings.setLastContractRange(pdManager.getAvsRangeMinMv(), pdManager.getAvsRangeMaxMv());
        return true;
    }

    settings.setLastPdoIndex(-1);
    settings.setLastContractType(SavedStartupContractType::FIXED);
    settings.setLastRequestedVoltageMv(contract.voltage_mv);
    settings.setLastContractRange(contract.voltage_mv, contract.voltage_mv);
    return true;
}

// ============================================================================
// Current Limit Helpers
// ============================================================================

void StateMachine::setCurrentLimitMa(uint32_t limit_ma) {
    if (limit_ma < AppConfig::CURRENT_LIMIT_MIN_MA) {
        limit_ma = AppConfig::CURRENT_LIMIT_MIN_MA;
    }
    if (limit_ma > AppConfig::CURRENT_LIMIT_MAX_MA) {
        limit_ma = AppConfig::CURRENT_LIMIT_MAX_MA;
    }

    _current_limit_ma = limit_ma;
}

void StateMachine::applyCurrentLimit() {
    LOG_INFO("Current limit set to %u mA (%s)",
             _current_limit_ma,
             currentLimitModeName(CcController::getMode()));

    // Update current limit target and let the controller keep hardware alert state in sync.
    CcController::setTargetCurrentMa(_current_limit_ma);

    // Persist to settings
    settings.setCurrentLimit(_current_limit_ma);
    settings.requestSave();

    if (settings.isSoundsEnabled()) {
        hw.buzzer.playTone(AppConfig::BEEP_CONFIRM_FREQ, AppConfig::BEEP_CONFIRM_DURATION);
    }
}

void StateMachine::applyPpsVoltage() {
    LOG_INFO("Requesting PPS: %umV @ %umA (PDO index %d)",
             _pps_target_voltage_mv, _pps_max_current_ma, _pps_pdo_index);

    // Request PPS contract with the selected voltage and explicit PDO index so the
    // driver uses the correct APDO's bounds (prevents overlap-APDO mis-selection).
    bool success = pdManager.requestPpsVoltage(_pps_target_voltage_mv, _pps_max_current_ma,
                                               (int8_t)_pps_pdo_index);

    if (success) {
        LOG_INFO("PPS request sent successfully");
        if (settings.isSoundsEnabled()) {
            hw.buzzer.playTone(AppConfig::BEEP_CONFIRM_FREQ, AppConfig::BEEP_CONFIRM_DURATION);  // Confirmation beep
        }
        // Save PPS state for boot restore (only in Last Used mode to reduce flash wear)
        if (settings.getStartupNegotiationMode() == StartupContractMode::LAST_USED) {
            saveStartupContractSnapshot(s_pdo_list[_pps_pdo_index], _pps_pdo_index, _pps_target_voltage_mv);
            settings.requestSave();
        }
    } else {
        LOG_ERROR("Failed to request PPS");
        // Error beep always plays (safety feedback)
        hw.buzzer.playTone(AppConfig::BEEP_ERROR_FREQ, AppConfig::BEEP_ERROR_DURATION);
    }
}

void StateMachine::applyAvsVoltage() {
    LOG_INFO("Requesting AVS: %umV @ %umA (PDO index %d)",
             _avs_target_voltage_mv, _avs_max_current_ma, _avs_pdo_index);

    bool success = pdManager.requestAvsVoltage(_avs_target_voltage_mv, _avs_max_current_ma,
                                               (int8_t)_avs_pdo_index);

    if (success) {
        LOG_INFO("AVS request sent successfully");
        if (settings.isSoundsEnabled()) {
            hw.buzzer.playTone(AppConfig::BEEP_CONFIRM_FREQ, AppConfig::BEEP_CONFIRM_DURATION);
        }
        // Save AVS state for boot restore (only in Last Used mode to reduce flash wear)
        if (settings.getStartupNegotiationMode() == StartupContractMode::LAST_USED) {
            saveStartupContractSnapshot(s_pdo_list[_avs_pdo_index], _avs_pdo_index, _avs_target_voltage_mv);
            settings.requestSave();
        }
    } else {
        LOG_ERROR("Failed to request AVS");
        hw.buzzer.playTone(AppConfig::BEEP_ERROR_FREQ, AppConfig::BEEP_ERROR_DURATION);
    }
}

// ============================================================================
// Settings Menu Helpers
// ============================================================================

void StateMachine::handleSettingsMenuState(EncoderEvent event) {
    // Helper to play navigation beep (respects sound setting)
    auto playNavBeep = [this]() {
        if (settings.isSoundsEnabled()) {
            hw.buzzer.playTone(AppConfig::BEEP_NAV_FREQ, AppConfig::BEEP_NAV_DURATION);
        }
    };

    switch (event) {
        case EncoderEvent::ROTATE_CW:
            // Check if any adjustable item is in adjust mode
            if (_selected_settings_item == SettingsItem::BRIGHTNESS && _brightness_adjusting) {
                if (_brightness_value < AppConfig::LCD_BRIGHTNESS_MAX) {
                    _brightness_value += AppConfig::LCD_BRIGHTNESS_STEP;
                    if (_brightness_value > AppConfig::LCD_BRIGHTNESS_MAX) _brightness_value = AppConfig::LCD_BRIGHTNESS_MAX;
                    settings.setLcdBrightness(_brightness_value);
                    settings.requestSave();
                    hw.display.setBacklightBrightness(_brightness_value);
                }
                playNavBeep();
            } else if (_selected_settings_item == SettingsItem::DIM_TIMEOUT && _dim_timeout_adjusting) {
                if (_dim_timeout_value < AppConfig::AUTO_DIM_MAX_MINUTES) {
                    _dim_timeout_value++;
                    settings.setAutoDimMinutes(_dim_timeout_value);
                    settings.requestSave();
                }
                playNavBeep();
            } else if (_selected_settings_item == SettingsItem::STARTUP_MELODY && _melody_adjusting) {
                if (_melody_value < AppConfig::STARTUP_MELODY_MAX) {
                    _melody_value++;
                    settings.setStartupMelody(_melody_value);
                    settings.requestSave();
                    // Preview melody on change
                    const Note* melody = getStartupMelody(_melody_value);
                    uint8_t length = getStartupMelodyLength(_melody_value);
                    if (melody && length > 0) {
                        hw.buzzer.playMelody(melody, length);
                    }
                }
            } else if (_selected_settings_item == SettingsItem::STARTUP_CONTRACT && _contract_mode_adjusting) {
                if (_contract_mode_value < AppConfig::STARTUP_CONTRACT_MODE_MAX) {
                    _contract_mode_value++;
                    settings.setStartupNegotiation(_contract_mode_value);
                    // When switching to Last Used, immediately snapshot current contract
                    if (static_cast<StartupContractMode>(_contract_mode_value) == StartupContractMode::LAST_USED) {
                        saveCurrentContractSnapshot();
                    }
                    settings.requestSave();
                }
                playNavBeep();
            } else {
                // Move down in settings menu (with wrap-around)
                int next = static_cast<int>(_selected_settings_item) + 1;
                if (next >= static_cast<int>(SettingsItem::SETTINGS_COUNT)) {
                    next = 0;
                }
                _selected_settings_item = static_cast<SettingsItem>(next);
                playNavBeep();
            }
            _last_activity_time = get_absolute_time();
            break;

        case EncoderEvent::ROTATE_CCW:
            if (_selected_settings_item == SettingsItem::BRIGHTNESS && _brightness_adjusting) {
                if (_brightness_value > AppConfig::LCD_BRIGHTNESS_MIN) {
                    _brightness_value -= AppConfig::LCD_BRIGHTNESS_STEP;
                } else {
                    _brightness_value = AppConfig::LCD_BRIGHTNESS_MIN;
                }
                settings.setLcdBrightness(_brightness_value);
                settings.requestSave();
                hw.display.setBacklightBrightness(_brightness_value);
                playNavBeep();
            } else if (_selected_settings_item == SettingsItem::DIM_TIMEOUT && _dim_timeout_adjusting) {
                if (_dim_timeout_value > AppConfig::AUTO_DIM_MIN_MINUTES) {
                    _dim_timeout_value--;
                    settings.setAutoDimMinutes(_dim_timeout_value);
                    settings.requestSave();
                }
                playNavBeep();
            } else if (_selected_settings_item == SettingsItem::STARTUP_MELODY && _melody_adjusting) {
                if (_melody_value > 0) {
                    _melody_value--;
                    settings.setStartupMelody(_melody_value);
                    settings.requestSave();
                    // Preview melody on change
                    const Note* melody = getStartupMelody(_melody_value);
                    uint8_t length = getStartupMelodyLength(_melody_value);
                    if (melody && length > 0) {
                        hw.buzzer.playMelody(melody, length);
                    } else {
                        hw.buzzer.stopMelody();  // Silent selected
                    }
                }
            } else if (_selected_settings_item == SettingsItem::STARTUP_CONTRACT && _contract_mode_adjusting) {
                if (_contract_mode_value > 0) {
                    _contract_mode_value--;
                    settings.setStartupNegotiation(_contract_mode_value);
                    // When switching to Last Used, immediately snapshot current contract
                    if (static_cast<StartupContractMode>(_contract_mode_value) == StartupContractMode::LAST_USED) {
                        saveCurrentContractSnapshot();
                    }
                    settings.requestSave();
                }
                playNavBeep();
            } else {
                // Move up in settings menu (with wrap-around)
                int prev = static_cast<int>(_selected_settings_item) - 1;
                if (prev < 0) {
                    prev = static_cast<int>(SettingsItem::SETTINGS_COUNT) - 1;
                }
                _selected_settings_item = static_cast<SettingsItem>(prev);
                playNavBeep();
            }
            _last_activity_time = get_absolute_time();
            break;

        case EncoderEvent::CLICK:
            switch (_selected_settings_item) {
                case SettingsItem::FLASH_EEPROM:
                    _adjust_mode = AdjustMode::EEPROM_FLASH;
                    displayManager.invalidate();
                    tpsEepromWorkflow.start();
                    break;

                case SettingsItem::AUTO_PPS:
                    settings.setAutoPpsEnabled(!settings.isAutoPpsEnabled());
                    settings.requestSave();
                    break;

                case SettingsItem::AUTO_AVS:
                    settings.setAutoAvsEnabled(!settings.isAutoAvsEnabled());
                    settings.requestSave();
                    break;

                case SettingsItem::AUTO_OUTPUT:
                    settings.setAutoOutput(!settings.isAutoOutput());
                    settings.requestSave();
                    break;

                case SettingsItem::BRIGHTNESS:
                    _brightness_adjusting = !_brightness_adjusting;
                    break;

                case SettingsItem::DIM_TIMEOUT:
                    _dim_timeout_adjusting = !_dim_timeout_adjusting;
                    break;

                case SettingsItem::STARTUP_MELODY:
                    _melody_adjusting = !_melody_adjusting;
                    break;

                case SettingsItem::STARTUP_CONTRACT:
                    _contract_mode_adjusting = !_contract_mode_adjusting;
                    break;

                case SettingsItem::SOUNDS:
                    settings.setSoundsEnabled(!settings.isSoundsEnabled());
                    settings.requestSave();
                    break;

                case SettingsItem::BACK:
                    transitionTo(AppState::MENU);
                    break;

                default:
                    break;
            }
            _last_activity_time = get_absolute_time();
            break;

        case EncoderEvent::LONG_PRESS:
            // Long press disabled (variable kept for future use)
            break;

        default:
            break;
    }
}

uint32_t StateMachine::getEffectiveMaxCurrentMa() const {
    const ActiveContract& contract = pdManager.getActiveContract();
    if (contract.valid && contract.current_ma > 0) {
        // Cap to the lesser of hardware max and contract max
        return (contract.current_ma < AppConfig::CURRENT_LIMIT_MAX_MA)
               ? contract.current_ma
               : AppConfig::CURRENT_LIMIT_MAX_MA;
    }
    // No valid PD contract - cap at 3A (USB BC1.2 limit)
    return AppConfig::CURRENT_LIMIT_NON_PD_MAX_MA;
}

