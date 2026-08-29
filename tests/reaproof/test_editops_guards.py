"""Edit-ops robustness (v2.5.0 audit, increment A6).

A6.1 - Timeline view kept a segment whose item REAPER had deleted (an edit
in the arrange, a script, an undo) and the solo button walked the dead
pointer: `ToggleTrackSolo` asked REAPER for the track of a freed item.
Control (955f5e8): crash or garbage solo. Fixed: dead segments are skipped
(ValidatePtr2, the same guard UpdateSoloState already had).
A6.5 - "Replace source in timeline" swapped the take's source but left the
item's length alone: after a Standalone edit that shortened the file the item
ran past its source (looping it, B_LOOPSRC is on by default). Control
(c2cf074): D_LENGTH unchanged.
A6.6 / A6.9 - the audit read Multi-item `GetMixedAudio` as striding every
layer by the view's channel count (a mono layer next to a stereo one would
read at double speed); the loader upmixes mono layers on install, so the mix
is right. Writing that spec found the real defect: Copy in Multi-item view
copied NOTHING - `GetSelectionSampleRange` clamps to the shared buffer's
sample count, which the multi view never sets (per-layer buffers), so the
range collapsed to 0 and Copy returned before touching the clipboard (Paste
then pasted the previous clipboard, or nothing). Control (c2cf074): no clip.
A6.7 - zero-crossing snap on a long item: the snap searched the working
buffer, which a long item does not have (lazy, 8g) or holds at a reduced
rate, so the selection edges did not snap at all (or snapped to the 8 kHz
grid). The edges are now looked up in a small window read from the source
at its own rate. Control (7766880): the raw click times.
"""
from __future__ import annotations

import time
from pathlib import Path

import soundfile as sf

from conftest import (SELECT_ITEM0, VK_DELETE, WAVE_Y, _heartbeat_t, burst_fixture, clear_project,
                      click_sync, drag_client, ensure_window, insert_item, insert_item_unselected, key_sync,
                      track_item_count, wait_audio_loaded)

SHOTS = Path("/tmp/sneakpeak-reaproof-shots/editops")
SOLO_BTN = (717, 66)   # the "S" button: waveform top-right, left of the dB scale (800x400 window, scale 1.0)


def _state(sess) -> str:
    s0, s1 = sess.eval("local s, e = reaper.GetSet_LoopTimeRange2(0, false, false, 0, 0, false) return {s, e}")
    toast = sess.eval('return reaper.GetExtState("SneakPeak", "last_toast")')
    items = sess.eval('local tr = reaper.GetTrack(0, 0) local out = {} '
                      'for i = 0, reaper.CountTrackMediaItems(tr) - 1 do local it = reaper.GetTrackMediaItem(tr, i) '
                      'out[#out + 1] = string.format("%.2f+%.2f", reaper.GetMediaItemInfo_Value(it, "D_POSITION"), '
                      'reaper.GetMediaItemInfo_Value(it, "D_LENGTH")) end return out')
    undo = sess.eval('return reaper.Undo_CanUndo2(0) or ""')
    return (f"items {track_item_count(sess)} {items}, time sel {float(s0):.2f}-{float(s1):.2f} s, "
            f"toast {toast!r}, undo {undo!r}")


def _delete_range(sess, x0: int, x1: int, survivors: int):
    drag_client(sess, x0, WAVE_Y, x1, WAVE_Y)      # a Send: posted messages die after it (macOS)
    time.sleep(0.5)
    print(f"\n[editops] after drag {x0}-{x1}: {_state(sess)}")
    key_sync(sess, VK_DELETE)
    try:
        sess.wait_until(lambda: track_item_count(sess) == survivors, timeout=15)
    finally:
        print(f"[editops] after Delete: {_state(sess)}")


