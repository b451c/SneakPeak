"""Long-file responsiveness of the EDIT flows (design_sdk_peaks_hybrid.md
phase 2a): a non-destructive delete splits the item and SneakPeak enters
Timeline view over the survivors; a multi-selection enters Multi-item view.
Both loaded EVERY segment/layer synchronously in v2.4.0 (and Timeline view
refused spans > 600 s, falling back to a full single-item reload).

Observables: REAPER's own item count (the split happened), the mode-bar
accent pixels (which view we are in), the bridge heartbeat (freeze), the
window title (background load progress -> done).
"""
from __future__ import annotations

import json
import time
from pathlib import Path

from conftest import (SELECT_ITEM0, VK_DELETE, WAVE_Y, clear_project, drag_client,
                      ensure_window, insert_item, insert_item_unselected,
                      measure_after, mode_from_capture, perf_media_dir,
                      track_item_count, wait_loaded, write_long_wav)

RESULTS = Path("/tmp/sneakpeak-perf-results.json")
SHOTS = Path("/tmp/sneakpeak-reaproof-shots/edit")
STALL_BUDGET = 0.25
FIRST_PAINT_BUDGET = 0.5


def _record(name: str, m: dict):
    data = json.loads(RESULTS.read_text()) if RESULTS.exists() else {}
    data[name] = m
    RESULTS.write_text(json.dumps(data, indent=1))
    print(f"\n[perf] {name}: {m}")


def _long_wav():
    return write_long_wav(perf_media_dir() / "long20min_stereo.wav", minutes=20)


def test_delete_range_on_long_item_enters_timeline_without_freeze(sess):
    media = _long_wav()
    clear_project(sess)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_loaded(sess, media.stem, timeout=60)
    assert mode_from_capture(sess, SHOTS / "before.png") == "ITEM"

    # select ~25%..62% of the 20-minute item on the waveform, then Delete
    drag_client(sess, 200, WAVE_Y, 500, WAVE_Y)
    time.sleep(0.5)
    # the action here is the key press itself (posted from Lua so the probe
    # gets REAPER's clock for it)
    m = measure_after(sess, "local h = SP_WINDOW() "
                            f'reaper.JS_WindowMessage_Post(h, "WM_KEYDOWN", {VK_DELETE}, 0, 0, 0) '
                            f'reaper.JS_WindowMessage_Post(h, "WM_KEYUP", {VK_DELETE}, 0, 0, 0) return true',
                      loaded_marker=media.stem, max_wait=90, quiet=1.5)
    _record("edit.delete_timeline", m)

    assert track_item_count(sess) == 2, "the delete must split the item into two survivors"
    mode = mode_from_capture(sess, SHOTS / "after_delete.png")
    assert mode == "TIMELINE", f"expected Timeline view over the survivors, got {mode}"
    assert m["max_stall"] <= STALL_BUDGET, f"delete froze REAPER: {m}"


def test_multi_item_select_without_freeze(sess):
    media = _long_wav()
    clear_project(sess)
    insert_item_unselected(sess, media)
    insert_item(sess, media, position=0.0)          # second track, second item
    sess.eval("reaper.SelectAllMediaItems(0, false) reaper.UpdateArrange() return true")
    ensure_window(sess)
    # Multi view keeps the plain title -> no title marker; the freeze is the
    # metric and the mode-bar pixels prove the view switched.
    m = measure_after(sess, "reaper.SelectAllMediaItems(0, true) reaper.UpdateArrange() return true",
                      loaded_marker="\x00never", max_wait=12, quiet=1.0)
    _record("edit.multi_select", m)
    mode = mode_from_capture(sess, SHOTS / "multi.png")
    assert mode == "MULTI", f"expected Multi-item view, got {mode}"
    assert m["max_stall"] <= STALL_BUDGET, f"multi-item load froze REAPER: {m}"
