"""Edit-ops robustness (v2.5.0 audit, increment A6).

A6.1 - Timeline view kept a segment whose item REAPER had deleted (an edit
in the arrange, a script, an undo) and the solo button walked the dead
pointer: `ToggleTrackSolo` asked REAPER for the track of a freed item.
Control (955f5e8): crash or garbage solo. Fixed: dead segments are skipped
(ValidatePtr2, the same guard UpdateSoloState already had).
"""
from __future__ import annotations

import time
from pathlib import Path

from conftest import (SELECT_ITEM0, VK_DELETE, WAVE_Y, _heartbeat_t, burst_fixture, clear_project,
                      click_sync, drag_client, ensure_window, insert_item_unselected, key_sync,
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
