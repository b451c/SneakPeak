"""Per-track zoom in the Multi-item view (v2.5.0 row 15 #2, forum #68).

The Layered modes overlay every layer on the same channel bands and share
ONE vertical zoom. The new "Lanes (per Track)" mode stacks each track in
its own horizontal band, and the wheel over the dB column of a lane (or
Alt+wheel over it) zooms THAT track only.
Oracle 1 (layout): with two mono tracks in Lanes mode the rows carrying
track 0's colour (green) all lie above the rows carrying track 1's (cyan).
Oracle 2 (zoom): three wheel notches on lane 0's dB column grow the green
extent by 1.15^3 while the cyan extent does not move.
Oracle 3 (rebuild): a split made outside SneakPeak rebuilds the view from
the project (guide: the view follows external changes); lane 0's zoom must
survive. Oracle 4 (order): four tracks whose item positions
disagree with the track order stack in ARRANGE order (green, cyan, magenta,
yellow top to bottom), folded to one band each in an 800x400 window.
Control (8b925b3): the Lanes command id is unknown, the view stays in
Layered (per Track) with the layers overlaid (row ranges overlap) - and the
dB-column wheel would zoom both.
"""
from __future__ import annotations

import itertools
import time
from pathlib import Path

import numpy as np
import soundfile as sf

from conftest import (capture, clear_project, client_size, command_sync, ensure_window, insert_item,
                      insert_item_unselected, mode_from_capture, perf_media_dir, send_sync,
                      wait_main_thread_idle, window_handle_lua, window_title)

SHOTS = Path("/tmp/sneakpeak-reaproof-shots/multi_lanes")
CM_MULTI_MODE_LAYERED_TRACKS = 2039  # edit_view.h ContextMenuID
CM_MULTI_MODE_LANES = 2259          # appended right before CM_LAST (CM_PRESET_BASE + 10 sits in between)
GREEN, CYAN = (0, 200, 80), (0, 200, 200)   # kLayerColors[0] / [1]: track 0 / track 1
MAGENTA, YELLOW = (200, 0, 200), (200, 200, 0)   # kLayerColors[2] / [3]
X0, X1 = 150, 600                   # capture columns: right of the lane label, left of the dB column
NOTCHES = 3
SR = 44100
_N = itertools.count(1)


def _tone(name: str, amp: float, channels: int = 1) -> Path:
    """220 Hz sine at `amp`, 10 s, float32; unique name per call."""
    path = perf_media_dir() / f"{name}_{next(_N)}.wav"
    t = np.arange(10 * SR) / SR
    y = np.stack([amp * np.sin(2 * np.pi * 220 * t)] * channels, axis=1)
    sf.write(str(path), y.astype("float32"), SR, subtype="FLOAT")
    return path


def _open_multi(sess):
    """Select every item -> Multi-item view -> Lanes (per Track)."""
    sess.eval("reaper.SelectAllMediaItems(0, false) reaper.UpdateArrange() return true")
    ensure_window(sess)
    sess.eval("reaper.SelectAllMediaItems(0, true) reaper.UpdateArrange() return true")
    time.sleep(1.0)
    wait_main_thread_idle(sess, timeout=60)
    try:
        sess.wait_until(lambda: mode_from_capture(sess, SHOTS / "multi.png") == "MULTI", timeout=15)
    except Exception:
        raise AssertionError(f"the selected items did not open a Multi-item view (mode "
                             f"{mode_from_capture(sess, SHOTS / 'multi.png')}, title {window_title(sess)!r})")
    command_sync(sess, CM_MULTI_MODE_LAYERED_TRACKS, settle=0.5)   # the control's closest mode: overlaid
    command_sync(sess, CM_MULTI_MODE_LANES, settle=1.0)
    wait_main_thread_idle(sess, timeout=30)


def _hue_rows(img: np.ndarray, rgb: tuple[int, int, int], x0: int, x1: int) -> np.ndarray:
    """Capture rows (indices) holding >= 3 pixels of the given hue inside the
    column window. The waveform bg is black, so the 0.7 / 0.9 layer blends
    keep the hue direction exactly (cosine match like mode_from_capture)."""
    band = img[:, x0:x1].astype(float)
    sat = band.max(axis=2) - band.min(axis=2)
    norms = band / np.maximum(np.linalg.norm(band, axis=2, keepdims=True), 1.0)
    ref = np.array(rgb, float)
    ref /= np.linalg.norm(ref)
    hit = ((norms @ ref) > 0.995) & (sat > 40)
    return np.nonzero(hit.sum(axis=1) >= 3)[0]


def _lanes(sess, out: Path) -> tuple[np.ndarray, np.ndarray, int, float]:
    """(green rows, cyan rows, titlebar rows, capture scale) for the current frame."""
    cap = capture(sess, out)
    cw, ch = client_size(sess)
    scale = cap.image.shape[1] / float(cw)
    titlebar = int(round(cap.height - ch * scale))
    g = _hue_rows(cap.image, GREEN, int(X0 * scale), int(X1 * scale))
    c = _hue_rows(cap.image, CYAN, int(X0 * scale), int(X1 * scale))
    return g, c, titlebar, scale


def _extent(rows: np.ndarray) -> int:
    return int(rows.max() - rows.min() + 1) if len(rows) else 0


def _wheel_at(sess, x: int, y: int, notches: int):
    """WM_MOUSEWHEEL carries SCREEN coordinates (edit_view.cpp ScreenToClient)."""
    sx, sy = sess.eval(f"local h = {window_handle_lua()} "
                       f"local sx, sy = reaper.JS_Window_ClientToScreen(h, {int(x)}, {int(y)}) "
                       f"return {{sx, sy}}")
    send_sync(sess, "WM_MOUSEWHEEL", (notches * 120) << 16, int(sx), int(sy), settle=0.8)


