# PD240W Host Unit Tests

Host-side (x86/ARM desktop) unit tests for the hardware-free logic in the
firmware. These compile the pure logic headers directly — no Pico SDK, no
target hardware — and run in well under a second under
[Snitch](https://github.com/snitch-org/snitch), with AddressSanitizer and
UndefinedBehaviorSanitizer enabled by default.

Only logic that is decoupled from the Pico SDK can be tested here: a module is
host-buildable when none of the headers it includes pull in `pico/*`.

---

## Build and run

Configure into a `build/` subdirectory (in-source builds are rejected):

```bash
cd test
cmake -G Ninja -B build       # configure (downloads Snitch on first run)
cmake --build build           # compile
cd build && ctest --output-on-failure
```

`ctest --output-on-failure` runs the suite and prints detail only on failure.
Running the binary directly (`./build/pd240w_tests`) shows Snitch's full report;
pass `--help` for filtering options.

The first configure takes longer because CMake fetches the Snitch framework via
`FetchContent`; subsequent configures reuse the cached copy.

---

## Options

Pass these to the configure step:

| Flag | Default | Effect |
|------|---------|--------|
| `-DENABLE_SANITIZERS=OFF` | ON | Disable ASan/UBSan (faster; loses memory/UB checking) |
| `-DENABLE_COVERAGE=OFF` | ON | Instrument for gcov and add a `coverage` target |

Changing an option requires re-running configure, e.g.
`cmake -G Ninja -B build -DENABLE_COVERAGE=OFF`.

---

## Coverage report

Coverage reports which lines the tests exercise. It requires `lcov` and
`genhtml`, which are not installed on macOS by default:

```bash
brew install lcov             # macOS; on Debian/Ubuntu: apt install lcov
```

Then:

```bash
cd test
cmake -G Ninja -B build -DENABLE_COVERAGE=ON
cmake --build build --target coverage
open build/coverage_report/index.html
```

Notes:

- The `coverage` target only exists when configured with `-DENABLE_COVERAGE=ON`.
- Without `lcov`/`genhtml` the target reports that they are missing rather than
  producing an empty result.

---

## Suites

| Suite | Module under test |
|-------|-------------------|
| `test_pd_diagnostics.cpp` | `src/logic/pd_diagnostics.h` — startup-contract matching (PPS/AVS clamping and step alignment), charger-identity VDO decode, cable-rating inference, and power/vendor/string helpers |

---

## How the host build sees hardware types

`pd_diagnostics.h` depends on two data types that were previously
hardware-coupled:

- `TPS26750_SourceCapability` — the TPS26750 driver is multiplatform. The test
  build defines `TPS26750_PLATFORM_NATIVE` (see `CMakeLists.txt`), selecting a
  desktop code path so its plain-data structs compile without an MCU I2C layer.
- `SavedStartupContractType` — extracted, with the other flash enums, into
  `src/logic/settings_types.h`, a header that includes only `<cstdint>`, so pure
  logic no longer pulls in the Pico-SDK-coupled `Settings` class.

---

## Adding a suite

1. Create `test_<module>.cpp` including `<snitch/snitch.hpp>`.
2. Add it to the `add_executable(pd240w_tests ...)` list in `CMakeLists.txt`.
3. Reconfigure, build, and run.

If the module does not compile because a header pulls in `pico/*`, extract its
pure logic into a `logic/`-style header first (as with `settings_types.h`).
