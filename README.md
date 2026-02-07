# PD240W

![CI](https://github.com/theohg/PD240W/actions/workflows/ci.yml/badge.svg)
![Pico SDK](https://img.shields.io/badge/Pico_SDK-2.2.0-blue)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)
![Platform](https://img.shields.io/badge/Platform-RP2040-green)

An adjustable power supply for motor drives using USB-C Power Delivery negotiation, supporting up to **240W at 48V 5A**. Firmware runs on a Raspberry Pi Pico (RP2040).

## Features

- **USB-C Power Delivery**: Negotiates Fixed, PPS (5-21V programmable), and AVS (15-48V EPR) profiles
- **Current Limiting**: Adjustable 10mA-5A via INA228 power monitor with hardware overcurrent protection
- **LCD Interface**: 240x320 ST7789 display with anti-aliased fonts and Prusa-style encoder navigation
- **Safety**: Overcurrent ISR, overtemperature monitoring (NTC + INA228), PD disconnect detection
- **Settings Persistence**: User settings stored in RP2040 flash (survives power cycles)
- **Auto PPS Tuning**: Closed-loop voltage correction for PPS charger output accuracy
- **Energy Monitoring**: Tracks mAh delivered since boot via INA228 charge accumulator
- **17V Buck Output**: Optional STO/SBC voltage for motor drive safety circuits
- **Configurable**: Brightness, auto-dim, startup melody, auto-output on boot

## Hardware

| Component | Specification |
|-----------|--------------|
| MCU | Raspberry Pi Pico (RP2040) |
| Display | 240x320 2.4" ST7789 (SPI) |
| USB-C PD Controller | TI TPS26750 |
| Current Sensor | TI INA228 (8mOhm shunt) |
| RGB LED | SK6812 (PIO driven) |
| Max Output | 48V @ 5A (240W) |

## Quick Start

### Flash Pre-built Firmware

1. Download `PD240W.uf2` from the [latest release](https://github.com/theohg/PD240W/releases/latest)
2. Hold **BOOTSEL** button on the Pico while connecting USB
3. Drag `PD240W.uf2` to the mounted `RPI-RP2` drive

### Build from Source

Requires: [Pico SDK 2.2.0](https://github.com/raspberrypi/pico-sdk), ARM GCC toolchain, CMake, Ninja

```bash
# Clone
git clone https://github.com/theohg/PD240W.git
cd PD240W

# Build
mkdir -p build && cd build
cmake -G Ninja ..
ninja
```

The output binary is `build/PD240W.uf2`.

### Flash via SWD

```bash
openocd -f interface/cmsis-dap.cfg -f target/rp2040.cfg \
  -c "adapter speed 5000; program build/PD240W.elf verify reset exit"
```

## Controls

| Input | Action |
|-------|--------|
| Encoder Rotate | Navigate menus / Adjust values |
| Encoder Click | Confirm / Select |
| Encoder Long Press | Go Back / Exit current screen |
| BTN1 | Toggle load switch output |
| BTN2 | Toggle 17V buck (requires VBUS > 18V) |

## Project Structure

```
src/
├── main.cpp              # Entry point, main event loop
├── hardware.h/cpp        # Hardware singleton
├── config/               # Pin definitions, constants, version
├── drivers/              # Hardware drivers (GPIO, display, power, input)
├── logic/                # State machine, safety, PD manager, settings
└── ui/                   # Display rendering
```

## Author

**Theo Heng** - [Synapticon GmbH](https://www.synapticon.com)
