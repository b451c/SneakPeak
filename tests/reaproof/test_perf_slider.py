"""Dynamics knob drag on a long item must track the mouse (phase 2b).

v2.4.0 ran Analyze -> ComputeCompression -> (Live) SimplifyCurve + envelope
writes INLINE on every mouse-move: 250-400 ms per tick on a 17-minute item,
i.e. the slider updated at 3-4 Hz (profile_2026-07-09_longfile.md). Target:
the mouse-move returns immediately (engine on a worker, latest value wins,
Live writes debounced), so per-move cost is a frame, not a pipeline.

Driver: the panel is positioned from the pixel-located Apply button and the
premium layout math (ComputeDynLayout @ 480x300, scale 1.0); the knob drag
is a synchronous JS_WindowMessage_Send sequence timed on REAPER's clock.
Effect: Live arms (envelope curve appears) and the drag changes the curve.
"""
from __future__ import annotations

import json
from pathlib import Path

from conftest import (CM_APPLY_DYNAMICS, SELECT_ITEM0, clear_project, click_client,
                      ensure_window, insert_item_unselected, locate_apply_button,
                      measure_after, perf_media_dir, send_command,
                      take_envelope_points, wait_audio_loaded, window_handle_lua,
                      write_long_wav)

RESULTS = Path("/tmp/sneakpeak-perf-results.json")
SHOTS = Path("/tmp/sneakpeak-reaproof-shots/slider")
PER_MOVE_BUDGET = 0.035     # s per mouse-move (~input rate)
STALL_BUDGET = 0.35         # s - the debounced Live write may still cost one tick

# --- premium panel geometry (ui_render.cpp ComputeDynLayout, Normal mode) ---
PANEL_W, PANEL_H, PAD, HEADER_H, FOOTER_H = 480.0, 300.0, 16.0, 44.0, 44.0
METER_BAR_W, METER_GAP, KNOB_GRID_GAP, KNOB_COL_GAP, KNOB_COLS, KNOB_ROWS = 18.0, 14.0, 18.0, 8.0, 2, 4
FOOTER_Y = PANEL_H - FOOTER_H
F_MID = FOOTER_Y + FOOTER_H * 0.5
BODY_TOP, BODY_H = HEADER_H, FOOTER_Y - HEADER_H
PLOT_SIDE = max(40.0, min(BODY_H - 2 * PAD, (PANEL_W - 2 * PAD) * 0.40))
PLOT_Y = BODY_TOP + BODY_H * 0.5 - PLOT_SIDE * 0.5
GRID_X = PAD + PLOT_SIDE + METER_GAP + METER_BAR_W + KNOB_GRID_GAP
CELL_W = (PANEL_W - PAD - GRID_X - KNOB_COL_GAP * (KNOB_COLS - 1)) / KNOB_COLS
CELL_H = PLOT_SIDE / KNOB_ROWS
TAB_W = [60.0, 58.0, 80.0, 52.0]
PILL_X = PANEL_W - PAD - (sum(TAB_W) + 3 * 2.0)
APPLY_CENTER = (PAD + 42.0, F_MID)          # L.apply = {pad, fMid-12, 84, 24}


def cell_center(col: int, row: int) -> tuple[float, float]:
    return (GRID_X + col * (CELL_W + KNOB_COL_GAP) + CELL_W * 0.5,
            PLOT_Y + row * CELL_H + CELL_H * 0.5)


def tab_center(i: int) -> tuple[float, float]:
    x = PILL_X + sum(TAB_W[:i]) + 2.0 * i
    return (x + TAB_W[i] * 0.5, F_MID)


def _record(name: str, m: dict):
    data = json.loads(RESULTS.read_text()) if RESULTS.exists() else {}
    data[name] = m
    RESULTS.write_text(json.dumps(data, indent=1))
    print(f"\n[perf] {name}: {m}")


def _panel_origin(sess) -> tuple[int, int]:
    ax, ay = locate_apply_button(sess, SHOTS / "panel.png")
    return int(round(ax - APPLY_CENTER[0])), int(round(ay - APPLY_CENTER[1]))


def _knob_drag_lua(x: int, y0: int, y1: int, steps: int) -> str:
    moves = " ".join(f'reaper.JS_WindowMessage_Send(h,"WM_MOUSEMOVE",1,0,{x},{int(y0 + (y1 - y0) * i / steps)})'
                     for i in range(1, steps + 1))
    return (f"local h = {window_handle_lua()} if not h then return nil end "
            f"local t0 = reaper.time_precise() "
            f'reaper.JS_WindowMessage_Send(h,"WM_LBUTTONDOWN",1,0,{x},{y0}) {moves} '
            f'reaper.JS_WindowMessage_Send(h,"WM_LBUTTONUP",0,0,{x},{y1}) '
            f"return reaper.time_precise() - t0")


def test_thresh_knob_drag_tracks_input_on_long_item(sess):
    media = write_long_wav(perf_media_dir() / "long20min_stereo.wav", minutes=20)
    clear_project(sess)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=90)

    send_command(sess, CM_APPLY_DYNAMICS)
    sess.wait_until(lambda: locate_apply_button(sess, SHOTS / "probe.png") is not None, timeout=15)
    px, py = _panel_origin(sess)

    # VIEW tab -> LIVE pill: arming Live performs an initial apply (effect: curve on the envelope)
    tx, ty = tab_center(3)
    click_client(sess, int(px + tx), int(py + ty))
    lx, ly = cell_center(1, 0)
    click_client(sess, int(px + lx), int(py + ly))
    sess.wait_until(lambda: len(take_envelope_points(sess)) > 4, timeout=20)
    before = take_envelope_points(sess)
    # back to the COMP tab for the THRESH knob (cell 0,0)
    cx, cy = tab_center(0)
    click_client(sess, int(px + cx), int(py + cy))
    kx, ky = cell_center(0, 0)
    kx, ky = int(px + kx), int(py + ky)

    steps = 24
    drag_lua = _knob_drag_lua(kx, ky, ky + 90, steps)
    m = measure_after(sess, drag_lua.replace("return reaper.time_precise() - t0",
                                             "DRAG_T = reaper.time_precise() - t0 return true"),
                      loaded_marker="\x00never", max_wait=6, quiet=0.5)
    drag_t = float(sess.eval("return DRAG_T or -1"))
    m["per_move"] = round(drag_t / steps, 4)
    m["drag_total"] = round(drag_t, 3)
    _record("slider.thresh_live", m)

    after = take_envelope_points(sess)
    assert after != before, "the THRESH drag must change the Live-written envelope"
    assert m["per_move"] <= PER_MOVE_BUDGET, f"knob lags the mouse: {m}"
    assert m["max_stall"] <= STALL_BUDGET, f"drag froze REAPER: {m}"
