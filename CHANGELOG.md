# Changelog

## [2.0.4] - 2026-07-07

### Bug Fixes

- Waking the screen from auto-dim now restores the saved brightness instead of a stale menu value, and the encoder turn or button press that wakes the screen no longer also changes a menu selection or toggles the output.
- A fault that changes type while already faulted (e.g. overcurrent then overtemperature) now redraws the screen instead of leaving the previous fault's details on display.
- INA228 die-temperature readings are sign-corrected, fixing a spurious over-temperature fault when the board is cold.
- Thermal status no longer sticks at FAULT after cooling back into an already-latched warning band.
- A TPS26750 EEPROM flash can no longer be abandoned mid-write by a fault or menu timeout, preventing a half-programmed EEPROM.
- The load switch now refuses to enable when the INA228 power monitor failed to initialize, instead of silently enabling an unmonitored output with no overcurrent protection.
- Buzzer pitch is now correct below ~120 Hz, and the alarm tone can no longer latch on indefinitely.
- Added I2C timeouts and a watchdog to prevent bus-hang lockups; PPS/AVS keep-alive now backs off after a failed request instead of hammering the bus every loop.
- Fixed assorted display and LED glitches: LED stuck yellow after a fault clears, mis-clamped minimum brightness, and unreachable PDO-list rows after a mid-selection contract refresh.

### CLI

- `MEAS:ENERGY?` now reports energy in mWh; added `MEAS:CHARGE?` for delivered charge in mAh.
- About-screen firmware version strings now show clean release tags.

### Internal

- Major testability refactor: extracted the pure logic (PD diagnostics, PPS/AVS tuning, constant-current regulation, settings flash codec, thermal FSM, EPR safe-exit sequencing) into host-testable headers with a Snitch/ASan/UBSan unit-test suite, CI, and a 90% coverage floor.
- CI now builds our sources with `-Werror` so warning regressions fail the PR; deduplicated PPS/AVS keep-alive and PDO handling.

## [2.0.3] - 2026-07-04

### Features

- PPS voltage range now goes as low as 3.3V, matching the full USB-PD PPS spec.

### Improvements

- TPS26750 PD configuration is now pushed over I2C at boot instead of requiring an EEPROM flash, and flash size is configurable instead of hardcoded to 2MB.
- Bumped the shared TPS26750 driver, fixing Max Power showing 0W for non-PD chargers and other small bugs (brightness CLI, buzzer melody latch).
- Lowered the PD disconnect VBUS threshold to 3.3V.

### Internal

- Code-review cleanup pass (warnings, dead code, deduplication) and SDK bump to 2.3.0.

## [2.0.2] - 2026-06-25

### Internal

- Migrated the TPS26750 USB-PD driver to the shared `tps26750_multiplatform` submodule, replacing the in-tree driver.

### UI Improvements

- Added a TPS firmware version line to the About PD240W screen and tightened its spacing so the layout sits clear of the bottom navigation hint.

## [2.0.1] - 2026-06-16

### UI Improvements

- Voltage select menu now fits up to 10 contracts on screen (was 8) and sits closer to the header, reducing scrolling on chargers with many contracts.

## [2.0.0] - 2026-05-31

### USB-PD 3.1 / 3.2 Release

- Added USB-PD 3.1 and 3.2 firmware support, making AVS and EPR negotiation possible.
- Added completed constant-current control work, including an OFF current-limit mode and updated INA228 integration.
- Improved USB-PD behavior across Fixed, PPS, and EPR AVS profiles with closed-loop tuning, safer contract transitions, smarter startup restore, and better high-voltage charger handling.
- Added charger diagnostics and a USB serial CLI for remote inspection and control.
- Refreshed the interface with PD240W branding, smoother visuals, clearer about screens, richer boot information, and more configurable settings such as auto-dim control.
- Fixed reboot voltage spikes, badge and cursor restore issues, noisy logs, menu navigation edge cases, charger-change restore problems, and several AVS/EPR negotiation regressions.

## [1.0.0]

- Initial public firmware release.