def test_wheel_on_a_lane_zooms_that_track_only(sess):
    a = _tone("lanes_track1", 0.15)               # ends up on track 1 = cyan (insert_item adds a track per call)
    b = _tone("lanes_track0", 0.3)                # track 0 = green, drawn first: the larger extent shows around the cyan
    clear_project(sess)
    insert_item_unselected(sess, a)
    insert_item(sess, b, position=0.0)          # a new track 0; item `a` is on track 1 now
    _open_multi(sess)

    g, c, titlebar, scale = _lanes(sess, SHOTS / "lanes.png")
    print(f"\n[lanes] scale {scale:g}, titlebar {titlebar}px, green rows "
          f"{(g.min(), g.max()) if len(g) else None}, cyan rows {(c.min(), c.max()) if len(c) else None}")
    assert len(g) and len(c), (f"no lane colours in the capture (green {len(g)} rows, cyan {len(c)} rows) - "
                               f"the view is not in a per-track layered layout")
    assert g.max() < c.min(), (f"the tracks are not stacked in lanes: green rows {g.min()}-{g.max()} overlap "
                               f"cyan rows {c.min()}-{c.max()} (overlaid layers)")
    e0, e1 = _extent(g), _extent(c)

    cw, ch = client_size(sess)
    yc = int(round((g.min() + g.max()) / 2.0 - titlebar) / scale)
    _wheel_at(sess, cw - 10, yc, NOTCHES)       # the dB column of lane 0
    g2, c2, _, _ = _lanes(sess, SHOTS / "lanes_zoomed.png")
    f0 = _extent(g2) / float(e0)
    f1 = _extent(c2) / float(e1) if e1 else 0.0
    want = 1.15 ** NOTCHES
    print(f"[lanes] wheel x{NOTCHES} on lane 0's dB column (client {cw - 10},{yc}): "
          f"green {e0} -> {_extent(g2)} px (x{f0:.2f}, want x{want:.2f}), cyan {e1} -> {_extent(c2)} px (x{f1:.2f})")
    assert abs(f0 - want) < 0.15 * want, f"lane 0 did not zoom by 1.15^{NOTCHES} (x{f0:.2f})"
    assert abs(f1 - 1.0) < 0.05, f"lane 1 moved with lane 0's wheel (x{f1:.2f}) - the zoom is not per track"

    # Oracle 3: a change made outside SneakPeak (a script splits the track-0 item)
    # rebuilds the Multi-item view from the project; lane 0 keeps its zoom
    sess.eval("local it = reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), 0) "
              "reaper.SplitMediaItem(it, 5.0) reaper.SelectAllMediaItems(0, true) "
              "reaper.UpdateArrange() return true")
    time.sleep(1.5)
    wait_main_thread_idle(sess, timeout=30)
    g3, c3, _, _ = _lanes(sess, SHOTS / "lanes_after_split.png")
    items = sess.eval("return reaper.CountMediaItems(0)")
    print(f"[lanes] after an external split ({items} items, mode {mode_from_capture(sess, SHOTS / 'mode.png')}): "
          f"green {_extent(g2)} -> {_extent(g3)} px, cyan {_extent(c2)} -> {_extent(c3)} px")
    assert int(items) == 3, f"the split did not land ({items} items)"
    assert len(g3) and len(c3) and g3.max() < c3.min(), "the rebuilt view lost its lanes"
    assert abs(_extent(g3) / float(_extent(g2)) - 1.0) < 0.1, "lane 0 lost its zoom in the rebuild"
    assert abs(_extent(c3) / float(_extent(c2)) - 1.0) < 0.1, "lane 1 changed in the rebuild"


def test_four_tracks_stack_in_arrange_order(sess):
    """Items inserted at 1, 0, 3, 2 s: each insert_item adds a track at the top,
    so the arrange order (top to bottom) is D(2 s), C(3 s), B(0 s), A(1 s) while
    the items sorted by position read B, A, D, C. The lanes must follow the
    arrange order - green, cyan, magenta, yellow from the top - and, four
    stereo lanes in a 400 px window, fold to one band each (no split)."""
    clear_project(sess)
    insert_item_unselected(sess, _tone("lanes_A", 0.3, 2), position=1.0)
    insert_item(sess, _tone("lanes_B", 0.3, 2), position=0.0)
    insert_item(sess, _tone("lanes_C", 0.3, 2), position=3.0)
    insert_item(sess, _tone("lanes_D", 0.3, 2), position=2.0)
    _open_multi(sess)
    cap = capture(sess, SHOTS / "lanes_four.png")
    cw, _ = client_size(sess)
    scale = cap.image.shape[1] / float(cw)
    rows = [_hue_rows(cap.image, rgb, int(X0 * scale), int(X1 * scale))
            for rgb in (GREEN, CYAN, MAGENTA, YELLOW)]
    spans = [(int(r.min()), int(r.max())) if len(r) else None for r in rows]
    print(f"\n[lanes] four stereo tracks, row spans top to bottom: {spans}")
    assert all(spans), f"a lane colour is missing: {spans}"
    for i in range(3):
        assert spans[i][1] < spans[i + 1][0], (f"lane {i} ({spans[i]}) is not above lane {i + 1} "
                                               f"({spans[i + 1]}) - lanes do not follow the arrange order")
    # Folded lanes: one contiguous band per colour (a split lane would show two runs)
    for i, r in enumerate(rows):
        runs = int((np.diff(r) > 1).sum()) + 1
        assert runs == 1, f"lane {i} shows {runs} bands - a 70 px stereo lane must fold to one"
