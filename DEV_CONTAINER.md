# PD240W Dev Container Setup

Reproducible development environment for PD240W firmware and host-based unit tests using VSCode Dev Containers.

---

## Overview

The dev container provides:
- ARM cross-compilation toolchain (gcc-arm-none-eabi) for RP2040 firmware
- Pico SDK v2.2.0 pre-installed
- Host C++ toolchain (clang) for unit tests
- Static analysis tools (clang-tidy, cppcheck)
- Code formatting (clang-format)
- Coverage tools (lcov, genhtml)
- CMake + Ninja build system

---

## Files to Create

### `.devcontainer/devcontainer.json`

```json
{
    "name": "PD240W Development",
    "build": {
        "dockerfile": "Dockerfile"
    },
    "customizations": {
        "vscode": {
            "extensions": [
                "ms-vscode.cpptools",
                "ms-vscode.cmake-tools",
                "llvm-vs-code-extensions.vscode-clangd",
                "xaver.clang-format",
                "ms-vscode.cpptools-extension-pack"
            ],
            "settings": {
                "cmake.generator": "Ninja",
                "cmake.buildDirectory": "${workspaceFolder}/build",
                "C_Cpp.default.compileCommands": "${workspaceFolder}/build/compile_commands.json",
                "[cpp]": {
                    "editor.formatOnSave": true,
                    "editor.defaultFormatter": "xaver.clang-format"
                },
                "[c]": {
                    "editor.formatOnSave": true,
                    "editor.defaultFormatter": "xaver.clang-format"
                }
            }
        }
    },
    "postCreateCommand": "bash .devcontainer/post-create.sh",
    "remoteUser": "vscode"
}
```

### `.devcontainer/Dockerfile`

```dockerfile
FROM mcr.microsoft.com/devcontainers/cpp:1-debian-12

# Avoid interactive prompts
ENV DEBIAN_FRONTEND=noninteractive

# Install ARM toolchain + analysis tools
RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc-arm-none-eabi \
    libnewlib-arm-none-eabi \
    cmake \
    ninja-build \
    git \
    python3 \
    python3-pip \
    clang \
    clang-tidy \
    clang-format \
    cppcheck \
    lcov \
    gdb-multiarch \
    && rm -rf /var/lib/apt/lists/*

# Install Pico SDK
ENV PICO_SDK_PATH=/opt/pico-sdk
RUN git clone --branch 2.2.0 --depth 1 \
    https://github.com/raspberrypi/pico-sdk.git ${PICO_SDK_PATH} \
    && cd ${PICO_SDK_PATH} \
    && git submodule update --init --depth 1

# Set environment for CMake to find the SDK
ENV PICO_TOOLCHAIN_PATH=/usr
```

### `.devcontainer/post-create.sh`

```bash
#!/bin/bash
set -e

echo "=== PD240W Dev Container Setup ==="

# Verify toolchain
echo "ARM GCC: $(arm-none-eabi-gcc --version | head -1)"
echo "Clang:   $(clang --version | head -1)"
echo "CMake:   $(cmake --version | head -1)"
echo "Ninja:   $(ninja --version)"
echo "Pico SDK: ${PICO_SDK_PATH}"

# Build firmware
echo ""
echo "=== Building firmware ==="
mkdir -p build && cd build
cmake -G Ninja ..
ninja

echo ""
echo "=== Build complete ==="
arm-none-eabi-size PD240W.elf

# Build tests (if test/ directory exists)
if [ -d "../test" ]; then
    echo ""
    echo "=== Building tests ==="
    cd ../test
    mkdir -p build && cd build
    cmake -G Ninja -DCMAKE_CXX_COMPILER=clang++ ..
    ninja
    echo ""
    echo "=== Running tests ==="
    ./pd240w_tests
fi
```

---

## Usage

### Prerequisites
- [Docker Desktop](https://www.docker.com/products/docker-desktop/) installed and running
- [VSCode](https://code.visualstudio.com/) with the [Dev Containers extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-containers)

### Opening the Container

1. Open the PD240W project folder in VSCode
2. Press `Cmd+Shift+P` (macOS) or `Ctrl+Shift+P` (Linux/Windows)
3. Select **"Dev Containers: Reopen in Container"**
4. Wait for the container to build (first time takes ~5 minutes, cached after that)

The `post-create.sh` script automatically builds the firmware and runs tests.

### Manual Commands Inside Container

```bash
# Firmware build
cd build && ninja

# Reconfigure (updates build date)
cd build && cmake -G Ninja .. && ninja

# Clean rebuild
cd build && rm -rf ./* && cmake -G Ninja .. && ninja

# Run unit tests
cd test/build && ./pd240w_tests

# Run tests with coverage
cd test/build
cmake -G Ninja -DENABLE_COVERAGE=ON .. && ninja
ninja coverage
# Open test/build/coverage_report/index.html

# Static analysis
cd test/build
run-clang-tidy -p . -header-filter='src/.*' ../../src/
cppcheck --enable=all --suppress=missingIncludeSystem --project=compile_commands.json

# Format check
find src test -name '*.h' -o -name '*.cpp' | xargs clang-format --dry-run --Werror

# Format fix
find src test -name '*.h' -o -name '*.cpp' | xargs clang-format -i
```

### Notes

- The container uses Debian 12 (bookworm) as base — same as Raspberry Pi OS
- Pico SDK is installed at `/opt/pico-sdk` and set via `PICO_SDK_PATH`
- The ARM toolchain is the system package (`gcc-arm-none-eabi`), not the Pico SDK's bundled one
- For font generation, install Pillow: `pip3 install Pillow` then run `python3 tools/generate_font.py ...`
- The container does **not** include hardware flashing tools (picotool, OpenOCD) — flash from the host
