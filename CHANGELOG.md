# Changelog

All notable changes to SneakPeak will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/), and this project adheres to [Semantic Versioning](https://semver.org/).

## [Unreleased] - v2.3.0 "Dynamics Suite" (in development)

### Added
- **Upward compression** - a DOWN/UP switch in the Dynamics panel header flips the whole processor: Up mode boosts quiet material toward the threshold instead of reducing loud material (leveling quiet speech, OTT-style lift). A mandatory **M.Boost** cap (0-24 dB, default 8) keeps the noise floor bounded, and with the gate enabled the boost is floored at the gate threshold - the gate always wins, so gated noise is never pumped up. The GR readout and meter turn amber and read "BOOST +x.x dB" when lifting. New **Upward Leveling** preset (gate on, RMS, gentle 2:1). Auto-makeup defaults off in Up mode (it acts as a downward trim there).
- **Extended Ratio to Inf:1 and beyond** (requested by saxmand) - the Ratio knob now sweeps 1:1-20:1 as before, then approaches **Inf:1** (true limiting, with a detent) and continues into an **over-compression** zone (negative ratios: reduction exceeds the overshoot, so loud input lands *below* the threshold). Auto-makeup is capped in the extended zone.
- **Envelope ceiling honesty** - if an applied curve (typically an Up-mode boost) exceeds the take volume envelope's range (+6 dB by default), the points are clamped at the ceiling and the Apply toast says how many and at what level, instead of REAPER clamping silently.
- **Bipolar Makeup (-24 to +24 dB)** - manual makeup can now trim downward, the useful direction after an Up-mode boost (auto-makeup already trims automatically there, and its readout now shows the signed value).
- **Per-stage bypass** (requested by sguyader) - small power dots inside the COMP and GATE tab buttons let you audition each stage independently in real time (amber = active, grey = bypassed), complementing the whole-chain A/B. Bypassing the compressor also mutes its makeup; bypassing the gate also lifts the Up-mode boost floor. Bypasses are audition-only: they never save into items or presets and reset when the panel opens.
- **BOTH mode (leveler)** - the mode button cycles DOWN, UP, BOTH: in BOTH the compressor reduces above the threshold AND boosts below it in a single pass, pulling everything toward the threshold (the classic "lift the quiet, tame the loud" voice leveler). The boost side keeps the M.Boost cap and the gate floor, and in BOTH the Knee knob sets the gentle "leave-alone" band around the target - natural dynamics inside the band stay untouched, correction ramps up smoothly outside it.
- **Full noise-gate rework** (forum #67) - the fixed 2:1 gate is now a complete downward expander: **G.Ratio** (1-10:1, 2:1 reproduces the old behavior exactly), **G.Hyst** hysteresis (the gate stays open until the level falls this far below the threshold - no more chatter on material hovering at the threshold; shown as a violet band on the transfer plot), exposed **G.Att / G.Rel** open/close speeds, and deeper floors: G.Thr down to **-90 dB**, G.Range down to **-80 dB**, G.Hold up to 500 ms.
- **G.Thr Off detent** - drag the gate threshold fully left to switch the gate off (readout shows "Off"); Cmd/Ctrl-click still works as a shortcut.
- **-96 dB transfer-plot floor** with auto-expand - if you push the gate threshold below the current plot floor, the plot rescales automatically (with a toast) so the draggable gate node never disappears off the plot edge.
- **De-esser (new DE-ESS tab)** - a wideband de-esser for taming sibilance in speech and vocals (forum #71). A band-filtered detector (band-pass with Freq/Width, or a 24 dB/oct high-pass) measures the sibilance band and drives a third gain-reduction stage alongside the compressor and gate, with its own Threshold/Ratio/Attack/Release and a hard **D.Range** reduction cap (default -10 dB) so ducking stays polite. **LISTEN** paints an amber lane over every span the de-esser bites - the quickest way to spot false triggers before applying. Enabled via the power dot in the DE-ESS tab button; saved per item like the other parameters; new **De-Ess Vocal** preset. Honest note: this is the classic wideband topology (the whole signal ducks briefly, not just the band) - a split-band destructive mode is on the roadmap. Offline analysis means the 1 ms attack is artifact-free.
- **Preset update** - Voice/Podcast, Broadcast and De-breath now ship with -6 dB gate hysteresis (smoother gating on breathy material). Saved per-item settings are unaffected.
- **Curved envelopes: editable curvature** (forum #51, Khron Studio) - hold Alt/Option and drag vertically on an envelope segment to bend it (bezier tension -1..+1, live value readout at the cursor; Cmd while dragging = fine mode). A linear segment promotes itself to Bezier on the first curvature drag - no menu trip needed - and "Reset curvature" in the point's right-click menu returns it to the neutral curve. The curve renders identically in SneakPeak and the REAPER arrange view. Note: within the envelope-line grab zone Alt now means curvature; elsewhere Alt+drag keeps its existing meanings (drag export, snap to segment).
- **Redo** (Ctrl+Shift+Z or Ctrl+Y, plus Edit > Redo) - Standalone mode gets a real redo stack (per tab, cleared by a new edit, like every wave editor); in ITEM mode the shortcut triggers REAPER's native redo and refreshes the view. Undo/redo in Standalone now also refresh the spectrogram.
- **Spectral marquee selection** - dragging on the spectrogram now selects a time x frequency rectangle in one gesture (dashed outline with a frosted interior), the standard spectral-editor selection and the target for Spectral Repair. Grab any edge or corner of the rectangle to fine-tune it (resize cursors mark the grab zones). Alt+drag still selects a full-width frequency band; Shift+click extends the time axis.
- **Faster, truthful spectrogram** - spectrogram computation is now multithreaded and packs channel pairs into single FFTs (roughly an order of magnitude faster on stereo), and zoomed-out rendering takes the peak over everything a pixel covers instead of sampling every Nth column - clicks and narrow tones no longer vanish at low zoom levels.
- **Spectral Repair (Standalone mode)** - surgically remove unwanted sounds (beeps, squeaks, coughs) straight from the spectrogram: drag a time x frequency rectangle on the spectrogram, then right-click > Process > **Heal Selection**. The selected time x frequency rectangle is rebuilt from the surrounding audio (per-frequency interpolation across the selection); pick Replace (100%) or a gentler Attenuate strength and re-apply iteratively - healing only ever reduces energy, so content quieter than its surroundings is left alone. **Repair Clicks in Selection** removes clicks and pops sample-accurately on a plain time selection (autoregressive detection + interpolation). Both are destructive Standalone edits with undo (Ctrl+Z); heal is limited to 10 s selections, click repair to 4 s. v1 heals horizontally (across time); vertical/pattern modes and direct ITEM-mode repair are planned follow-ups.
- **Waveform style: Detailed / Simple** (forum #83) - a new selector in Settings > View: Detailed (default) keeps the darker RMS band inside the peak waveform; Simple draws a single-colour waveform (peaks only, the same state as the old Show RMS toggle). Clip and over-0dB marking stays in both styles.
- **Hide the ruler** (forum #51, Khron Studio) - a Ruler toggle in Settings > View collapses the time ruler so the waveform gets the extra rows. Markers and regions stay visible (drawn over the waveform); ruler editing gestures need the ruler shown.
- **Faster, finer fades** (forum #51, Khron Studio) - scroll the mouse wheel over a fade handle to nudge its length (5% of the item per notch, Cmd = 1 ms steps), and hold Shift while dragging a handle for a fine 1/4-speed trim (press or release Shift mid-drag freely - the fade edge never jumps).
- **Bindable toolbar shortcuts** (forum #51, Khron Studio) - every toolbar command is now a named REAPER action (SneakPeak: Zoom in/out/to fit/to selection, Play, Stop, Normalize, Fade in/out, Reverse, Vertical zoom in/out/reset): assign any shortcut in REAPER's Action List. Actions run only while the SneakPeak window is open, and keys you bind to them are never swallowed by the editor's own shortcut list.
- **Slip content** (forum #51, Khron Studio) - Alt/Option+drag on the waveform (outside a selection) in plain ITEM mode slides the take's source audio under the item, REAPER's "move item contents" edit without leaving the editor. The arrange updates live with a toast readout of the slip amount, the offset clamps to the source bounds, and each slip is one undo point. v1 scope: single items with non-looped sources; existing Alt gestures (drag export in selection, segment snap in SET/timeline/multi, envelope curvature on the line) keep their meanings.
- **"Did You Know?" guide section** (forum #80/#84, mb945) - the user guide now opens its reference part with the ten most-missed features, headlined by where destructive editing lives (the right-click processing entries and the drop-a-file-onto-the-window Standalone editor).
- Old projects and presets load bit-identically: with the new parameters at their defaults the engine output is byte-for-byte the same as v2.2.0 (verified by an offline envelope-diff regression harness added to the repo).

### Fixed
- **Splitter drag tracks the cursor at every UI scale** - the waveform/spectral splitter no longer drifts away from the cursor while dragging at UI scales other than 100%, or with the meters hidden / minimap shown (the drag used its own unscaled copy of the layout math).
- **Standalone editing no longer eats memory on long files** - undo used to snapshot the ENTIRE audio buffer on every edit (a 30-minute stereo file costs ~1.4 GB per undo step). Bounded edits (Heal, Repair Clicks, Silence on a selection, selection gain) now snapshot only the touched range - megabytes instead of gigabytes - and switching between file tabs no longer copies the audio and undo history back and forth (instant now, regardless of file size). Whole-file operations keep full snapshots; undo/redo behavior is unchanged.
- **Long files no longer freeze the window while loading** - Standalone decoding (WAV/MP3/FLAC) now runs in small timer-driven slices: the title bar shows "Loading ... N%", the interface stays fully responsive and you can keep editing whatever is open; the new tab appears when the file finishes. Short files load exactly as before.
- **Closing an unsaved background tab with "Yes" now really saves it** - the save-before-close prompt only saved the file when the closed tab happened to be the active one; background tabs were silently closed without saving.

## [2.2.0] - 2026-06-17

### Highlights
SneakPeak v2.2.0 is the **UI scaling release**: the entire interface scales from 80% to 200% (the top forum request), a new premium Settings panel becomes the home for preferences, the gain knob, level meters and toasts get the premium rendering treatment, and the waveform now tells the truth about clipping.

### Added
- **Global UI scale (80-200%)** - every part of the interface (fonts, toolbar, mode bar, ruler, scrollbar, meters, panels, hit zones) scales from a single slider. First run auto-detects the system DPI (Windows display scaling, Linux GDK scale); after a manual change your choice is never overridden. On Windows, dragging the floating window across mixed-DPI monitors re-suggests the scale automatically until you set one manually. (Reporters: Rodulf #59, weirpaul #61, X-Raym #63, Illad #66, Stevie #77)
- **Settings panel** - click the gear icon in the mode bar (or right-click > Settings...). UI scale slider with live preview, density presets (Compact / Comfortable / Spacious), Fit to Window, plus the migrated Ruler / Meters / View preferences. The right-click context menu now stays a pure work menu.
- **Premium-rendered gain knob, L/R meters and toasts** - anti-aliased, DPI-crisp rendering with gradient meter bars (-18/-6 dB zones, per-mode shading for Peak/RMS/VU) and zone-colored peak-hold. Toast notifications fade smoothly.
- **Dynamics panel follows the global scale** - the panel multiplies the global UI scale with its own resize grip (capped at 2.4x), so it grows with the rest of the UI and the grip stays a per-panel fine-tune.
- **Truthful clip display** - red now means real clipping in the source samples (flat-topped runs, detected on raw data - including int16 files clipped at positive full-scale, which the old test missed); amber means over 0 dBFS headroom warning in float contexts where nothing actually clips yet. A dark red 0 dBFS reference line appears when zoomed out vertically. (Forum discussion #72-#79, mschnell and Lunar Ladder)
- **Meters show what you hear** - the level meter feed now folds in item fades and the take volume envelope at the latency-compensated play position (A/B bypass respected), so Live dynamics meters as heard.
- **ESC closes the Dynamics panel**; new **D hotkey** toggles it (Stevie #77).
- **New action "SneakPeak: Toggle Master Track View"** - a bindable action for the MASTER output view, same as clicking the mode-bar MASTER tab (X-Raym #63).
- **Wheel-zoom center preference** - choose whether scroll zoom anchors on the mouse position (default) or the edit cursor: Settings > View > Zoom (Ben Zero #83).
- **Middle-mouse drag pans the view** horizontally in the waveform and spectral areas (weirpaul #61).
- **Selection edge resize** - hover a selection edge to get a resize cursor and drag it to adjust the selection (Lunar Ladder #64).

### Fixed
- **Skewed spectral view on Linux (Wayland/arm64)** - the spectrogram wrote rows assuming the framebuffer stride equals the width, but Linux SWELL pads the stride, shearing the image diagonally at roughly half of all window widths. Same latent bug fixed in the premium panel blitter. (Reporter: Lunar Ladder #65, Arch/KDE/Wayland)
- **Garbled non-ASCII text on Windows** - all text, window titles, message boxes, file dialogs, drag&drop paths and file IO now go through UTF-8-aware Win32 wrappers (WDL win32_utf8). Fixes accented take/file names, the gear/heart/infinity glyphs, and opening/saving files with non-ASCII paths. (Reporter: X-Raym #63)
- **Standalone mode unreachable on Windows** - dropped files fell through to the REAPER timeline because the window never registered for Windows drag&drop (mac/Linux register automatically). (Reporter: Ben Zero #83)
- **Launch shortcut dead when docked** - if you bound a SneakPeak action to a bare key, SneakPeak's own keyboard handler could swallow that key while the docked window had focus. SneakPeak now recognizes its own action bindings and lets them fire. (Reporter: Ben Zero #83)
- **Standalone meter feed** - meters now run during standalone preview playback, item volume is no longer misread when no REAPER item is loaded, and the Master meter source is ignored in standalone (preview plays outside the project graph).
- **Channel solo keeps stereo placement** - the [1]/[2] channel badges now solo via take pan balance instead of REAPER's mono channel modes, so a soloed channel stays on its own side instead of folding to centred mono. Your take pan is saved on solo and restored on unsolo or item switch. Also fixes the old trap where the badges disappeared after a reload (the take had been turned mono) with no way to revert from SneakPeak.
- **Mode bar polish** - hover feedback on tabs / MASTER / gear / Support (with hand cursor), the Settings gear moved to the far right and enlarged, ruler timestamps vertically centred, and the dB scale column decluttered (wider label spacing, no collisions with the channel badges, real margins).

---

## [2.1.1] - 2026-04-18

### Fixed
- **Destructive edits on Linux did not refresh until REAPER restart** - `RefreshItemSource` (called after Reverse, Normalize, DC Remove, LUFS normalize, etc.) swapped `P_SOURCE` and called `UpdateArrange` but did not invalidate REAPER's cached audio data. On macOS the arrange refresh was enough; on Linux the cache persisted until a full REAPER relaunch, making destructive edits appear to have no effect during the session. Fix adds `UpdateItemInProject(item)` on every refresh, matching the pattern already used by the Replace Source in Timeline feature. (Reporter: reaperfreaker, Debian Trixie)

---

## [2.1.0] - 2026-04-18

### Highlights
SneakPeak v2.1 addresses critical bug reports from the v2.0 release and adds workflow features: two crash/UX bugs fixed, automatic envelope activation, RMS/meter visibility toggles, source replacement back into the REAPER timeline, and an update checker.

### Fixed
- **Catalina right-click menu crash** - On macOS 10.15 opening the main context menu triggered a use-after-free when the submenus were double-released (SWELL `InsertMenuItem` transfers submenu ownership to the parent, so the explicit `DestroyMenu` calls left dangling references). The parent now cleans up submenus on every platform. Newer macOS and Linux allocators were lenient and hid the bug; Catalina's stricter reuse made it deterministic. (Reporter: alphoc, forum #42/#46)
- **Envelope point add did not drag the new point** - Clicking on the envelope line inserted a point with `selected=false` and the drag loop moves only selected points, so the newly added point stayed still while a previously-selected point slid underneath. Add-point now deselects all others, selects the new point, and initializes the drag clamp bounds from its neighbors. Also sorts the envelope immediately after Cmd+click (freehand start) so `Envelope_Evaluate` returns correct values during the gesture - previously the waveform on the unrelated side briefly distorted until mouse-up. (Reporter: Lunar Ladder, forum #41)

### Added
- **Auto-activate take volume envelope** - Enabling Show Volume Envelope or opening the Dynamics Panel on a take without an active volume envelope now creates and activates it automatically via REAPER action 40693. No more manually right-clicking the item and enabling the envelope before SneakPeak can work with it. Toast confirms "Volume envelope enabled" on first activation. Multi-take items: only activates when SneakPeak's displayed take is the item's active take. (Reporter: Khron Studio)
- **Hide RMS toggle** - View menu entry to hide the darker RMS overlay inside the waveform, leaving only the peak outline. Useful on dense stereo content where RMS fill obscures detail. Persists via ExtState. (Reporter: Khron Studio, forum #35)
- **Hide Meters toggle** - View menu entry to collapse the bottom meter/info panel entirely, giving the waveform the full vertical space. Scrollbar stays at the bottom. Persists via ExtState. (Reporter: Khron Studio)
- **Replace Source in REAPER Timeline** - New context menu entry in standalone mode: after editing a file in SneakPeak, one click saves the edited content (respecting the existing smart-save rules: WAV overwrites with a prompt, non-WAV auto-creates `name_edit.wav`) and swaps `P_SOURCE` on every take in the project whose source file matches the standalone's original path. Toast reports the number of items updated. Immediate waveform refresh in REAPER arrange (no need to change window focus). (Reporter: Lunar Ladder, forum #41)
- **Check for Updates** - Click the version label in the mode bar to query GitHub's Releases API via `curl` (5 s timeout). Toast reports either "SneakPeak is up to date (vX.Y.Z)" or "Update available: vX.Y.Z (you have vA.B.C)". Graceful failure when offline.

---

## [2.0.0] - 2026-04-16

### Highlights
SneakPeak v2.0 is a major release: **multiplatform support** (Windows, Linux), a full **dynamics engine** (compressor + gate + presets), **volume envelope editing**, and **Live mode** for real-time envelope writing.

### Multiplatform
- **5 platform builds** - macOS arm64 (Apple Silicon), macOS x86_64 (Intel), Windows x64, Linux x86_64, Linux aarch64. All built via GitHub Actions CI.
- **ReaPack support** for all platforms - install via ReaPack for automatic updates.

### Dynamics Engine
- **Professional compressor** - Industry-standard gain-smoothing model (ratio, threshold, soft knee, attack, release, auto makeup gain). Matches FabFilter Pro-C / Waves / ReaComp architecture.
- **Noise gate** - Post-compression gate for breath reduction in speech/podcast. Three parameters: threshold, range, hold time. Gate threshold shown as dim red line on waveform.
- **Lookahead** - 0-20ms transient detection. Scans ahead in the audio buffer so the compressor starts reducing gain before the peak arrives.
- **Peak/RMS detection** - Toggle between peak and RMS analysis. RMS provides smoother compression curves for music content.
- **Auto makeup gain** - Automatic loudness compensation from compressed points only (not diluted by silence).

### Dynamics Panel
- **Inline control panel** - 10 real-time sliders: Threshold, Ratio, Knee, Lookahead, Gate Threshold, Attack, Release, Makeup, Gate Range, Gate Hold. Any change instantly updates the compression preview on the waveform.
- **Live mode** - [Live] toggle writes envelope points to REAPER in real-time as you drag sliders. Waveform updates instantly. Single undo block per gesture (Cmd+Z reverts entire adjustment).
- **6 built-in presets** - Default, Gentle Leveling, Voice/Podcast, Broadcast, De-breath, Music Bus. Researched from iZotope, Waves, EBU R128, BBC guidelines.
- **Per-item persistence** - Dynamics settings auto-saved to REAPER item P_EXT on Apply, auto-loaded when reopening the panel on the same item.
- **GR meter** - Gain reduction meter in the panel title bar showing real-time compression depth.
- **Compression preview curve** - Purple overlay showing post-compression levels alongside the original amplitude curve (orange).
- **GR shading** - Semi-transparent fill between original and compressed curves showing where and how much compression is applied. Toggle with [GR] button.
- **A/B bypass** - [A/B] toggle disables the volume envelope in REAPER for instant before/after comparison (audio + visual). Auto-restored when panel closes.
- **Visibility toggles** - [Dyn] shows/hides dynamics curves, [Env] shows/hides envelope overlay. Waveform always reflects the actual envelope effect.
- **Slider fine mode** - Hold Cmd/Ctrl for 1/5th sensitivity. Grab offset prevents value jumping.

### Volume Envelope Editing
- **Envelope overlay** - Cyan curve showing the take volume envelope, rendered per-segment in timeline/SET modes. 1:1 match with REAPER's arrange view (uses native fader-scale Y mapping).
- **Point editing** - Click on the envelope line to add a point. Drag to move. Double-click or Delete/E to remove. Right-click for curve shape menu (6 shapes: Linear, Square, Slow start/end, Fast start, Fast end, Bezier).
- **Multi-select** - Shift+click to toggle point selection. Drag any selected point to move all selected. Delete removes all selected.
- **Freehand drawing** - Cmd+drag on the envelope line to draw points freehand (creates points every 4px, removes overlapping).
- **Selection rectangle** - Cmd+drag on empty area draws a selection rectangle with hatched fill.
- **Dense point interaction** - After Apply Dynamics creates >100 points, Cmd+drag draws a reveal rectangle. Points within become visible and interactive. Time-based (survives zoom/scroll).
- **Works in all modes** - Envelope editing works in ITEM, Timeline, and SET modes via per-segment envelope lookup.
- **Auto-refresh** - Envelope changes made in REAPER arrange view are detected and displayed automatically.

### Quality of Life
- **Scroll-for-gain on knob** - Mouse wheel on the gain knob adjusts gain +/-0.5 dB per notch. Cmd+scroll for fine mode (+/-0.1 dB). Scrolling outside the knob still zooms/pans as before.
- **Support button in mode bar** - Clickable heart icon next to the version number opens a dropdown with Ko-fi, Buy Me a Coffee, PayPal, and GitHub links.
- **Multi-item dropdown** - Click the "MULTI" label for a dropdown menu with Mix/Layered modes and "Timeline View" option.
- **Gain knob range** - Extended to +/-24 dB (matches REAPER API range).
- **Gain knob colors** - Blue for single/selected item, gold for batch mode.
- **Multi-item copy/paste** - Copy in multi-item view mixes layers into clipboard.
- **Drag export bakes fades** - Exported audio includes REAPER item fades.
- **Pinch gesture consumed** - No longer passes through to REAPER arrangement.
- **Horizontal scroll direction** - Matches REAPER arrange view.

### Performance
- **Dynamics curve rendering** - Binary search + max-peak-per-stride + deduplication reduces drawn points from 60000 to ~600 per frame.
- **RDP curve simplification** - Ramer-Douglas-Peucker reduces envelope points from 60000 to 200-500 for Apply.
- **Adaptive point rendering** - Points hidden when >100 visible, small when 30-100, normal when <30.

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
