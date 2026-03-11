# Changelog

All notable changes to SneakPeak will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/), and this project adheres to [Semantic Versioning](https://semver.org/).

## [1.4.0] - 2026-03-11

### Added
- **Meter mode selection** — Right-click the bottom meter panel to switch between Peak (PPM), RMS (AES/EBU 300ms integration), and VU metering modes. Each mode has distinct attack/decay ballistics and visual feedback. Default is Peak.
- **Multi-item Mix/Layered view modes** — Select multiple items in REAPER and view them together:
  - **Mix (Sum)** — all items summed into a single waveform on an absolute timeline.
  - **Layered (per Item)** — each item in a distinct color (8-color palette) with transparency.
  - **Layered (per Track)** — items colored by their parent track.
- **Crossfade join indicators** — Multi-item view shows join-point lines at crossfade midpoints for easy visual reference of item transitions.
- **Batch gain control** — One gain knob adjusts relative gain across all selected items in multi-item mode.
- **Per-track layered mode** — Items colored by their parent track for track-aware visualization in multi-item view.

### Changed
- **RMS integration window** — Increased from 50ms to 300ms per AES/EBU standard for accurate RMS readings matching professional meters.
- **Optimized layered drawing** — Column range clipping per layer, pre-created GDI pens, merged peak+RMS into single pass. Handles 20+ items across 10+ tracks without lag.

### Fixed
- **Undo menu state** — Undo option always enabled in REAPER mode (REAPER manages its own undo stack). Standalone mode correctly tracks undo availability.
- **Compiler warnings** — Zero warnings in Release build (unused variables in debug logging wrapped in `#ifdef SNEAKPEAK_DEBUG`).

---

## [1.3.0] - 2026-03-09

### Added
- **SDK peak rendering (v2.0)** — Waveform display powered by REAPER's `GetMediaItemTake_Peaks` API. Zero RAM usage for waveform display in REAPER mode.
- **Lazy audio loading** — Audio data loaded on-demand only for destructive operations, spectral analysis, clipboard, and export. Massive memory savings for large files.
- **Dual-mode rendering** — SDK peaks for REAPER single-item, legacy full-audio for standalone/multi-item/old REAPER.
- **Minimap SDK peaks** — Minimap uses SDK peak rendering with graceful fallback.
- **Levels meter graceful degradation** — Meters work without loaded audio (view-based estimation).
- **Snap graceful degradation** — Snap to zero-crossing works with SDK peaks when possible.

---

## [1.2.0] - 2026-03-07

### Added
- **Spectral view** — Async FFT spectrogram (2048-point, magma colormap) with frequency band selection (Alt+drag).
- **Minimap** — Resizable overview bar with click-to-navigate and drag-to-scroll.
- **Gain panel** — Interactive gain knob with fine-adjust mode (Cmd+drag), double-click to reset.
- **Solo button** — Track solo-in-place toggle in the waveform header.
- **Drag & drop export** — Drag a selection from the waveform to export as temp WAV.
- **Multi-item concatenated view** — Select multiple items and view them as a continuous waveform.
- **Track follow during playback** — Auto-switches to the item on the currently playing track.

---

## [1.1.0] - 2026-03-04

### Added
- **Standalone file mode** — Drag & drop WAV files for offline editing with independent undo stacks.
- **Multiple file tabs** — Up to 8 standalone files open simultaneously.
- **Fade shapes** — 7 curve types (linear, fast/slow start, steep, S-curve) for fade-in and fade-out.
- **LUFS normalization** — Normalize to -14 LUFS or -16 LUFS via REAPER's loudness analysis.
- **DC offset removal** — One-click DC bias correction.
- **Markers** — Add, edit, delete, drag markers. Tab/Shift+Tab navigation.
- **Context menu** — Full right-click menu with Edit, Process, Markers, and View submenus.

---

## [1.0.0] - 2026-03-01

### Added
- Native C++ REAPER extension (no script dependencies)
- Dockable window with double-buffered GDI rendering
- Precision waveform display with peak + RMS
- Click-and-drag selection, shift+click extend, double-click select all
- Horizontal and vertical zoom with scroll wheel
- Toolbar with zoom, transport, and processing buttons
- Cut / Copy / Paste / Delete / Silence
- Normalize (peak)
- Reverse selection or full item
- Playback from cursor or selection with playhead follow
- dB scale with grid lines
- Channel mute buttons (L/R)
- Mono downmix toggle
- Selection time display (HH:MM:SS.mmm)
- Format info panel (sample rate, bit depth, channels, file size)
- Auto-follow item selection in REAPER
- Persistent settings via REAPER ExtState
- Cross-platform architecture via WDL/SWELL
