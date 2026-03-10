# EditView — Release Readiness Audit

## P0 — Must Fix Before Release

### B1. Stale MediaItem pointer (crash risk)
- **File:** `main.cpp:97`
- `g_lastSelectedItem` caches raw pointer. If user deletes item in REAPER → dangling pointer.
- **Fix:** Validate pointer against project item list before comparing.

### B2. DrawFadeBackground freezes UI
- **File:** `waveform_view.cpp:928-953`
- Per-pixel MoveToEx/LineTo for fade tinting = millions of GDI calls on wide fades.
- **Fix:** Use column-based approach (one vertical line per pixel column).

---

## P1 — Should Fix

### B3. Fade handle hit-test wrong Y threshold
- **File:** `edit_view.cpp:873,881`
- `y < m_waveformRect.top + m_waveformRect.bottom / 2` — missing parentheses.
- **Fix:** `y < (m_waveformRect.top + m_waveformRect.bottom) / 2`

### B4. Keyboard accelerator eats ALL keystrokes
- **File:** `main.cpp:108-113`
- EditView swallows every key when focused, breaking REAPER global shortcuts.
- **Fix:** Only return 1 for keys EditView actually handles.

### B5. WAV dataSize integer overflow
- **File:** `audio_engine.cpp:148`
- `uint32_t dataSize` overflows for files > 4GB.
- **Fix:** Check size fits in uint32_t, refuse with error if not.

### B6. m_renderValid data race
- **File:** `spectral_view.cpp:197`
- Plain bool written from background thread, read from main thread.
- **Fix:** Make `std::atomic<bool>` or only write from main thread.

### B7. Hann window init not thread-safe
- **File:** `spectral_view.cpp:17-24`
- Plain bool guard, no synchronization.
- **Fix:** Use `std::call_once`.

### B8. Scrollbar drag missing InvalidateRect
- **File:** `edit_view.cpp:934-949`
- **Fix:** Add InvalidateRect after scrollbar drag ends.

### L1. Context menu submenus may leak on SWELL
- **File:** `edit_view.cpp:1183-1255`
- SWELL may not auto-destroy submenus.
- **Fix:** Explicitly DestroyMenu each submenu.

### P2. GDI objects created/destroyed every paint (30fps)
- **Files:** All draw functions
- Dozens of HPEN/HBRUSH/HFONT created per frame.
- **Fix:** Cache as member variables, create once on theme change.

### P3. Full audio copy for spectral thread
- **File:** `spectral_view.cpp:138`
- Copies entire buffer (potentially 50MB+) every recompute.
- **Fix:** Use shared_ptr<const vector<double>>.

### P4. stat() called on source file every 33ms
- **File:** `edit_view.cpp:693-698`
- File I/O in paint path.
- **Fix:** Cache file size on item load.

### P6. 5 REAPER API calls per paint for fade info
- **File:** `waveform_view.cpp:467-472`
- **Fix:** Cache values in timer, pass to paint.

### Q1. Magic numbers everywhere
- REAPER action IDs, RGB colors, pixel offsets.
- **Fix:** Named constants, route colors through theme.

### M1. No WAV-64/RF64 for files > 4GB
- **File:** `audio_engine.cpp`
- **Fix:** Detect and refuse, or implement RF64.

### M2. Non-WAV destructive editing corrupts files
- **File:** `audio_engine.cpp`
- Writing WAV header to .flac/.mp3/.aiff path = corruption.
- **Fix:** Block destructive editing on non-WAV or use REAPER PCM_source.

### M3. No dirty/modified indicator
- **Fix:** Visual indicator after destructive edits.

---

## P2 — Nice to Have

### Dead Code
- **D1.** LICE API imported but never used (`main.cpp:79-84`, `globals.h/cpp`)
- **D2.** `SpectralView::EnableOnItem()` is empty no-op
- **D3.** `g_GetMediaItemTake_Peaks` imported but never called
- **D4.** Toolbar invisible (TOOLBAR_HEIGHT=0) but code still runs
- **D5.** `DrawRegions` declared but never defined (`edit_view.h:89`)
- **D6.** Duplicate API: `g_GetMediaItemTrack` vs `g_GetMediaItem_Track`

### Quality
- **Q3.** config.h static constants → use `inline constexpr`
- **Q4.** `system("open URL")` → use NSWorkspace or SWELL
- **Q5.** Internal undo buffer unreachable (REAPER undo always fires first)
- **Q6.** DrawRuler has no explicit font

### Other
- **B9.** Negative time in ruler produces garbage labels
- **L2/L3.** DrawRuler and LevelsPanel don't manage fonts
- **P5.** UpdatePeaks misses true peaks when very zoomed out
- **M4.** SpectralView won't compile on Windows (SWELL-only APIs)

---

## Summary

| Priority | Count |
|----------|-------|
| P0 | 2 |
| P1 | 14 |
| P2 | 14 |
| **Total** | **30** |
