# SneakPeak — Agent Instructions

## First Thing Every Session
Read `.harness/AGENT.md` and follow its startup sequence before doing anything else.

## Build
```bash
cd /Volumes/@Basic/Projekty/EditView/cpp/build
make -j$(sysctl -n hw.ncpu)
```

## Install to REAPER
```bash
cp build/reaper_sneakpeak.dylib ~/Library/Application\ Support/REAPER/UserPlugins/reaper_sneakpeak-arm64.dylib
```

## REAPER API Reference
Use MCP tools: `mcp__reaper-dev__get_function_info` and `mcp__reaper-dev__search_functions` (api: "reascript")

## Key Constraint
This is a community open-source project. Code quality is publicly visible. No regressions. Build clean. Commit clean.
