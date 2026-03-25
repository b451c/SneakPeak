# SneakPeak Refactor Log

## Summary
Refactored the 4,336-line monolith `edit_view.cpp` into 5 focused modules.
Zero regressions. All features preserved. Builds clean.

Recovery tag: `v1.6.0-pre-refactor` (local)

## Results

| File | Before | After | Change |
|------|--------|-------|--------|
| edit_view.cpp | 4,336 | 2,523 | **-42%** |
| standalone_file.cpp | — | 545 | new |
| audio_commands.cpp | — | 823 | new |
| context_menu.cpp | — | 389 | new |
| drag_export.cpp | — | 130 | new |
| waveform_view.cpp | 1,738 | 1,716 | -22 (dedup) |
| levels_panel.cpp | 413 | 379 | -34 (dedup) |
| Total | 11,222 | 11,268 | +46 (headers) |

## Commits (11 total)

### Bug Fixes
1. `WriteAndRefresh` — check write success before setting dirty
2. `marker_manager` — encapsulate public state (private + getters/setters)

### Extractions
3. `FileNameFromPath` → `config.h` (inline, shared across all TUs)
4. `standalone_file.cpp` — tab lifecycle, save/load/save-as, preview playback (528 lines)
5. `audio_commands.cpp` — clipboard, undo, editing, normalize, fade, reverse, gain (803 lines)
6. `context_menu.cpp` — right-click menu + command dispatch (371 lines)
7. `drag_export.cpp` — drag & drop WAV export (111 lines)

### Deduplication
8. `waveform_view` — `GetActiveFadeParams()` replaces 3x duplicated fade param blocks
9. `levels_panel` — `GetBallistics()` replaces 2x duplicated switch blocks (C++17 structured bindings)

### Performance
10. `toolbar` — cached font via `g_fonts.toolbar` instead of per-frame CreateFont/DeleteObject

### Architecture
- No new classes — used C++ multi-TU member implementation (each .cpp includes edit_view.h)
- `s_clipboard` static member stays in edit_view.cpp (single definition)
- Static helpers (`MenuAppend`, `GenerateEditPath`) moved with their callers