def test_solo_after_external_delete(sess):
    """Timeline view over three survivors, the middle one deleted through the
    API (no SneakPeak edit -> no rebuild), then a click on the solo button:
    the track must be soloed and REAPER must still be alive."""
    media = burst_fixture("editops_solo_10s.wav", seconds=10, channels=2)
    clear_project(sess)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=60)
    _delete_range(sess, 200, 300, 2)      # ITEM -> Timeline view over two survivors
    time.sleep(1.0)
    _delete_range(sess, 500, 600, 3)      # three survivors
    time.sleep(1.5)                       # the edit guard expires; the view settles
    sess.eval('local tr = reaper.GetTrack(0, 0) '
              'reaper.DeleteTrackMediaItem(tr, reaper.GetTrackMediaItem(tr, 1)) '
              'reaper.UpdateArrange() return true')
    assert track_item_count(sess) == 2
    time.sleep(1.0)
    t0 = _heartbeat_t(sess)
    click_sync(sess, *SOLO_BTN)
    time.sleep(1.0)
    solo = int(sess.eval('return reaper.GetMediaTrackInfo_Value(reaper.GetTrack(0, 0), "I_SOLO")'))
    t1 = _heartbeat_t(sess)
    print(f"\n[editops] I_SOLO after the click: {solo}; heartbeat {t0} -> {t1}")
    assert t1 is not None and t0 is not None and t1 > t0, "REAPER's main loop stopped after the solo click"
    assert solo != 0, "the solo click did nothing (the dead segment aborted the toggle)"


def _item_len(sess, idx: int) -> float:
    return float(sess.eval(f"return reaper.GetMediaItemInfo_Value(reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), {idx}), 'D_LENGTH')"))


def _item_pos(sess, idx: int) -> float:
    return float(sess.eval(f"return reaper.GetMediaItemInfo_Value(reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), {idx}), 'D_POSITION')"))


def test_set_view_follows_external_nudge(sess):
    """A6.2: SET view over two survivors (concatenated: the gap between them is
    not shown, so every edit maps through the segments' cached positions); a
    selection is made inside the second item, then that item is moved +1 s
    from outside SneakPeak (an undo block, as every arrange gesture, action
    and well-behaved script makes one - REAPER's project-state counter, the
    poll's trigger, does not move for a bare API setter). Nothing SneakPeak
    did raised its edit guard, so the view must follow, keeping the
    selection: Delete now splits the item 1 s later in the project. Control:
    the segment keeps its stale position - the split lands where the item
    used to be."""
    from conftest import CM_TRACK_VIEW, command_sync, mode_from_capture
    media = burst_fixture("editops_nudge_10s.wav", seconds=10, channels=2)
    clear_project(sess)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=60)
    _delete_range(sess, 200, 300, 2)      # [0, 2.64] + [3.96, 10] on the track
    time.sleep(1.0)
    sess.eval("reaper.SelectAllMediaItems(0, true) reaper.UpdateArrange() return true")
    time.sleep(0.5)
    command_sync(sess, CM_TRACK_VIEW, settle=1.5)
    assert mode_from_capture(sess, SHOTS / "set.png") == "SET"
    drag_client(sess, 500, WAVE_Y, 550, WAVE_Y)   # a selection inside the second item
    time.sleep(0.5)
    s0, s1 = (float(v) for v in sess.eval(
        "local s, e = reaper.GetSet_LoopTimeRange2(0, false, false, 0, 0, false) return {s, e}"))
    assert s0 > _item_pos(sess, 1), "the selection must sit inside the second item"
    sess.eval("reaper.Undo_BeginBlock() local tr = reaper.GetTrack(0, 0) local it = reaper.GetTrackMediaItem(tr, 1) "
              "reaper.SetMediaItemInfo_Value(it, 'D_POSITION', reaper.GetMediaItemInfo_Value(it, 'D_POSITION') + 1.0) "
              "reaper.Undo_EndBlock('spec: nudge item 2', -1) reaper.UpdateArrange() return true")
    time.sleep(1.5)
    key_sync(sess, VK_DELETE)             # the carried selection, mapped through the rebuilt segments
    sess.wait_until(lambda: track_item_count(sess) == 3, timeout=10)
    time.sleep(0.5)
    # SET mode ripple-deletes, so the oracle is the LEFT part's end: the split
    # point, 1 s after where the selection was made
    left_end = float(sess.eval("local it = reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), 1) "
                               "return reaper.GetMediaItemInfo_Value(it, 'D_POSITION') + reaper.GetMediaItemInfo_Value(it, 'D_LENGTH')"))
    print(f"\n[editops] selection {s0:.3f}-{s1:.3f} s before the +1 s move; after Delete the left part ends at "
          f"{left_end:.3f} s (want {s0 + 1.0:.3f}); items {track_item_count(sess)}")
    assert abs(left_end - (s0 + 1.0)) < 0.02, "the SET view did not follow the moved item (Delete split at the old place)"


