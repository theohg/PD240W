# Changelog

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