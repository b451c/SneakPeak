"""Per-item UI memory (s20, user): the spectral view and the zoom you had on an
item come back when you click another item and return - a process started on
an item continues where it was. Session-only, keyed by the item's GUID.

Oracles: the ExtState mirror SneakPeak/ui_state ("spectral=1" and
"view=<start>,<duration>").
"""
from __future__ import annotations

import time
from pathlib import Path

from conftest import (clear_project, ensure_window, insert_item_unselected,
                      perf_media_dir, send_command, wait_audio_loaded, window_title, write_long_wav)

CM_ZOOM_IN, CM_TOGGLE_SPECTRAL = 2017, 2028   # edit_view.h ContextMenuID (compiled enum)
SHOTS = Path("/tmp/sneakpeak-reaproof-shots/item_ui_memory")
# insert_item puts every item on a NEW track on top: after A then B, track 0 = B, track 1 = A.
def _select_track_item(track: int) -> str:
    return ("reaper.SelectAllMediaItems(0, false) "
            f"local it = reaper.GetTrackMediaItem(reaper.GetTrack(0, {track}), 0) "
            "reaper.SetMediaItemSelected(it, true) reaper.UpdateArrange() return true")


SELECT_A, SELECT_B = _select_track_item(1), _select_track_item(0)


def _ui(sess) -> dict[str, str]:
    raw = str(sess.eval('return reaper.GetExtState("SneakPeak", "ui_state")'))
    return dict(tok.split("=", 1) for tok in raw.split() if "=" in tok)


def _view(sess) -> tuple[float, float]:
    v = _ui(sess).get("view", "0,0").split(",")
    return float(v[0]), float(v[1])


def test_spectral_and_zoom_come_back_on_reselect(sess):
    a = write_long_wav(perf_media_dir() / "uimem_a_15s.wav", minutes=0.25)
    b = write_long_wav(perf_media_dir() / "uimem_b_15s.wav", minutes=0.25)
    clear_project(sess)
    insert_item_unselected(sess, a, position=0.0)
    insert_item_unselected(sess, b, position=20.0)
    ensure_window(sess)
    sess.eval(SELECT_A)
    wait_audio_loaded(sess, a.stem, timeout=60)
    try:
        if _ui(sess).get("spectral") != "1":
            send_command(sess, CM_TOGGLE_SPECTRAL)
        sess.wait_until(lambda: _ui(sess).get("spectral") == "1", timeout=5)
        sess.wait_until(lambda: "Computing" not in window_title(sess), timeout=60)
        send_command(sess, CM_ZOOM_IN)
        send_command(sess, CM_ZOOM_IN)
        time.sleep(0.5)
        view_a = _view(sess)
        assert 0.0 < view_a[1] < 14.0, f"zoom in did not narrow the view: {view_a}"

        sess.eval(SELECT_B)
        wait_audio_loaded(sess, b.stem, timeout=60)
        time.sleep(0.5)
        assert _ui(sess).get("spectral") == "0", "a fresh item inherited the spectral view"

        sess.eval(SELECT_A)
        wait_audio_loaded(sess, a.stem, timeout=60)
        time.sleep(0.8)
        assert _ui(sess).get("spectral") == "1", "the spectral view did not come back on the item that had it"
        view_a2 = _view(sess)
        assert abs(view_a2[0] - view_a[0]) < 0.01 and abs(view_a2[1] - view_a[1]) < 0.01, \
            f"the zoom did not come back: {view_a} -> {view_a2}"
    finally:
        if _ui(sess).get("spectral") == "1":
            send_command(sess, CM_TOGGLE_SPECTRAL)
