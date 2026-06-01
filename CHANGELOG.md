# Changelog

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