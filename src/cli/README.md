# PD240W Serial CLI Reference

## Connection

The PD240W exposes a serial CLI over USB CDC at **115200 baud, 8N1**. Lines are terminated with `\n`.

> [!NOTE]
> On the current PCB revision, native USB CDC requires the USB D+/D- rework described in the repository [README](../../README.md).

**Finding the serial port:**

| OS | Command |
|----|---------|
| macOS | `ls /dev/tty.usbmodem*` |
| Linux | `ls /dev/ttyACM*` |
| Windows | Device Manager → Ports (COM & LPT) |

Connect with any serial terminal:
```bash
# macOS / Linux
screen /dev/tty.usbmodem1234 115200
# or
minicom -D /dev/ttyACM0 -b 115200
```

## Protocol Rules

- **Queries** end with `?` and return the value on a single line: `20000\n`
- **Set commands** include a value after a space and return `OK\n` on success
- **Action commands** (no argument) return `OK\n`
- **Errors** return `ERR <CODE>\n` immediately

### Error Codes

| Code | Description |
|------|-------------|
| `UNKNOWN_CMD` | Command not found in dispatch table |
| `INVALID_PARAM` | Argument is missing, malformed, or wrong type (e.g. non-numeric, not ON/OFF) |
| `INVALID_INDEX` | PDO index out of range |
| `OUT_OF_RANGE` | Numeric value outside allowed bounds |
| `NOT_AVAILABLE` | Operation not possible (e.g. PPS command with no PPS contract, buck with low VBUS) |
| `FAULT_ACTIVE` | Output command rejected because a safety fault is active |

### Log Output

Firmware log lines use prefixes: `[INFO]`, `[WARN]`, `[ERROR]`, `[DEBUG]`. These are separate from CLI responses and can be suppressed:

- `LOG:OFF` — Suppress all log output (recommended for scripting)
- `LOG:ON` — Re-enable log output

### Remote Control Mode

Receiving any serial command puts the device in **REMOTE** mode:
- Front panel is locked (encoder/buttons disabled)
- LCD shows a red **RMT** badge
- Long-press encoder returns to local control
- `SYST:LOC` command also exits remote mode

## Command Reference

### Identity & Status

| Command | Arguments | Response | Description |
|---------|-----------|----------|-------------|
| `*IDN?` | — | `PD240W,HWA.1,FWv2.0.0` | Device identification |
| `SYST:STAT?` | — | `MAIN` / `FAULT:OVERTEMP` / ... | System state |
| `SYST:UPTIME?` | — | `12345` | Uptime in seconds |
| `SYST:REBOOT` | — | `OK` | Software reboot (watchdog reset) |
| `SYST:BOOTSEL` | — | `OK` | Reboot into BOOTSEL (UF2 flash mode) |
| `SYST:LOC` | — | `OK` | Exit remote mode, return to local control |

### Output Control

| Command | Arguments | Response | Description |
|---------|-----------|----------|-------------|
| `OUTP:SW?` | — | `ON` / `OFF` | Query load switch state |
| `OUTP:SW` | `ON` / `OFF` | `OK` | Enable/disable load switch |
| `OUTP:BUCK?` | — | `ON` / `OFF` | Query 17V buck state |
| `OUTP:BUCK` | `ON` / `OFF` | `OK` | Enable/disable 17V buck (requires VBUS > 18V) |

### Measurements (read-only)

| Command | Arguments | Response | Description |
|---------|-----------|----------|-------------|
| `MEAS:VOLT?` | — | `20000` | Output voltage in mV (from INA228) |
| `MEAS:CURR?` | — | `1500` | Output current in mA |
| `MEAS:POW?` | — | `30000` | Output power in mW |
| `MEAS:TEMP?` | — | `452` | Board temperature in °C × 10 (45.2°C) |
| `MEAS:ITEMP?` | — | `380` | INA228 die temperature in °C × 10 |
| `MEAS:ENERGY?` | — | `1234` | Accumulated energy since boot in mAh |
| `MEAS:VBUS?` | — | `20100` | Pre-switch VBUS voltage in mV (from ADC) |
| `MEAS:ALL?` | — | `20000,1500,30000,452,380,1234` | All measurements, comma-separated |

### PD Contract Management