def test_timeline_follows_reaper_undo(sess):
    """A6.2: a SneakPeak delete split the item (Timeline view); REAPER's own
    Undo puts the single item back. The view must return to the plain item
    (ITEM mode over one segment). Control: the two segments stayed and their
    dead takes crashed the next paint (GetTakeEnvelopeByName)."""
    from conftest import mode_from_capture
    media = burst_fixture("editops_undo_10s.wav", seconds=10, channels=2)
    clear_project(sess)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=60)
    _delete_range(sess, 200, 300, 2)
    time.sleep(1.0)
    sess.eval("reaper.Main_OnCommand(40029, 0) reaper.UpdateArrange() return true")   # Edit: Undo
    sess.wait_until(lambda: track_item_count(sess) == 1, timeout=10)
    time.sleep(1.5)
    mode = mode_from_capture(sess, SHOTS / "after_undo.png")
    print(f"\n[editops] after REAPER undo: {track_item_count(sess)} item, mode {mode}")
    assert mode == "ITEM", f"the view did not follow REAPER's undo (mode {mode}; control: stale segments, a crash on paint)"


def test_locked_item_not_deleted(sess):
    """A6.4: an item locked in REAPER (C_LOCK) must not be split or deleted by
    SneakPeak's Delete - REAPER's own edits respect the lock, and the API
    calls SneakPeak makes do not. The item stays whole and a toast says why.
    Control (c4724b8): the item is split and the middle deleted."""
    media = burst_fixture("editops_lock_10s.wav", seconds=10, channels=2)
    clear_project(sess)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=60)
    sess.eval("reaper.Undo_BeginBlock() local it = reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), 0) "
              "reaper.SetMediaItemInfo_Value(it, 'C_LOCK', 1) reaper.Undo_EndBlock('spec: lock item', -1) "
              "reaper.UpdateArrange() return true")
    sess.eval('reaper.DeleteExtState("SneakPeak", "last_toast", false)')
    time.sleep(0.5)
    drag_client(sess, 200, WAVE_Y, 300, WAVE_Y)
    time.sleep(0.5)
    key_sync(sess, VK_DELETE)
    time.sleep(1.5)
    state = _state(sess)
    toast = str(sess.eval('return reaper.GetExtState("SneakPeak", "last_toast")'))
    print(f"\n[editops] locked item after Delete: {state}")
    assert track_item_count(sess) == 1, "Delete split a locked item"
    assert abs(_item_len(sess, 0) - 10.0) < 0.01, "Delete shortened a locked item"
    # whole-item delete (Ctrl+A, Delete): REAPER's split API refuses a locked
    # item by itself, DeleteTrackMediaItem does not
    from conftest import command_sync
    CM_SELECT_ALL = 2007
    sess.eval('reaper.DeleteExtState("SneakPeak", "last_toast", false)')
    command_sync(sess, CM_SELECT_ALL, settle=0.3)
    key_sync(sess, VK_DELETE)
    time.sleep(1.5)
    toast2 = str(sess.eval('return reaper.GetExtState("SneakPeak", "last_toast")'))
    print(f"[editops] locked item after select-all Delete: {_state(sess)}")
    assert track_item_count(sess) == 1, "Delete removed a locked item whole"
    assert "locked" in toast.lower(), f"no lock toast after the partial Delete: {toast!r}"
    assert "locked" in toast2.lower(), f"no lock toast after the whole-item Delete: {toast2!r}"


# --- A6.5: Replace source in timeline follows a shortened file ----------------
CM_REPLACE_SOURCE = 2067   # edit_view.h ContextMenuID
REPLACE = ('reaper.defer(function() reaper.JS_WindowMessage_Send(SP_WINDOW(), "WM_COMMAND", '
           f'{CM_REPLACE_SOURCE}, 0, 0, 0) end) return true')
OPEN_STANDALONE = ('reaper.defer(function() reaper.Main_OnCommand('
                   'reaper.NamedCommandLookup("_SneakPeak_OpenStandalone"), 0) end) return true')


