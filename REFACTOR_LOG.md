# SneakPeak Refactor Log (v1.7.0)

## Summary
Monolithic codebase refactored into focused modules.
17 commits. Zero regressions. All features preserved.

Recovery tag: `v1.6.0-pre-refactor` (local)

## Results

| File | Before | After | Role |
|------|--------|-------|------|
| edit_view.cpp | 4,336 | **796** | Core: lifecycle, timer, layout, message dispatch |
| rendering.cpp | — | 923 | Paint, Draw*, solo button, toast |
| input_handling.cpp | — | 847 | Mouse, keyboard, toolbar dispatch |
| audio_commands.cpp | — | 823 | Clipboard, undo, editing, processing |
| standalone_file.cpp | — | 545 | Tab lifecycle, save/load, preview |
| context_menu.cpp | — | 389 | Right-click menu, command dispatch |
| drag_export.cpp | — | 130 | Drag & drop WAV export |
| waveform_view.cpp | 1,716 | 653 | Data: load, zoom, selection, coordinates |
| waveform_rendering.cpp | — | 1,086 | Peaks, waveform draw, fades, dB scale |
| levels_panel.cpp | 413 | 379 | Dedup: GetBallistics |

**edit_view.cpp: -82% (4,336 → 796)**
**waveform_view.cpp: -62% (1,716 → 653)**
**No file exceeds 1,100 lines**

## Bug Fixes
- **RefreshItemSource memory leak** — use P_SOURCE instead of SetMediaItemTake_Source
- **WriteAndRefresh** — check write success before setting dirty
- **marker_manager** — encapsulate public state

## Performance
- **PreventUIRefresh** in all 13 REAPER undo blocks
- **Toolbar font** cached via g_fonts.toolbar
- **Deduplicated** fade params (3x→1x) and ballistics (2x→1x)

## Architecture
- 8 new .cpp modules, each includes edit_view.h
- No new classes — C++ multi-TU member implementation
- Static members (s_clipboard) in single TU
- FileNameFromPath shared via config.h inline
