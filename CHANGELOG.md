# Changelog

All notable changes to SneakPeak will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/), and this project adheres to [Semantic Versioning](https://semver.org/).

## [Unreleased] - v2.5.0 (in development)

### Added
- **Scriptable Standalone open** - the action "SneakPeak: Open file in Standalone" opens the file whose path a script stored in ExtState `SneakPeak/open_path` (the drag-and-drop route, for ReaScripts).

### Changed
- **Long items no longer freeze REAPER** - selecting an item now paints the waveform from REAPER's own peak files immediately and decodes the audio in the background (title shows the progress); the same applies to Timeline view after an edit, to SET view and to Multi-item view, which used to decode every segment or layer synchronously. Timeline view also drops its old 600-second span limit: a long item split by a delete now stays in Timeline view instead of falling back to a full reload. Measured on a 20-minute WAV and a 17-minute AAC: the longest main-thread stall on select/reselect/delete/multi-select went from 0.4-1.5 s to under 0.1 s. Operations that need the raw samples (destructive edits, drag export, dynamics apply, spectral) show a short "still loading" toast until the audio is in.
- **Dynamics knobs track the mouse on long items** - the analysis no longer runs inline on every mouse move: the panel updates at input rate while the engine recomputes the latest knob position on a worker thread, and Live mode writes the envelope 150 ms after the knob settles (and immediately on release), so the written points for the final position are unchanged. Measured on a 20-minute item with Live on: 10 ms per mouse move (was 230 ms). The envelope ceiling lookup and the transfer-curve overlay are also cached/clipped to the visible range.
- **Long items are drawn from REAPER's own peaks, always** - items over about 3.8 minutes (stereo, 44.1 kHz) keep a reduced-rate working buffer for analysis; the waveform and the minimap used to switch to that buffer once it had loaded, which hides transients (a one-sample click at 0.9 read 0.16 through an 8 kHz buffer) and everything above 4 kHz. Such items now stay on REAPER's peak files, the same data the arrange view draws, so what you see matches REAPER at every zoom. (The RMS band needs real samples and stays off on these items; full-rate items are unchanged.) Selecting a one-hour WAV also no longer pauses REAPER for the buffer allocation: the buffer grows chunk by chunk as the audio streams in.
- **Re-selecting the same item is instant** - the decoded buffer of the last single item survives a deselect; clicking it again reuses it when the take is unchanged instead of decoding again.
- **Long items no longer decode a working buffer on select** - since the waveform, the minimap, every export and the Dynamics panel work from REAPER's peak files or stream the source, the reduced-rate working buffer of an item over about 3.8 minutes (stereo, 44.1 kHz) is now decoded only when something actually needs the samples: the Spectral view and the One-Shot Factory open at once and fill in when it lands (the title shows the progress), the Hard Limiter and the remaining sample-based commands ask for it with a short toast and work on the next try. Selecting a one-hour item, re-selecting it, deleting a range of it (Timeline view) or entering a SET view over long items now costs no decode and no memory (a one-hour stereo item used to hold 460 MB); Reverse, DC Remove and Gain on a selection edit the file in place without waiting for any buffer, and the buffer is refused outright (with a message) on items that would need more than 1 GB, instead of silently allocating it. The level meter follows playback on such items without the buffer (it reads the played window straight from the item at the source rate). Items under 3.8 minutes are unchanged (full-rate buffer, loaded in the background as before).

### Fixed
- **Hard Limiter in ITEM mode could rewrite a multichannel file as stereo, or a stereo file as mono** - SneakPeak's working buffer holds at most two channels (one when the take uses a mono channel mode), and the limiter's whole-file write did not compare that against the file: applying it to a 6-channel WAV wrote channels 1-2 back over the file (channels 3-6 lost; the single-level undo was the only way back), and a stereo item set to "Mono (mix)" came back as a mono file. Every whole-file write now refuses a channel-count mismatch, and the Hard Limiter checks all of its requirements (WAV, full-rate buffer, item covering the whole file, matching channels, normal or reversed-stereo channel mode) before the confirmation prompt and before any processing, with a message that says the file was not changed.
- **Destructive edits on a reversed take edited the wrong part of the file** - a take reversed in REAPER (Item properties, or "Toggle take reverse"), or a take that plays a section of its file, goes through a SECTION source whose parent is the WAV; SneakPeak edited the parent file at the item's window as if the take played it forwards from that offset, so Reverse, DC Remove, Gain on a selection, Paste and the Hard Limiter changed a different region of the file than the one on the timeline. Such takes are now refused before the confirmation prompt with a message (the file and the reversed playback stay as they are); reverse or unsection the take in REAPER first, or glue it, to edit it destructively.
- **A destructive edit went ahead even when its undo copy could not be written** - before Reverse, DC Remove, Gain on a selection, Paste or the Hard Limiter rewrite the source file, SneakPeak copies the file into the temp folder so the edit can be undone; when that copy failed (temp folder missing, disk full) the failure was only logged and the file was edited anyway, with no way back. The edit is now cancelled with a message naming the temp folder, and the previous undo copy (if any) is kept until the new one has been written.
- **A destructive edit that failed part-way left the file half-edited** - Reverse, DC Remove and Gain on a selection edit the source file in place, chunk by chunk; when a read or write failed half-way (a truncated file whose header claims more audio than it holds, a disk that fills up) the chunks already written stayed, behind a plain "Failed to write WAV file" box. The file is now put back from the pre-edit copy taken moments before (message: "Write failed - the file was restored from the pre-edit copy"), and only if that restore fails too does an error box name where the copy is.
- **Cut deleted the selection even when nothing was copied** - a Copy that was refused (a selection over the 1 GB buffer cap, audio that could not be read) still went on to ripple-delete the selection, leaving the clipboard stale. Cut now deletes only after the copy succeeded.
- **Pasted clips could vanish from a saved project** - Paste wrote its clip into the OS temp folder, which macOS, Linux and Windows purge (reboot, age rules, Storage Sense), so a project saved with pasted audio lost that media later; two pastes within the same second also overwrote each other's file. The clip is now written into the project's recording folder (REAPER's default recording path while the project is unsaved), named after the source with a per-session counter.
- **Dragging audio out of an unsaved project exported into the temp folder** - the drop into REAPER referenced a file that the OS could purge later. The export now goes to the project's recording folder whether or not the project has been saved (then next to the source file, and to the temp folder only when neither exists).
- **Windows: destructive edits failed on files under a non-ASCII path** - the in-place editors (Reverse, DC Remove, Gain on a selection) and the loop-point reader opened the file through the ANSI C runtime, so a source under a folder or user name with accented characters was "not found" ("Failed to write WAV file"). They now open files the same UTF-8-aware way as the rest of SneakPeak.
- **Windows: temporary files leaked under a non-ASCII user name** - the undo copies, paste clips, preview and export temp files were deleted with the ANSI C runtime, which does not find them under an accented %TEMP% path; they are now removed through the UTF-8 file API.
- **Standalone silently folded multichannel files to stereo** - a 6-channel WAV dropped into Standalone loaded as its first two channels, and Save wrote that back over the original (channels 3-6 gone). Standalone now refuses files with more than two channels up front, with a message; the same 1 GB working-buffer limit as for items now applies to Standalone too (a file over it is refused instead of allocated, and very long files no longer overflow the frame count).
- **Standalone: Reverse, DC Remove and Paste had no undo** - they went down the ITEM path (a "modifies the audio file on disk" prompt, an ITEM-mode undo snapshot that Standalone never uses), so Ctrl+Z after them did nothing and Paste touched a null item. They are now plain in-memory Standalone edits with their own undo step and no prompt.
- **Standalone Dynamics could race its own buffer** - knob drags in Standalone were routed through the background analysis worker although Standalone edits replace the buffer on the main thread; Standalone analyses synchronously again (as documented), and every buffer mutator waits for a running worker first.
- **One-Shot Factory: LUFS-I on a PCM Standalone file clipped** - normalizing a quiet 16/24-bit file to a LUFS target pushed its peaks over full scale and the PCM writer clipped them flat; the slice now goes through the true-peak limiter (ceiling -0.1 dBTP) when the gain would clip.
- **One-Shot Factory: REGIONS mode in Standalone sliced by unrelated regions** - a Standalone file is not on the timeline, so slicing it by the project's regions cut it at meaningless positions; the mode is now refused in Standalone with a message (open the file as an item to slice by regions).
- **Keys SneakPeak had no use for were swallowed** - with the editor focused, a bare arrow key, Tab without markers, Ctrl+S outside Standalone or Ctrl+C without a selection were eaten and REAPER never saw its own shortcut (arrows did nothing at all). SneakPeak now takes a key only when it actually acts on it in the current state; everything else reaches REAPER as before. Alt+letter combinations are never taken.
- **Ctrl+Y did not redo in Standalone** - the key was handled by the editor but never let through by the keyboard hook; Ctrl+Y now redoes (Ctrl+Shift+Z still works).
- **A drag interrupted by a right-click or a lost mouse capture stayed "stuck"** - the selection (or envelope point, fade, slider ...) kept following the mouse and its undo block stayed open. A right-click no longer interrupts a drag, and a capture lost to another window, a dialog or Alt+Tab ends the drag exactly like releasing the button.
- **"Check for updates" froze REAPER for up to 5 seconds** on a slow or blocked network; it now runs in the background and reports with a toast. It also compares versions properly (2.10 is newer than 2.9, an rc is older than its release, a prerelease build is offered the final) and knows the release tag it was built from - a local development build skips the check.
- **Windows: the floating window opened at twice its size** - the window template gives its size in dialog units, which Windows scales by the system font (about 2 px each), so a fresh install opened a roughly 1600x800 window (wider than a 1512 px screen) where macOS and Linux open 800x400. The window now opens at 800x400 (scaled by the display's DPI); a saved position and size still restore as before.
- **The floating window could restore off-screen** - a position saved on a monitor that is no longer there put the window out of reach; it is now moved onto the nearest monitor. ESC in the Settings panel while a slider is being dragged first ends the drag.
- **Unloading the extension left its keyboard hook and actions registered** - REAPER could call into unloaded code; they are unregistered on unload and at exit.
- **Linux: the support links in the right-click menu did nothing** - they called the macOS opener (`/usr/bin/open`); they now go through `xdg-open` on Linux.
- **Normalize (peak) on long items landed too hot** - on items over about 3.8 minutes the peak was measured from the downsampled working buffer, which flattens transients (a full-scale click can read 15 dB low), so the item was normalized above the -0.1 dB target. Peak normalize now streams the true peak from the source at its full sample rate (no buffer needed at all - the "loading" wait on long items is gone with it); LUFS normalize was always measured by REAPER and is unchanged.
- **Windows: dragging audio out of SneakPeak did nothing** (forum #105/#97, Ben Zero) - the exported selection was written, but the Windows shell refused the file's path (SneakPeak builds paths with forward slashes, which the C runtime accepts and the shell's item parser rejects), so the drag session never started - into REAPER's timeline or anywhere else. The path handed to the shell is now normalised, and a drag from a selection drops into the arrange as a new item.
- **Tiny UI on scaled displays despite a persisted 1.0 scale** (forum #97/#105, Ben Zero, Windows) - an install that saved UI scale 1.0 before the first-run auto-detect existed (v2.2.0), or detected it on a 96-DPI monitor, kept the tiny scale on a scaled display forever. If the saved scale is exactly 1.0, the user never moved the UI SCALE slider, and the system asks for 125% or more, the scale now re-seeds from the system once; a slider choice still ends the automatics for good.
- **Windows: Standalone playback was silent, and Paste, Undo snapshots, One-Shot LUFS normalize and some exports failed quietly** (forum #105, Ben Zero) - every temporary WAV SneakPeak writes (the Standalone preview file, the paste clip, the pre-edit undo copy, the One-Shot LUFS measurement file, the export fallback when a project is unsaved) went to `/tmp`, which does not exist on Windows, so the write failed and the feature did nothing. They now go to the system temp folder (`%TEMP%` on Windows; `$TMPDIR` or `/tmp` elsewhere, unchanged). Export paths also recognise Windows path separators, so "next to the source file" works there too.
- **Windows: Undo of a destructive edit failed with "check the file permissions"** - after an in-place edit the old media source kept a write-denying handle on the file for the rest of the session, so restoring the pre-edit snapshot (and any later destructive edit of the same file) was refused. The post-edit refresh no longer replaces the source object on Windows (the file is rewritten in place, so REAPER picks the new audio up from the same source), and the restore itself now takes the item's media offline for the copy, like the destructive edits themselves.
- **Windows: the SneakPeak window had no caption until the first item loaded** - the window title (and the taskbar entry) was empty after opening; it is now "SneakPeak" from the start, as on macOS and Linux.
- **Dynamics on long items analysed a reduced-rate buffer** - items over about 3.8 minutes (stereo, 44.1 kHz) are held in a downsampled working buffer, and the Dynamics panel (compressor, gate, de-esser, Live, Apply) read that buffer, so on a one-hour item the detector saw band-limited audio with flattened peaks (a one-sample click at 0.9 read 0.16; a 12 kHz sibilant tone all but vanished). Item views now analyse the source at its own rate, streamed chunk by chunk in the background (the title shows "Analysing dynamics... N%"); the working buffer is no longer needed for Dynamics at all. On a short item this takes a few tens of milliseconds and nothing visible changes; on a long one the curve, the GR meter and a pressed Apply follow as soon as the analysis lands, and changing the detector (Peak/RMS, RMS window, de-ess band) re-streams while every other knob recomputes from the finished analysis. Standalone files are unchanged (full-rate buffer). The analysis also holds a third of the memory it used to per item.
- **Exports from long items were written at a reduced sample rate** - items over about 3.8 minutes (stereo, 44.1 kHz) are held in a downsampled working buffer, and drag export, "Edit Copy in Standalone" and the One-Shot Factory all wrote that buffer, so a 5-minute item produced 22050 Hz files (an hour-long one, 8000 Hz). These exports now stream the item from its source at the source's own rate, chunk by chunk (no whole-item buffer): drag export writes the selection in the source file's format (24-bit PCM for non-WAV sources), Edit Copy writes the copy in the background with progress in the title (an item whose full-rate copy would not fit Standalone's 1 GB buffer is refused with a message), and each One-Shot slice is trimmed, faded and normalized on the full-rate audio (a slice over 3.8 minutes is skipped - one-shots are short). Item volume is now baked into these exports consistently (single-item exports used to ignore it) and take channel modes (mono mix, left, right) are honoured as before. Loop Lab is unaffected (it only ever works on Standalone's full-rate buffer).
- **Copy from a long item put reduced-rate audio on the clipboard** - items over about 3.8 minutes (stereo, 44.1 kHz) are held in a downsampled working buffer, and Copy (and Cut) lifted the selection out of that buffer, so Paste then inserted an 8 kHz temp file into the project. Copy now streams the selection from the source at its own rate (item volume baked in, like every export) and needs no working buffer; a selection whose samples would not fit the 1 GB buffer cap is refused with a message. Standalone and Multi-item copies are unchanged.
- **Destructive edits on long items could re-encode the source at a reduced sample rate** - items over about 3.8 minutes (stereo, 44.1 kHz) are held in a downsampled working buffer, and Reverse, DC Remove and Gain on a selection wrote that buffer back to the file (a 20-minute WAV came back at 8 kHz), as did Undo of such an edit. Those three edits now stream through the file itself at its own rate, in place, with every other chunk left untouched; the Hard Limiter refuses such items with a message instead of writing; and the single-level destructive Undo snapshots the pre-edit file itself (a temp-folder copy) rather than the working buffer, so it restores the original bytes on items of any length.
- **Destructive edits on a trimmed item truncated the source file** - Reverse, DC Remove and Gain on a selection wrote the item's window back as the whole file, so an item that showed only part of its source (trimmed, offset or rate-changed) left the file cut down to that part. They now edit exactly the item's window inside the file, mapped through the take's start offset and playrate; the Hard Limiter refuses such items with a message rather than writing.
- **Destructive ITEM edits were sometimes inaudible until a restart** (forum #47, Linux; also seen on macOS) - Reverse, Normalize, DC Remove, LUFS and the other in-place edits wrote a temporary file and renamed it over the source, which gives the source a new file identity. REAPER keeps a pool of decoders per path, and any decoder already open on the old file kept playing the pre-edit audio - so playback, peaks and audio accessors could disagree, or all show the old audio. The edited file is now rewritten in place (same file identity), which every open decoder picks up immediately.
- **Timeline, SET and Multi-item views did not follow edits made outside SneakPeak** - the segments of these views remember their items' positions from the moment the view was built. Moving an item in the arrange, a script, or REAPER's own Undo (of a SneakPeak split, say) left the view showing the old layout: clicks and pastes landed where the item used to be, and a segment whose item had been deleted was still painted, which could crash REAPER once that memory was reused. The views now watch REAPER's project-state counter and rebuild themselves when a segment no longer matches its item, keeping the visible range, the selection and the cursor; edits SneakPeak makes itself never trigger a rebuild. Undoing a SneakPeak split in REAPER returns the view to the single item.
- **Changing an item's playrate while it was shown wrote the next Dynamics envelope in the wrong timebase** - SneakPeak polled the item's position, length and channel mode but not the take's playrate, so a rate changed in Item properties (or by a script) after the item was loaded left Apply Dynamics and Live mode writing points in item time (the v2.5 playrate fix, undone by a stale value). The playrate is now polled like the length, in single-item views and per segment in Timeline and SET view.
- **Apply Dynamics in Timeline and SET view left steps in the envelopes** - each fragment received only the curve points that fell inside it, and the write preserves the old envelope outside the written span, so a stretch of constant gain (makeup, a plateau between two dips) that crossed a fragment boundary left the rest of that fragment at the old 0 dB: a jump mid-item on every fragment. Every fragment now gets the curve's value at both of its edges.
- **Items locked in REAPER could be deleted** - SneakPeak's Delete went through REAPER's API, which refuses to split a locked item but deletes one whole without complaint, so a selection covering a locked item removed it (and the ripple moved locked neighbours). Locked items are now left alone by Delete, ripple delete, Paste and gain on a selection, with a message: "Item is locked in REAPER - unlock it to edit here".
- **Replace Source in REAPER Timeline left items longer than the new file** - after a Standalone edit that shortened the file, the items kept their old length and ran past the new end (with REAPER's default "loop source" they wrapped back to the start), and because REAPER keeps one file reader per path while a take holds it open, the swapped-in source still reported the old length. The items are now taken offline for the swap so the new source reads the file as saved, and each item is shortened to end where the new file ends (at its playrate); an item that showed only audio past the new end restarts from the file's start. The path match is now exact on Linux (case-insensitive on Windows and macOS, whose file systems are).
- **Working set could lock the view** - after a SET view whose items (or track) were later deleted in REAPER, SneakPeak could refuse to load any newly selected item (stale set flags compared dead item pointers; a new item reusing the address counted as "in the set"). The set is now validated before it is consulted and reset when nothing of it survives.
- **Envelopes on items with playrate other than 1.0** (forum #107, Lunar Ladder) - REAPER stores take-envelope point times in the take's own timebase (item time x playrate) and SneakPeak read and wrote them in plain item time. On a rate-changed item every envelope written by SneakPeak (Apply Dynamics, Live mode, clicked, dragged and freehand points) landed at the wrong time in REAPER - early on faster items, late on slower ones - and the overlay drew REAPER's own points shifted the other way. All envelope reads and writes now map through the take playrate, per segment in Timeline and SET view.

## [2.4.0] - 2026-07-10
<!-- User decision 2026-07-02: v2.3.0 is SKIPPED - the Dynamics Suite ships
     together with the Game Audio Suite (Loop Lab, One-Shot Factory, True-Peak
     Hard Limiter) as one v2.4.0 release. -->

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
- **Spectral frequency grid + Hz/Notes scale** (forum #88, sguyader) - faint horizontal reference lines now run across the spectrogram at the labeled frequencies, and a new Settings > View selector switches the scale (and the grid) between Hz labels and note names (A0-A9 octaves, A4 = 440 Hz bold) - identify the pitch of a tone or resonance before healing it.
- **Loop Lab panel** - the whole loop workflow lives in one panel now, not in menu trips: right-click > **Loop Lab...** (Standalone mode) opens it with live **START / END / LENGTH** readouts, the finder's results as a clickable **candidate list** (click a row to set the loop and hear it wrap; the active row stays highlighted; texture candidates are tagged **TEX**; the numbered ruler pins remain as on-waveform anchors and select the same state), **PLAY LOOP** / **PLAY SEAM** transport pills, **FIND**, **WELD** with the crossfade length right next to it (drag or scroll, 5-500 ms - no dialog), **SET FROM SELECTION**, **CLEAR** and the **WRITE SMPL ON SAVE** toggle. The Loop submenu collapses to the panel entry plus a direct Set Loop From Selection; the panel remembers its position.
- **Loop Lab: Find Loop Points** - **FIND** scans the file in the background for seam candidates: loop lengths from max(1 s, 10% of the file) up, endpoints on rising zero crossings, ranked by how well the audio just before the end continues into the audio just before the start (normalized cross-correlation of 30 ms windows, with an FFT timbre check as tie-break). The top 5 land in the panel's candidate list and as **numbered yellow pins** on the ruler - click either to set the loop and hear it wrap immediately. A toast reports the count. On stochastic ambiences (birds, rain, wind) - where waveforms never repeat and the strict search honestly finds nothing - a **texture fallback** kicks in: candidates are picked by perceptual continuity instead (averaged spectrum similarity + matching energy level, avoiding cuts mid-transient) and the toast says to **Weld the seam** after choosing, which is exactly how ambience loops are made.
- **Loop Lab: Weld Loop (crossfade the seam)** - when even the best loop points still click, **WELD** bakes an equal-power crossfade (5-500 ms, default 50, set right on the panel) over the end of the loop, blending it into the material that precedes the loop start - after the weld, the wrap is continuous by construction. Length-preserving and destructive with a bounded undo (only the crossfaded stretch is snapshotted); a running audition replays the welded seam immediately. Needs the crossfade's worth of audio before the loop start (an honest toast says so otherwise).
- **Loop Lab: loop points saved into the WAV (`smpl` chunk)** - with a loop set, saving writes a standard `smpl` sustain loop (forward, infinite) into the file, the format Unity, FMOD, Wwise and samplers read natively - no more round-tripping through external editors just to author loop metadata. Loading a WAV that already has loop points shows them as the loop region (with a toast). The panel's **WRITE SMPL ON SAVE** pill toggles the behavior (on by default, remembered). Note: SneakPeak's writer rebuilds the WAV on save, so other metadata chunks (bext/iXML) from the original file are not carried over - same as before, now documented.
- **One-Shot Factory (Standalone mode)** - the repetitive SFX-prep chain in one panel: right-click > **One-Shot Factory...** opens ONE-SHOT PREP with **Trim silence** (threshold, keep-padding, toggleable), **edge micro-fades** (the industry click-killers, 5 ms in / 20 ms out defaults) and **Normalize** (Off / Peak dBFS / LUFS-I with a custom target measured by REAPER's own engine / **True-Peak safe** - the hard limiter at your ceiling). While the panel is open the waveform previews the prep LIVE: the zones the trim will cut are dimmed, amber boundary lines mark the kept region and diagonal ramps show where the edge fades land - all following the knobs as you turn them. **Run** writes the prepared file(s) next to the source (existing files are never overwritten - collisions get a numbered suffix `_2`, `_3`, ...) - the loaded audio is never touched. Unrecognized `{tokens}` in the naming pattern degrade gracefully (braces are stripped, so a mistyped `{test}_{01}` still writes a clean `test_01.wav`). Settings persist between sessions.
- **One-Shot Factory: slice to variations + naming pattern** - the "10-30 variations per SFX" workflow in one Run: a **SLICE** selector splits the file into **WHOLE** (one file, the previous behavior), **REGIONS** (each region becomes a variation; with no regions, markers act as split points), or **SILENCE** (auto-split on gaps longer than 150 ms below the trim threshold; slices shorter than 50 ms are dropped, and each slice keeps its padding without ever bleeding into a neighbour). Every slice runs the full trim > fades > normalize chain and is written using the **NAME pattern** - tokens `{name}` (source basename) and `{nn}` (01-based, zero-padded); any digit token also numbers: `{01}` counts from 01, `{001}` pads to three digits, `{5}` starts at 5. Default `{name}_{nn}`; click the pattern box to edit it. The live waveform preview shows every slice that will be written (dimmed cut zones, amber boundaries and fade ramps per slice) so the threshold can be tuned until the split is right. Long batches show progress in the title bar; the summary toast reports "14 files written" (plus any skipped slices).
- **One-Shot Factory in ITEM mode** - the Factory now also runs directly on a selected timeline item (plain ITEM mode): design the sound on the timeline, select the item, right-click > One-Shot Factory - the finished assets are written next to the item's media file, and the item itself is never modified. The whole panel works there, including slicing and the live per-slice preview (project regions and markers slice exactly as they do in Standalone mode). Not available in SET / Timeline View / multi-item (segmented buffers).
- **One-Shot Factory: OPEN FOLDER button** - reveals the export destination folder in Finder / Explorer / your file manager, straight from the panel - no hunting for where the batch landed.
- **Edit Copy in Standalone (ITEM mode)** - the one-command bridge from the timeline into every standalone-only tool: right-click a selected item > **Edit Copy in Standalone** writes the item's audio as `{name}_edit.wav` (32-bit float, lossless) next to its media file and opens it as a new Standalone tab - Loop Lab, Spectral Repair, the Hard Limiter and destructive editing are all available on the copy, the original item stays untouched. When done, **Replace Source in REAPER Timeline** closes the round trip. Collisions get numbered suffixes like the Factory.
- **Dynamics in Standalone mode (destructive)** - the Dynamics panel now opens on Standalone files too, working like a classic offline compressor: Apply multiplies the computed gain curve (compressor + gate + de-esser - exactly what the envelope version writes) directly into the audio, with one undo step and a toast reporting the average GR. Live mode and A/B are envelope concepts and render disabled in Standalone; the transfer plot, GR meter and waveform preview curves stay fully live, and parameters use session defaults (no per-item storage without an item). Together with the Hard Limiter this completes the standalone "podcast chain": Dynamics > Hard Limiter > Save.
- **Hard Limiter in ITEM mode** - the limiter panel now also opens on a selected timeline item (plain ITEM mode), with the full live preview (GR band over the waveform, IN/OUT/max-GR readouts). Apply follows the same destructive rules as Reverse and DC Remove: a confirm prompt, the source WAV is rewritten on disk (WAV sources only), every item referencing it refreshes, and the edit is one REAPER undo point. In ITEM mode the panel footer carries a standing "APPLY REWRITES THE SOURCE FILE" note, so the destructive reality is visible before the prompt. A time selection limits just that range (with short handoff ramps). The limiter stays sample-destructive by design - a true-peak ceiling cannot be expressed as a volume envelope.
- **Loop Lab: Audition Seam** - on a long loop, checking the wrap should not cost a full pass: **PLAY SEAM** plays just the last ~2 s of the loop, wraps seamlessly into the first ~2 s, pauses 250 ms and repeats - the seam plays every few seconds no matter how long the loop is, and the pause cleanly separates passes. Follows bracket drags and welds like the full audition.
- **Loop Lab: loop region + gapless audition (Standalone mode)** - the first piece of the game-audio loop toolset: select the part that should loop and press **SET FROM SELECTION** (also a direct right-click item) - yellow brackets and a strip mark the region on the ruler (they fall onto the waveform top if the ruler is hidden). Drag either bracket to fine-tune (snap-to-zero-crossing applies when enabled), then **PLAY LOOP** plays the region wrapping seamlessly end-to-start - the truthful way to hear whether an ambience or engine loop clicks at the seam. The audition follows bracket drags and loops until stopped (the pill again, or Space). The loop region is remembered per tab.
- **True-Peak Hard Limiter (Standalone mode)** - a transparent lookahead brickwall limiter for final asset loudness: right-click > Process > **Hard Limiter...** opens a panel with Gain (how hard you push), Ceiling (-12..0, **dBTP** with True Peak on / dBFS with it off), Attack (lookahead), Hold and Release knobs, a stereo **LINK** switch and four factory presets (**Game Asset -1 dBTP**, Master -0.3 dBTP, Brickwall 0 dBFS, Loud + Proud) plus **user presets** (Save preset as... / Delete preset in the same dropdown; up to 32, overwrite by name). While the panel is open, a red band along the top of the waveform previews the gain reduction over time, with input/output peak and max-GR readouts that recompute as you turn knobs (computed on a background thread - long files stay responsive). The ceiling is enforced on true (inter-sample) peaks: detection runs at 8x oversampling, finer than the BS.1770-4 meter standard, and the engine re-measures its own output until it reads clean, so limited files pass platform loudness QC (e.g. the -1 dBTP game-asset ceiling) with margin. Apply is a destructive edit with full undo - whole file, or just the time selection with short handoff ramps at its edges - and runs in the background with progress in the title bar, so limiting an hour-long podcast never freezes the window (if you edit the audio meanwhile, your edit wins and the limiter result is discarded with a note). The engine is locked by an offline correctness harness (ceiling sweeps verified by two independent true-peak meters; below-ceiling audio passes through bit-identical).
- Old projects and presets load bit-identically: with the new parameters at their defaults the engine output is byte-for-byte the same as v2.2.0 (verified by an offline envelope-diff regression harness added to the repo).

### Changed
- **Every mode has its own colour now** - the Multi-item accent was a second orange, indistinguishable from Standalone at indicator size; it is now magenta. Full mode palette: ITEM blue, STANDALONE orange, SET green, TIMELINE lavender, MULTI magenta, MASTER red.
- **Envelope selection rectangle looks like a selection now** - the Cmd+drag rectangle (and the dense-envelope reveal band) draws a frosted translucent interior, the same treatment as the spectral marquee, instead of the old hatched vertical lines.

### Fixed
- **Dynamics sliders no longer stutter on long items** - every knob move re-ran the full peak/RMS scan over the whole file (~23M samples per tick on a 17-minute item); the scan is now cached and re-runs only when the audio or the detection mode actually changes. Reported by BogdanS on a 4-minute item.
- **Destructive ITEM edits are now undoable** (Cmd/Ctrl+Z) - Reverse, DC Offset Remove, destructive gain/paste and the new Hard Limiter rewrite the item's source file on disk, which REAPER's own undo cannot restore - so these edits were effectively one-way (the confirm prompt was the only guard). SneakPeak now keeps a single-level snapshot of the pre-edit audio: undo writes the original back to the file and every item referencing it refreshes. The snapshot is tied to the exact source file (switching items keeps it safe), and REAPER's own undo history entry for the edit remains as a label.
- **Standalone no longer bounces back to ITEM mode** - with the timeline item still selected (the usual case), switching to a standalone tab could immediately exit back to ITEM mode: the selection poll treated the leftover selection as "user selected an item". It now reacts only to an actual selection change.
- **Mode-bar tabs finally say what they are** - the ITEM tab used to show the window title ("SneakPeak: name"), reading like a second mystery file next to the standalone tab; it now shows the take name. Every tab also carries a type marker matching the mode indicator: blue diamond = REAPER item, orange dot = standalone file.
- **Crash when clicking back on the same item from Standalone mode** - selecting the item that was already loaded before entering Standalone crashed REAPER (the fast tab-switch buffer move left the view empty, and the selection poll skipped the reload because "nothing changed"). The reload now always fires when leaving Standalone, and the renderer refuses to index a moved-out buffer.
- **Drag export now works on Windows** - dragging a selection (or a standalone file) out of the SneakPeak window did everything except the actual drag on Windows: the export WAV was written, then nothing followed (the drop hand-off only existed for macOS/Linux). Windows now hands the file to the OS drag as well, so dropping onto the REAPER timeline or Explorer works on all platforms.
- The "Add Region from Selection" context-menu entry now shows its Shift+M shortcut.
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