def test_replace_source_shorter_file(sess):
    """The item's file is opened in Standalone, its tail deleted, then
    "Replace source in timeline" (Save -> overwrite -> P_SOURCE swap). The
    item must end where the new file ends: D_LENGTH == the saved length.
    Control (c2cf074): the item keeps its 10 s over an 8 s source."""
    from conftest import dismiss_native_modal, mode_from_capture, perf_media_dir, wait_main_thread_idle
    # A Standalone tab outlives the test and a second open of the SAME path
    # lands in it (saved state, no prompt): every run gets its own file.
    for old in perf_media_dir().glob("editops_replace_*.wav"):
        old.unlink()
    media = _tone_fixture(f"editops_replace_{int(time.time())}.wav", [0.2, 0.2])
    clear_project(sess)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(f'reaper.SetExtState("SneakPeak", "open_path", "{media.as_posix()}", false) return true')
    sess.eval(OPEN_STANDALONE)
    wait_audio_loaded(sess, media.name, timeout=60)
    time.sleep(0.5)
    assert mode_from_capture(sess, SHOTS / "replace_standalone.png") == "STANDALONE"
    drag_client(sess, 606, WAVE_Y, 790, WAVE_Y)   # ~8 s to past the right edge: the tail
    time.sleep(0.5)
    key_sync(sess, VK_DELETE)
    wait_main_thread_idle(sess, timeout=60)
    time.sleep(0.5)
    sess.eval('reaper.DeleteExtState("SneakPeak", "last_toast", false)')
    sess.eval(REPLACE)
    prompted = dismiss_native_modal(sess, timeout=10)    # "Overwrite original file?" -> Yes
    wait_main_thread_idle(sess, timeout=60)
    time.sleep(1.0)

    new_len = float(sf.info(str(media)).duration)
    item_len = _item_len(sess, 0)
    src_len = float(sess.eval("local tk = reaper.GetActiveTake(reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), 0)) "
                              "return reaper.GetMediaSourceLength(reaper.GetMediaItemTake_Source(tk))"))
    toast = str(sess.eval('return reaper.GetExtState("SneakPeak", "last_toast")'))
    src_path = str(sess.eval("local tk = reaper.GetActiveTake(reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), 0)) "
                             "return reaper.GetMediaSourceFileName(reaper.GetMediaItemTake_Source(tk))"))
    print(f"\n[editops] replace source: prompted {prompted}, file now {new_len:.3f} s, take source {src_len:.3f} s "
          f"{src_path!r} (spec path {media.as_posix()!r}), item D_LENGTH {item_len:.3f} s, toast {toast!r}")
    assert prompted, "no overwrite prompt: Save did not run (nothing to replace)"
    assert new_len < 9.0, f"precondition: the Standalone delete did not shorten the file ({new_len:.3f} s)"
    assert abs(src_len - new_len) < 0.01, "precondition: the take does not point at the saved file"
    assert "Replaced 1 item" in toast, f"replace toast missing: {toast!r}"
    assert abs(item_len - new_len) < 0.01, f"the item ({item_len:.3f} s) runs past its shortened source ({new_len:.3f} s)"


# --- A6.6: Multi-item Copy mixes a mono and a stereo layer correctly ---------
CM_COPY, CM_PASTE = 2002, 2003


def _tone_fixture(name: str, amps: list[float], *, seconds: float = 10.0, sr: int = 44100) -> Path:
    """One 220 Hz sine per channel at the given amplitudes (same phase everywhere,
    so the layer sum is exact), 32-bit float, fresh every call."""
    from conftest import perf_media_dir
    import numpy as np
    path = perf_media_dir() / name
    t = np.arange(int(seconds * sr)) / sr
    y = np.stack([a * np.sin(2 * np.pi * 220 * t) for a in amps], axis=1)
    sf.write(str(path), y.astype("float32"), sr, subtype="FLOAT")
    return path


