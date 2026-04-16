# Changelog

All notable changes to SneakPeak will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/), and this project adheres to [Semantic Versioning](https://semver.org/).

## [2.0.0-alpha.12] - 2026-04-16

### Added
- **Gate in dynamics engine** - Noise gate integrated into the compressor for breath reduction in speech/podcast audio. Gate operates on post-compression+makeup level, catching quiet sounds (breaths) that were boosted by compression. Three new sliders in dynamics panel: G.Thr (gate threshold, -60 to 0 dB, default -100 = off), G.Range (max reduction, -40 to 0 dB, default -20 for natural sound), G.Hold (hold time, 0-200ms, default 50ms prevents syllable-level chattering). Gate threshold shown as dim red horizontal line on waveform. GR shading and purple curve include gate reduction. Based on research of ZamGate, x42/darc, Airwindows SoftGate, Faust compressors library.
- **Envelope editing in timeline/SET modes** - All envelope editing operations now correctly use per-segment envelope lookup (GetEnvelopeAtTime) instead of first segment only. Fixed 12 code paths: point add/drag/delete, freehand drawing, rectangle selection, keyboard shortcuts (Delete/E), context menu shape/delete, A/B bypass (toggles all segments), Apply Dynamics (groups points per segment with segment-relative times), and auto-refresh (aggregates all segments).
- **Multi-item to Timeline transition** - Click on "MULTI" label in mode bar shows dropdown menu with Mix/Layered modes and "Timeline View" option. Also available via context menu View > "Switch to Timeline View". Opening Dynamics Panel in multi-item mode auto-switches to timeline view.
- **Dynamics curves hidden in multi-item view** - Dynamics curves no longer render in multi-item view where they would be meaningless (composite audio, no single envelope to write to).

### Changed
- **Dynamics panel layout** - 10 sliders in 5 rows (was 7 in 4 rows). New row 3: L.ahead + G.Thr. New row 4: G.Range + G.Hold. Panel height 148px (was 130px).
- **Compressor + Gate in single pass** - ComputeCompression now runs two passes: first computes compressor GR + auto-makeup, then applies gate on post-compression level. Gate has independent smoothing (2ms attack / 100ms release, hardcoded) and hold timer.

---

## [2.0.0-alpha.11] - 2026-04-16

### Added
- **Dense envelope point interaction** - After Apply Dynamics creates >100 envelope points, Cmd+drag on the waveform draws a reveal rectangle. Points within the rectangle become visible (4px circles) and fully interactive - drag, delete, double-click delete, Shift+click multi-select. The reveal range is time-based (survives zoom/scroll) and clears on click outside it. Subtle hatched background marks the revealed zone.
- **Gain reduction shading** - Semi-transparent fill between the orange (original amplitude) and purple (compressed) curves, showing where and how much compression is reducing gain. Uses warm red tint with 50% opacity (alternating columns). Toggle with [GR] button in dynamics panel.
- **A/B envelope bypass** - New [A/B] toggle in dynamics panel for audition comparison. When active (yellow), disables the volume envelope in REAPER (via GetSetEnvelopeInfo_String ACTIVE flag) so you hear the audio without compression. Waveform also renders without envelope gain. Bypass auto-restored when panel is closed.
- **[GR] toggle** - Show/hide gain reduction shading between dynamics curves. Warm red color (RGB 180,60,40) when active.

### Fixed
- **[Env] toggle no longer changes waveform shape** - Previously, toggling envelope visibility also removed envGain from the waveform rendering, making it look like the original audio. Now [Env] only hides the cyan envelope curve - the waveform always reflects the actual envelope effect (what you hear). Use A/B for before/after comparison.
- **Apply button disabled during Live mode** - Apply is grayed out when Live is active since envelope points are already written in real-time. Prevents confusion about redundant action.
- **Peak/RMS toggle triggers immediate re-analysis** - Switching detection mode now instantly recalculates dynamics curves, GR meter, and (if Live) envelope points. Previously only triggered during slider drag.

### Changed
- **Dynamics panel bottom row** - Now shows: [Dyn] [Env] [Live] [GR] [A/B] ... [Peak/RMS] ... [Apply]
- **New REAPER API imports** - SetEnvelopeStateChunk, GetSetEnvelopeInfo_String (15 total envelope functions)

---

## [2.0.0-alpha.10] - 2026-04-16

### Added
- **Gain-smoothing compressor model** - Industry-standard architecture: gain computer sees raw peaks instantly, attack/release smooth the gain reduction signal (not the input level). Matches FabFilter Pro-C, Waves, UAD, REAPER ReaComp. More predictable compression amounts, more musical feel. Previously used level-detection smoothing where attack/release smoothed the input level before gain computation.
- **Lookahead slider** - New L.ahead parameter (0-20ms) in the dynamics panel. Scans ahead in the audio buffer for upcoming transients so the compressor starts reducing gain before the peak arrives. Allows slow attack (musical feel) while still catching transients. Zero latency cost in offline processing.
- **Live mode** - New [Live] toggle in dynamics panel (green when active). When enabled, envelope points are written to REAPER in real-time as you drag sliders - the waveform updates instantly, no Apply needed. Single undo block per drag gesture (Cmd+Z reverts entire adjustment). When disabled, original Apply workflow.
- **Dyn/Env visibility toggles** - Two toggle buttons in the dynamics panel bottom row: [Dyn] (orange) shows/hides dynamics curves (amplitude, compression, threshold), [Env] (cyan) shows/hides volume envelope overlay. Quick clean waveform preview without closing the panel.
- **Slider fine mode** - Hold Cmd/Ctrl while dragging dynamics panel sliders for 1/5th sensitivity. Delta-based from drag start value for precise adjustments on short slider tracks.
- **Slider grab offset** - Clicking near a slider thumb no longer jumps the value. Drag starts from current position with relative offset tracking.

### Fixed
- **Threshold line aligns with dB scale** - Yellow threshold line at -18dB now lands exactly on the -18dB grid line. All dynamics curves (orange amplitude, purple compression, yellow threshold) use the same amplitude-based Y mapping as the waveform: `y = centerY - pow(10, dB/20) * halfH`. Previously used linear-in-dB normalization which gave completely different Y positions.
- **Apply Dynamics non-destructive** - Each Apply now clears existing envelope points in the time range (via DeleteEnvelopePointRange) before writing new ones, with guard points at boundaries to prevent discontinuity. Repeated Apply is idempotent - same settings always produce the same result. Previously, points accumulated on each Apply, stacking compression.
- **Auto makeup gain accuracy** - Average gain reduction now computed from compressed points only (where GR < -0.01 dB), no longer diluted by silence/uncompressed passages. Makes auto-makeup properly compensate for actual loudness reduction.
- **Dynamics curve zoom consistency** - At zoomed-out views, dynamics curves now show max-peak-per-stride (matching how the waveform takes max peak per column). Previously used arbitrary stride sampling which created misleading phantom spikes that disappeared when zoomed in.
- **Dynamics hidden when no item** - Dynamics curves no longer render over the empty "Select a media item" state. Previously, stale curves from a previous item remained visible after deselection.

### Changed
- **DynamicsParams struct rewritten** - Now uses standard compressor parameters (threshold dB, ratio, knee dB, attack ms, release ms, makeup dB, RMS mode) instead of percentage-based compression.
- **Apply Dynamics opens panel** - Context menu "Apply Dynamics..." now opens the inline panel instead of a modal dialog.
- **Dynamics panel layout** - 7 sliders in 4 rows (left: Thresh/Ratio/Knee/L.ahead, right: Attack/Release/Makeup). Dyn/Env/Live toggles in bottom-left. Peak/RMS and Apply right-aligned. Panel height 130px.
- **Compressor model** - Switched from level-detection smoothing to gain smoothing. Attack/release now control how fast compression fades in/out (not how fast the detector responds).

### Performance
- **DrawDynamicsCurve optimized** - Binary search for visible range + max-peak-per-stride + same-pixel deduplication reduces drawn points from 60000 to ~600 per frame.
- **RDP curve simplification** - Ramer-Douglas-Peucker algorithm reduces envelope points from 60000 to 200-500 when applying dynamics to REAPER envelope. Prevents performance issues with large point counts.
- **Adaptive envelope point rendering** - Points hidden when >100 visible, 2px when 30-100, 4px when <30. Prevents visual clutter at high zoom-out.
- **Silence floor** - Dynamics curves clamped at -45dB floor, preventing visual noise from near-silence regions.
- **Muted overlay colors** - Dynamics curve colors toned down for better visual hierarchy with waveform content.

---

## [2.0.0-alpha.9] - 2026-04-16

### Added
- **Dynamics control panel** - Inline floating panel replaces the modal GetUserInputs dialog. Six real-time sliders: Threshold, Ratio, Knee, Attack, Release, Makeup. Dragging any slider instantly updates the compression preview curve on the waveform. Apply button writes envelope points with toast feedback.
- **Standard compressor math** - Professional compression engine with ratio + threshold + soft knee curve, replacing the old percentage-based Above/Below system. Matches standard compressor behavior.
- **Auto makeup gain** - Automatic gain compensation based on average gain reduction, keeping perceived loudness consistent after compression.
- **RMS detection mode** - Toggle between Peak and RMS detection in the dynamics panel. RMS provides smoother, less aggressive compression curves.
- **GR meter bar** - Gain reduction meter in the dynamics panel title bar, showing real-time compression depth.
- **Compression preview curve** - Purple curve overlay showing post-compression levels alongside the original dynamics curve (orange). Visual before/after comparison.
- **Envelope fader-scale Y mapping** - Envelope display now uses REAPER's native ScaleToEnvelopeMode/ScaleFromEnvelopeMode for Y axis mapping, achieving 1:1 visual match with REAPER's arrange view. Reads MAXVAL from envelope state chunk for correct display range.

---

## [1.9.0] - 2026-04-04

### Added
- **Non-destructive paste** - Cmd+V now creates a new REAPER item at cursor position instead of modifying audio files on disk. Splits the item at cursor, ripples subsequent items right, inserts pasted audio in the gap. Works in single-item, timeline, and SET modes. Standalone mode retains destructive paste.
- **Bars & Beats ruler** - New ruler mode synced with REAPER's tempo map. Shows measure numbers at major ticks, beat subdivisions at minor ticks. Handles tempo changes and time signature changes. Three ruler modes: Relative Time, Absolute Time, Bars & Beats (context menu > View).
- **Ripple Delete** - Shift+Delete or Shift+E removes selection and shifts subsequent items left to close the gap. Standard Delete (no Shift) preserves gaps. Available in context menu: Edit > Ripple Delete.
- **Arrow key segment navigation** - Option+Left/Right navigates between segments in timeline/SET/multi-item views. Selects the target segment, scrolls to show it, syncs cursor to REAPER. During playback, automatically jumps to the new segment.
- **Arrow key gain adjustment** - Up/Down arrows adjust gain +/-1 dB on the current item or batch. Visual feedback in all view modes.
- **Horizontal trackpad scroll** - Two-finger horizontal swipe on macOS trackpad pans the waveform (WM_MOUSEHWHEEL support).
- **Pinch to zoom** - Trackpad pinch gesture zooms the waveform horizontally, centered on cursor position. Sensitivity dampened to 15% for smooth feel.
- **Gain knob relative indicator** - Batch mode (SET/timeline/multi-item) shows gold-colored "+0.0 dB rel" label to distinguish from absolute single-item mode (blue).

### Fixed
- **Scroll modifier detection** - All modifiers now use GetAsyncKeyState instead of SWELL wParam flags (which are always zero on macOS). Cmd+Scroll pans, Option+Scroll zooms vertically.
- **SET mode only includes selected items** - Previously, pressing T with items 1 and 3 selected (but not 2) included all three. Now stores explicit item pointers. Working set refreshes validate pointers after split/delete.
- **Track follow respects explicit selection** - During playback, track follow no longer overrides a user-selected item. Only activates when no item is selected or selection doesn't match displayed item.
- **Fade preservation after gain+selection** - Fade-in/out params saved before split, re-applied to outermost surviving items. Works in both single-item and timeline gain paths.
- **Gain double-apply eliminated** - After split+reload in single-item gain path, db is zeroed to prevent ScaleAudioRange from re-applying gain already baked into freshly loaded audio.
- **Timeline view rebuild after gain** - Both selection and no-selection gain paths rebuild timeline from track items, ensuring segments are up-to-date after splits.
- **SET mode items refresh** - After gain or delete operations, working set items list is rebuilt from the track to include new split fragments.
- **Ripple delete view clamp** - View duration always clamped to content after delete (both single-item and timeline paths). Fixes silence gap at end after ripple delete.
- **Space plays from cursor** - When cursor is outside the selection, Space plays from cursor position. When inside, plays the selection. Allows previewing audio before/after a selection.
- **Cut is now ripple** - Cmd+X (Cut) uses ripple delete by default, matching standard waveform editor behavior (removes selection and closes gap).
- **Knob drag race condition** - skipBatchWrite set immediately on knob drag start, preventing first-frame D_VOL write to whole item when selection is active.
- **Context menu fade alignment** - Fade-out from menu spans from selection start to item end. Fade-in spans from item start to selection end.
- **Segment navigation scroll clamp** - View cannot scroll past audio content when navigating to last segment.
- **Pasted item waveform** - UpdateItemInProject + Build Missing Peaks ensures waveform appears immediately in REAPER arrange.

### Performance
- **Full-selection gain optimization** - When selection covers the entire timeline, uses direct D_VOL path (no split). Avoids floating-point edge cases and unnecessary item fragmentation.

## [1.8.0] - 2026-03-27

### Added
- **Timeline View** - After cutting a section from an item, SneakPeak now shows all surviving fragments with gaps preserved (1:1 with REAPER timeline). Dark background marks gap regions. Continues working through repeated cuts with zoom preserved.
- **Dock/Undock control** - Window starts floating by default (resizable). Context menu: "Dock SneakPeak in Docker" / "Undock SneakPeak". Floating position and size remembered across sessions.
- **Option+click segment snap** - In SET, timeline, and multi-item views, Option+click on a segment instantly selects its full range. Enables quick re-selection of previously split fragments for gain adjustment without new splits.
- **Selection-aware gain in all views** - Gain knob with selection now works consistently across REAPER view (split + D_VOL), timeline view, multi-item view, and SET mode. Live visual preview on selection range only.
- **Multi-item gap visualization** - Dark gap regions between items in multi-item MIX mode, same as timeline view.
- **Multi-item editing** - Delete and gain operations work across segments in multi-item view.
- **Drag export without Alt** - Drag a selection outside the SneakPeak window to export to REAPER timeline. Alt+drag still works for immediate export to Finder/external apps.
- **Split at Cursor in context menu** - Edit > Split at Cursor (S) added to context menu.

### Fixed
- **Dock scroll propagation** - Mouse wheel in docked SneakPeak no longer scrolls REAPER arrangement.
- **T key macOS beep** - T key intercepted by accelerator hook before REAPER processes it.
- **Fade real-time sync** - Bidirectional: fades changed in REAPER update SneakPeak instantly, fades dragged in SneakPeak update REAPER timeline in real-time (removed PreventUIRefresh from fade drag).
- **Fade handles always visible** - Grab zones shown even with zero fade length, enabling creation of new fades from item edges.
- **Fade-out targets correct item** - In multi-item/SET mode, fade-out handle reads/writes to last segment item (not first).
- **Fade-in/out clamped to segment** - Fade length limited to segment duration in multi-item mode.
- **Fade-in and fade-out block each other** - Fades stop at meeting point in both REAPER and standalone modes.
- **Volume mismatch REAPER to SET** - Fixed double D_VOL application when SET has single segment.
- **Item length change detection** - External item length changes in REAPER properly reload audio and clamp view. Position-only changes skip reload (fixes lag when dragging items).
- **Repeated gain without split accumulation** - EDGE_EPS detects pre-existing boundaries from previous gain operations. Crossfade only applied at fresh split points.
- **Delete at item start/end** - Edge case handling for selections covering item beginning or end.
- **View preserved after delete** - Zoom position maintained after cut operations.
- **Gain flash eliminated** - UpdateFadeCache called immediately after batchGainOffset reset, preventing one-frame stale cache.
- **Gain preview matches result** - standaloneGain used only for selection range (prevents double-apply with batchGainOffset).
- **Selection preserved after gain** - Selection stays active after gain knob release in all view modes.
- **Undo refreshes timeline view** - Ctrl+Z properly rebuilds timeline view segments.
- **Docker close + reopen** - Window properly recreated when toggled after docker tab closed.
- **Toggle action state** - Icon correctly reflects visibility (IsPendingClose check).

### Performance
- **Instant gain in timeline view** - ScaleAudioBuffer/ScaleAudioRange modifies audio in-place instead of full reload.
- **No audio reload on item position change** - Only length changes trigger reload (eliminates lag during item dragging).

## [1.7.0] - 2026-03-26

### Added
- **Working Set mode** - Select items on timeline, press T (or use REAPER action "SneakPeak: Toggle Track View") to lock them as a persistent editing set. Gaps collapsed into continuous waveform. Click elsewhere and come back - the set auto-restores. Exit with T again or ESC.
- **Selection-aware gain** - Gain knob respects selection: with selection, applies gain only to the selected fragment (split + D_VOL with 10ms crossfade overlap in SET mode, destructive with crossfade in standalone). Without selection, applies to whole item.
- **Clipping visualization** - Waveform peaks above 0dB now draw in red (top 30% of peak height), visible at any zoom level. No more clamping at 0dB.
- **Group Set Items** - Group all items in the working set (or just selected range) for easy timeline manipulation. Toggle via context menu with checkmark. Visual indicator: colored bar below ruler.
- **Ruler time format toggle** - Switch between relative and absolute REAPER timeline time via context menu (View > Ruler: Absolute Time). Persisted across sessions. Auto-enables in SET mode.
- **REAPER action: Toggle Track View** - Registered as assignable REAPER action for keyboard shortcut binding.
- **Bidirectional cursor sync** - Click on REAPER timeline updates SneakPeak playhead. Click in SneakPeak scrolls REAPER arrange view to that position.

### Changed
- **Modular architecture** - Monolithic `edit_view.cpp` (4,336 lines) split into 7 focused modules: rendering, input handling, audio commands, standalone file management, context menu, drag export. No file exceeds 1,100 lines.
- **Waveform rendering** split from data management (waveform_view.cpp - waveform_view.cpp + waveform_rendering.cpp).
- **Drag export requires Alt/Option** - Prevents accidental drags during selection. Hold Alt/Option + drag from selection to export.

### Fixed
- **Memory leak** - `RefreshItemSource` now uses `P_SOURCE` via `GetSetMediaItemTakeInfo` instead of deprecated `SetMediaItemTake_Source` which leaked the old PCM source on every destructive edit.
- **WriteAndRefresh** now checks write success before marking item as dirty.
- **Marker manager** - `m_showMarkers` and `m_rightClickMarkerIdx` properly encapsulated (private with accessors).
- **Selection edges clamp to waveform area** - Selection lines and highlight no longer bleed onto the dB scale.
- **Selection sync at item boundaries** - Selection dragged to item edges no longer reverses on the REAPER timeline.

### Performance
- **PreventUIRefresh** - All REAPER undo blocks wrapped with `PreventUIRefresh(1)/-1)` to prevent redundant arrange view redraws during multi-step operations.
- **Toolbar font caching** - Font created once via theme system instead of per-frame `CreateFont`/`DeleteObject`.
- **Deduplicated fade parameters** - `GetActiveFadeParams()` replaces 3 identical 12-line blocks in waveform rendering.
- **Deduplicated meter ballistics** - `GetBallistics()` replaces 2 identical switch blocks with C++17 structured bindings.

---

## [1.6.0] - 2026-03-25

### Added
- **Save As (Ctrl+Shift+S)** - Save standalone files to a new location via file dialog.
- **Continuous fade curvature** - Vertical drag on fade handles now controls smooth curvature (REAPER's D_FADEINDIR/D_FADEOUTDIR, -1..1) instead of cycling through preset shapes. Matches REAPER's native fade behavior.
- **Standalone waveform fade preview** - Waveform visually reacts to fade changes in real-time in standalone mode.

### Changed
- **Smart Save (Ctrl+S)** - First save on WAV asks to overwrite or Save As. First save on MP3/FLAC auto-creates `[name]_edit.wav` next to original (24-bit PCM). Subsequent saves overwrite silently. Save state persisted across tab switches.
- **Drag to timeline** - Clean files drag original path (zero copies). Dirty files auto-save first. Selections export as `[name]_sel_HHMMSS.wav`. Files saved to project recording folder when available, next to source file otherwise.
- **Fade handle UX** - Hit zone increased from 8px to 16px, handle size 10x10px. Curvature range matched to REAPER's visual depth. Fade-in direction corrected to match REAPER timeline.

### Fixed
- **Fade-in direction** - Fade-in curvature now renders identically to REAPER's timeline display (was inverted).
- **Drag export file loss** - Exported files no longer go to /tmp where they'd be lost on restart. Saved to project folder or next to source file.
- **GitHub issue #1** - "Weird fade handle behavior" - fade handles now responsive with proper curvature control.

---

## [1.5.0] - 2026-03-12

### Added
- **Master meter mode** - Click the MASTER tab in the mode bar to monitor master track output with a real-time rolling peak waveform (L channel up, R channel down from center) and level meters.
- **Clipping indicator** - Master waveform turns red above 0dB for instant visual feedback on clipping.
- **dB scale in master view** - Familiar dB scale column on the right side of the master waveform.
- **Master Output meter source** - Right-click the meter panel and enable "Master Output" to read meters from the master track while viewing item/multi-item waveforms. Persisted across sessions.
- **S shortcut** - Split item at cursor position.

### Fixed
- **Meter accuracy** - Take volume (D_VOL) now included in level calculation alongside item volume for correct readings.
- **Multi-item volume tracking** - Volume changes on REAPER timeline now detected and auto-reloaded (~1s polling).
- **Meter sync** - Latency-compensated playback position (GetPlayPosition) used for meter timing instead of buffer position.
- **SWELL menu checkmarks** - Fixed MF_CHECKED causing grayed-out menu items on macOS (SWELL bug workaround).
- **Docker close crash** - Fixed crash when closing docked window on macOS with deferred destruction.

### Changed
- **GDI caching for meters** - Pre-created brushes and pens reused per frame (0 allocations per frame instead of ~12-15).
- **Master meter ballistics** - Mode-dependent attack/decay matching item meter behavior (Peak: instant attack + slow decay, VU: sluggish symmetric, RMS: fast decay).

---

## [1.4.0] - 2026-03-11

### Added
- **Meter mode selection** - Right-click the bottom meter panel to switch between Peak (PPM), RMS (AES/EBU 300ms integration), and VU metering modes. Each mode has distinct attack/decay ballistics and visual feedback. Default is Peak.
- **Multi-item Mix/Layered view modes** - Select multiple items in REAPER and view them together:
  - **Mix (Sum)** - all items summed into a single waveform on an absolute timeline.
  - **Layered (per Item)** - each item in a distinct color (8-color palette) with transparency.
  - **Layered (per Track)** - items colored by their parent track.
- **Crossfade join indicators** - Multi-item view shows join-point lines at crossfade midpoints for easy visual reference of item transitions.
- **Batch gain control** - One gain knob adjusts relative gain across all selected items in multi-item mode.
- **Per-track layered mode** - Items colored by their parent track for track-aware visualization in multi-item view.

### Changed
- **RMS integration window** - Increased from 50ms to 300ms per AES/EBU standard for accurate RMS readings matching professional meters.
- **Optimized layered drawing** - Column range clipping per layer, pre-created GDI pens, merged peak+RMS into single pass. Handles 20+ items across 10+ tracks without lag.

### Fixed
- **Undo menu state** - Undo option always enabled in REAPER mode (REAPER manages its own undo stack). Standalone mode correctly tracks undo availability.
- **Compiler warnings** - Zero warnings in Release build (unused variables in debug logging wrapped in `#ifdef SNEAKPEAK_DEBUG`).

---

## [1.3.0] - 2026-03-09

### Added
- **GDI resource caching** - Pen creation moved out of draw loops, incremental time stepping in waveform rendering.
- **Safety fixes** - Bounds checks in levels panel and mono downmix, minimap pen restore, spectral mutex, off-by-one fixes.
- **Config constants** - Magic numbers extracted to `config.h` (EDGE_ZONE, PLAY_GRACE_TICKS, ZERO_SNAP_RANGE).

### Changed
- **UpdateSoloState()** moved out of paint into OnTimer() for cleaner separation.
- **Dead code cleanup** - Removed empty blocks, unused variables, consolidated debug logging.

---

## [1.2.0] - 2026-03-07

### Added
- **Spectral view** - Async FFT spectrogram (2048-point, magma colormap) with frequency band selection (Alt+drag).
- **Minimap** - Resizable overview bar with click-to-navigate and drag-to-scroll.
- **Gain panel** - Interactive gain knob with fine-adjust mode (Cmd+drag), double-click to reset.
- **Solo button** - Track solo-in-place toggle in the waveform header.
- **Drag & drop export** - Drag a selection from the waveform to export as temp WAV.
- **Multi-item concatenated view** - Select multiple items and view them as a continuous waveform.
- **Track follow during playback** - Auto-switches to the item on the currently playing track.

---

## [1.1.0] - 2026-03-04

### Added
- **Standalone file mode** - Drag & drop WAV files for offline editing with independent undo stacks.
- **Multiple file tabs** - Up to 8 standalone files open simultaneously.
- **Fade shapes** - 7 curve types (linear, fast/slow start, steep, S-curve) for fade-in and fade-out.
- **LUFS normalization** - Normalize to -14 LUFS or -16 LUFS via REAPER's loudness analysis.
- **DC offset removal** - One-click DC bias correction.
- **Markers** - Add, edit, delete, drag markers. Tab/Shift+Tab navigation.
- **Context menu** - Full right-click menu with Edit, Process, Markers, and View submenus.

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
