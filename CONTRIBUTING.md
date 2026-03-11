# Contributing to SneakPeak

Thanks for your interest in contributing to SneakPeak!

## Getting Started

### Prerequisites

- C++17 compiler (Clang on macOS, GCC on Linux, MSVC on Windows)
- CMake 3.15+
- REAPER 7.0+ for testing

### Building

```bash
# Clone dependencies
git clone https://github.com/justinfrankel/reaper-sdk.git sdk
git clone https://github.com/justinfrankel/WDL.git WDL

# Build
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(sysctl -n hw.ncpu)

# Install
cp reaper_sneakpeak.dylib ~/Library/Application\ Support/REAPER/UserPlugins/
```

Debug builds log to `/tmp/sneakpeak_debug.log`.

## Code Style

- C++17, no exceptions, no RTTI
- SWELL API for cross-platform windowing (no direct Win32 or Cocoa calls in core code)
- Named constants in `config.h` — no magic numbers
- Compile with zero warnings (`-Wall -Wextra -Wshadow -Wconversion`)

## Pull Request Process

1. Fork the repository and create a feature branch (`git checkout -b feature/my-feature`)
2. Make your changes and ensure the build compiles with zero warnings
3. Test in REAPER — verify the extension loads, basic view/selection/editing works
4. Commit with a descriptive message
5. Push and open a Pull Request against `main`

## Reporting Bugs

Use [GitHub Issues](https://github.com/b451c/SneakPeak/issues) to report bugs. Include:

- REAPER version and OS
- Steps to reproduce
- Expected vs actual behavior
- Debug log output if available (`/tmp/sneakpeak_debug.log` from Debug builds)

## Feature Requests

Use [GitHub Issues](https://github.com/b451c/SneakPeak/issues) to suggest features. Describe the use case and how it fits into your workflow.