def test_multi_copy_mono_plus_stereo(sess):
    """A mono item (0.2) on one track and a stereo item (L 0.4, R 0.1) on
    another, both selected -> Multi-item view. A selection in the second
    half of the items, Copy, Paste: the pasted clip must be the mix per
    channel (L 0.6, R 0.3 in amplitude -> RMS 0.424 / 0.212). A mono layer
    read at the stereo stride would contribute nothing there. Control
    (c2cf074): Copy copies nothing, no clip is pasted."""
    from conftest import (click_client, command_sync, mode_from_capture, send_command,
                          wait_main_thread_idle, window_title)
    import numpy as np
    mono = _tone_fixture("multi_mono_10s.wav", [0.2])
    stereo = _tone_fixture("multi_stereo_10s.wav", [0.4, 0.1])
    clear_project(sess)
    insert_item_unselected(sess, mono)
    insert_item(sess, stereo, position=0.0)          # a new track 0; the mono item is on track 1 now
    sess.eval("reaper.SelectAllMediaItems(0, false) reaper.UpdateArrange() return true")
    ensure_window(sess)
    proj_dir = Path(str(sess.eval("return reaper.GetProjectPathEx(0)")))
    before = set(proj_dir.glob("sneakpeak_paste_*.wav"))
    sess.eval("reaper.SelectAllMediaItems(0, true) reaper.UpdateArrange() return true")
    time.sleep(1.0)
    wait_main_thread_idle(sess, timeout=60)
    try:   # the selection poll loads the two items in the background
        sess.wait_until(lambda: mode_from_capture(sess, SHOTS / "multi.png") == "MULTI", timeout=15)
    except Exception:
        raise AssertionError(f"the two selected items did not open a Multi-item view (mode "
                             f"{mode_from_capture(sess, SHOTS / 'multi.png')}, title {window_title(sess)!r}, "
                             f"selected {sess.eval('return reaper.CountSelectedMediaItems(0)')}, "
                             f"items {sess.eval('return reaper.CountMediaItems(0)')})")
    drag_client(sess, 500, WAVE_Y, 600, WAVE_Y)      # ~6.6-7.9 s: the second half of both items
    time.sleep(0.5)
    sess.eval('reaper.DeleteExtState("SneakPeak", "last_toast", false)')
    for _ in range(10):                              # the layers load in the background
        command_sync(sess, CM_COPY, settle=0.5)
        toast = str(sess.eval('return reaper.GetExtState("SneakPeak", "last_toast")'))
        if "loading" not in toast.lower():
            break
        sess.eval('reaper.DeleteExtState("SneakPeak", "last_toast", false)')
        time.sleep(1.0)
    s0, s1 = sess.eval("local s, e = reaper.GetSet_LoopTimeRange2(0, false, false, 0, 0, false) return {s, e}")
    print(f"\n[editops] after Copy: toast {toast!r}, title {window_title(sess)!r}, time sel {float(s0):.2f}-{float(s1):.2f} s, "
          f"items {sess.eval('return reaper.CountMediaItems(0)')}, tracks {sess.eval('return reaper.CountTracks(0)')}")
    click_client(sess, 300, WAVE_Y)                  # cursor: the paste position
    time.sleep(0.3)
    send_command(sess, CM_PASTE)
    try:
        sess.wait_until(lambda: len(set(proj_dir.glob("sneakpeak_paste_*.wav")) - before) >= 1, timeout=20)
    finally:
        time.sleep(0.5)
        toast = str(sess.eval('return reaper.GetExtState("SneakPeak", "last_toast")'))
        print(f"\n[editops] multi copy/paste: toast {toast!r}, clips {sorted(p.name for p in set(proj_dir.glob('sneakpeak_paste_*.wav')) - before)}")
    clip = max(set(proj_dir.glob("sneakpeak_paste_*.wav")) - before, key=lambda p: p.stat().st_mtime)
    got = sf.read(str(clip), dtype="float64", always_2d=True)[0]
    rms = np.sqrt((got ** 2).mean(axis=0))
    want = np.array([0.6, 0.3]) / np.sqrt(2.0)
    print(f"[editops] pasted clip {clip.name}: {got.shape[1]} ch, {len(got) / 44100:.3f} s, RMS {rms.round(4).tolist()} (want {want.round(4).tolist()})")
    assert got.shape[1] == 2, f"the pasted clip has {got.shape[1]} channels, expected 2"
    assert np.abs(rms - want).max() < 0.02 * want.max() + 0.005, "the pasted mix does not match mono + stereo per channel"


