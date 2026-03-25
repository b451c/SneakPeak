# SneakPeak Refactor Log

## Summary
Refactored the 4,336-line monolith `edit_view.cpp` into 7 focused modules.
Zero regressions. All features preserved. Builds clean.

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
| waveform_view.cpp | 1,738 | 1,716 | Dedup: GetActiveFadeParams |
| levels_panel.cpp | 413 | 379 | Dedup: GetBallistics |

**edit_view.cpp reduction: 82% (4,336 → 796)**
**No file exceeds 1,000 lines** (except waveform_view.cpp — focused rendering engine)

## Commits (13 total)

### Bug Fixes
1. `WriteAndRefresh` — check write success before setting dirty
2. `marker_manager` — encapsulate public state

### Shared Utilities
3. `FileNameFromPath` → `config.h` inline

### Module Extractions
4. `standalone_file.cpp` — 528 lines from edit_view.cpp
5. `audio_commands.cpp` — 803 lines from edit_view.cpp
6. `context_menu.cpp` — 371 lines from edit_view.cpp
7. `drag_export.cpp` — 111 lines from edit_view.cpp
8. `rendering.cpp` — 902 lines from edit_view.cpp
9. `input_handling.cpp` — 825 lines from edit_view.cpp

### Deduplication
10. `waveform_view` — GetActiveFadeParams (3x → 1x)
11. `levels_panel` — GetBallistics (2x → 1x, C++17 structured bindings)

### Performance
12. `toolbar` — cached font via g_fonts.toolbar

### Documentation
13. Complete refactor log