| Command | Arguments | Response | Description |
|---------|-----------|----------|-------------|
| `PD:LIST?` | — | `0:5000/3000,1:9000/3000,...` | List available PDOs (index:mV/mA or PPS/AVS format) |
| `PD:ACTIVE?` | — | `20000,3000,FIXED` | Active contract: voltage, current, type |
| `PD:REV?` | — | `PD3.2` | PD revision string (`PD2.0` / `PD3.0` / `PD3.1` / `PD3.2`) |
| `PD:SEL` | `<index>` | `OK` | Select PDO by index (from PD:LIST?) |
| `PD:PPS` | `<voltage_mv>` | `OK` | Set PPS voltage in mV (must be in active PPS range) |
| `PD:AVS` | `<voltage_mv>` | `OK` | Set AVS voltage in mV (must be in active AVS range) |

### Current Limit

| Command | Arguments | Response | Description |
|---------|-----------|----------|-------------|
| `CURR:LIM?` | — | `1500` | Query current limit in mA |
| `CURR:LIM` | `<10-5000>` | `OK` | Set current limit in mA (10 mA steps) |

### Settings

| Command | Arguments | Response | Description |
|---------|-----------|----------|-------------|
| `SETT:BRIGHT?` | — | `80` | Query LCD brightness (5–100) |
| `SETT:BRIGHT` | `<5-100>` | `OK` | Set LCD brightness |
| `SETT:SOUND?` | — | `ON` / `OFF` | Query sounds enabled |
| `SETT:SOUND` | `ON` / `OFF` | `OK` | Toggle sounds |
| `SETT:AUTOPPS?` | — | `ON` / `OFF` | Query auto PPS tuning |
| `SETT:AUTOPPS` | `ON` / `OFF` | `OK` | Toggle auto PPS tuning |
| `SETT:AUTOAVS?` | — | `ON` / `OFF` | Query auto AVS tuning |
| `SETT:AUTOAVS` | `ON` / `OFF` | `OK` | Toggle auto AVS tuning |
| `SETT:AUTOOUT?` | — | `ON` / `OFF` | Query auto output |
| `SETT:AUTOOUT` | `ON` / `OFF` | `OK` | Toggle auto output |
| `SETT:DIM?` | — | `5` | Query dim timeout in minutes |
| `SETT:DIM` | `<0-10>` | `OK` | Set dim timeout in minutes (`0` = OFF) |
| `SETT:SAVE` | — | `OK` | Force save settings to flash |
| `SETT:RESET` | — | `OK` | Reset all settings to defaults |

### Log Control

| Command | Arguments | Response | Description |
|---------|-----------|----------|-------------|
| `LOG:ON` | — | `OK` | Enable firmware log output |
| `LOG:OFF` | — | `OK` | Suppress firmware log output |

### System

| Command | Arguments | Response | Description |
|---------|-----------|----------|-------------|
| `SYST:REBOOT` | — | `OK` | Software reboot via watchdog |
| `SYST:BOOTSEL` | — | `OK` | Reboot into UF2 flash mode |
| `SYST:LOC` | — | `OK` | Return to local (front panel) control |

## Raw Session Example

```
> *IDN?
PD240W,HWA.1,FWv2.0.0
> LOG:OFF
OK
> SYST:STAT?
MAIN
> PD:LIST?
0:5000/3000,1:9000/3000,2:15000/3000,3:20000/3000,4:PPS/3300-21000/5000
> PD:ACTIVE?
20000,3000,FIXED
> PD:REV?
PD3.0
> PD:SEL 4
OK
> PD:PPS 9500
OK
> MEAS:ALL?
9480,0,0,312,298,0
> CURR:LIM 2000
OK
> CURR:LIM?
2000
> OUTP:SW ON
OK
> MEAS:VOLT?
9500
> MEAS:CURR?
850
> MEAS:POW?
8075
> SETT:BRIGHT 70
OK
> OUTP:SW OFF
OK
```

## Python Quick Start

```python
from pd240w import PD240W

with PD240W("/dev/ttyACM0") as psu:
    print(psu.identity())           # PD240W,HWA.1,FWv2.0.0
    print(psu.list_pdos())          # ['0:5000/3000', '1:9000/3000', ...]
    psu.select_pdo(3)               # Select 20V
    psu.output(True)                # Enable output
    m = psu.measurements()
    print(f"{m['voltage_mv']/1000:.2f}V  {m['current_ma']/1000:.3f}A")
    psu.output(False)               # Disable output
```

See [tools/pd240w.py](../../tools/pd240w.py) for the full library and CLI tool.