# --- A6.7: zero-crossing snap reads the source on long items ----------------
CM_SNAP_ZERO = 2029   # edit_view.h ContextMenuID (toggles + writes ExtState SneakPeak/snap_zero)


def _sine_long_wav(path: Path, *, minutes: float, hz: float, sr: int = 44100) -> Path:
    """Stereo sine at `hz`, amplitude 0.5, 16-bit; the zero crossings sit at
    multiples of 1/(2*hz) s (exact samples when sr/(2*hz) is an integer)."""
    import numpy as np
    if path.exists():
        return path
    n = int(minutes * 60 * sr)
    chunk = sr * 10
    with sf.SoundFile(str(path), "w", samplerate=sr, channels=2, subtype="PCM_16") as f:
        for start in range(0, n, chunk):
            t = (np.arange(min(chunk, n - start)) + start) / sr
            y = 0.5 * np.sin(2 * np.pi * hz * t)
            f.write(np.stack([y, y], axis=1).astype(np.float32))
    return path


def _snap_edges_check(sess, media: Path, *, what: str):
    from conftest import assert_no_loading, command_sync
    clear_project(sess)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=30)
    assert_no_loading(sess, 1.5)
    was_on = str(sess.eval('return reaper.GetExtState("SneakPeak", "snap_zero")')) == "1"
    if not was_on:
        command_sync(sess, CM_SNAP_ZERO, settle=0.3)
    assert str(sess.eval('return reaper.GetExtState("SneakPeak", "snap_zero")')) == "1", "Snap to Zero did not switch on"
    sess.eval("reaper.GetSet_LoopTimeRange2(0, true, false, 0, 0, false) return true")   # no stale time selection
    length = _item_len(sess, 0)
    try:
        drag_client(sess, 200, WAVE_Y, 300, WAVE_Y)
        time.sleep(0.5)
        s0, s1 = (float(v) for v in sess.eval(
            "local s, e = reaper.GetSet_LoopTimeRange2(0, false, false, 0, 0, false) return {s, e}"))
    finally:
        if not was_on:
            command_sync(sess, CM_SNAP_ZERO, settle=0.3)
    period = 1.0 / (2 * 50)                      # a crossing every 10 ms
    err = [abs(t / period - round(t / period)) * period for t in (s0, s1)]
    one_sample = 1.0 / 44100
    # the waveform lane spans ~758 px for the whole item (800x400 window)
    want0, want1 = length * 200 / 758, length * 300 / 758
    print(f"\n[editops] snap on {what}: item {length:.1f} s, selection {s0:.6f}-{s1:.6f} s (raw drag ~{want0:.2f}-{want1:.2f}), "
          f"distance to the nearest crossing {err[0] * 1e6:.1f} / {err[1] * 1e6:.1f} us (one sample = {one_sample * 1e6:.1f} us)")
    tol = 0.03 * length + 0.1
    if not (abs(s0 - want0) < tol and abs(s1 - want1) < tol):
        from conftest import mode_from_capture, window_title
        raise AssertionError(f"precondition: the drag did not select ~{want0:.1f}-{want1:.1f} s ({s0}-{s1}); mode "
                             f"{mode_from_capture(sess, SHOTS / 'snap.png')}, title {window_title(sess)!r}")
    assert max(err) <= 1.5 * one_sample, f"the selection edges did not snap to a zero crossing ({what})"


def test_snap_to_zero_on_long_item(sess):
    """A 5-minute item (no working buffer: lazy) with Snap to Zero on: both
    edges of a dragged selection must land within one sample of a zero
    crossing of the 50 Hz sine (every 10 ms, on exact samples). REAPER's
    time selection, synced on drag end, is the oracle."""
    from conftest import perf_media_dir
    media = _sine_long_wav(perf_media_dir() / "sine50_5min_stereo.wav", minutes=5, hz=50)
    _snap_edges_check(sess, media, what="a 5-min item (lazy)")


def test_snap_to_zero_on_short_item(sess):
    """The same on a 10 s item, which snaps in its full-rate working buffer
    (the search shared with the long-item path)."""
    from conftest import perf_media_dir
    media = _sine_long_wav(perf_media_dir() / "sine50_10s_stereo.wav", minutes=10 / 60, hz=50)
    _snap_edges_check(sess, media, what="a 10 s item (buffer)")
