"""Dynamics analysed from the stream (v2.5 increment 8f).

Item views used to analyse the working buffer, which is DOWNSAMPLED above the
10M-frame cap (8 kHz for one hour): the detector saw band-limited audio with
flattened peaks. Now the view streams the source at full rate into the
analysis (design_dynamics_stream.md). Ground truth is always REAPER's own:
the take envelope points Apply wrote (API) and the TRACK audio accessor
(what the listener hears).

Specs:
  1. streamed == buffered: on a 3-minute full-rate item the envelope Apply
     writes must be IDENTICAL (point for point) to the one the buffer path
     wrote - recorded from the 8912905 control build first:
       SNEAKPEAK_DYLIB=<control> SNEAKPEAK_DYN_RECORD=control rp.sh ...::test_streamed_equals_buffered_3min
     then the new build compares against that record, which lives in
     tests/reaproof/records/ (the spec FAILS when it is absent).
  2. one hour: a 12 kHz tone burst near the end of a 1-h item is compressed
     (the 8 kHz buffer band-limits it away: control RED), RSS delta recorded,
     the Apply pressed before the analysis lands fires when it does, no stall.
  3. perf: test_perf_slider's THRESH drag on the 1-h item stays at input rate.
"""
from __future__ import annotations

import json
import os
import subprocess
import time
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

from conftest import (CM_APPLY_DYNAMICS, SELECT_ITEM0, apply_dynamics, assert_no_loading, burst_fixture,
                      clear_project, click_client, db, ensure_window,
                      insert_item_unselected, locate_apply_button, measure_after,
                      perf_media_dir, send_command, take_envelope_points,
                      track_rms_windows, wait_audio_loaded, window_handle_lua,
                      window_title)
from test_perf_slider import (PER_MOVE_BUDGET, STALL_BUDGET, _knob_drag_lua,
                              _panel_origin, cell_center, tab_center)

RESULTS = Path("/tmp/sneakpeak-perf-results.json")
SHOTS = Path("/tmp/sneakpeak-reaproof-shots/dynstream")
RECORD_DIR = Path(__file__).resolve().parent / "records"   # in the repo: the control is evidence, not a temp file
SR = 44100
FLOOR_AMP, BURST_AMP, HF_AMP = 0.03, 0.9, 0.8
WB_BURST = (1800.0, 1801.0)     # wideband 220 Hz burst at 30:00
HF_BURST = (3540.0, 3541.0)     # 12 kHz tone burst at 59:00


def _record(name: str, m: dict):
    data = json.loads(RESULTS.read_text()) if RESULTS.exists() else {}
    data[name] = m
    RESULTS.write_text(json.dumps(data, indent=1))
    print(f"\n[perf] {name}: {m}")


def _rss_mb(sess) -> float:
    from conftest import rss_mb          # ps on macOS/Linux, GetProcessMemoryInfo on Windows
    return rss_mb(sess)


def _wait_title_settled(sess, name: str, timeout: float):
    """The trace job retitles 'Analyzing dynamics... N%' while it streams;
    the plain item title (no Loading, no Analyzing) means the analysis landed."""
    def done():
        t = window_title(sess)
        return name in t and "Loading" not in t and "Analyzing" not in t
    sess.wait_until(done, timeout=timeout)


# ---------------------------------------------------------------------------
# 1. streamed == buffered (equivalence, same on both builds)
# ---------------------------------------------------------------------------
def test_streamed_equals_buffered_3min(sess):
    media = burst_fixture("long3min_burst24.wav", seconds=180, channels=2)
    clear_project(sess)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=90)
    apply_dynamics(sess, SHOTS / "eq3min")
    _wait_title_settled(sess, media.stem, timeout=30)
    pts = take_envelope_points(sess)
    assert len(pts) > 4

    RECORD_DIR.mkdir(parents=True, exist_ok=True)
    tag = os.environ.get("SNEAKPEAK_DYN_RECORD")
    if tag:
        (RECORD_DIR / f"envelope_3min.{tag}.json").write_text(json.dumps(pts))
        print(f"\n[record] {len(pts)} points -> envelope_3min.{tag}.json")
        return
    control = RECORD_DIR / "envelope_3min.control.json"
    # Recorded 2026-08-28 from the 8912905 build (the buffer path) and kept in
    # the repo: a missing control is a broken checkout, not a reason to skip (A7.7).
    assert control.exists(), f"control record missing: {control} (record it from the 8912905 build with SNEAKPEAK_DYN_RECORD=control)"
    want = [tuple(p) for p in json.loads(control.read_text())]
    assert len(pts) == len(want), f"{len(pts)} points vs control {len(want)}"
    def same(a, b):   # bit-for-bit on the recording platform; last-bit slack for another compiler (MSVC arm64)
        return all(abs(x - y) <= 1e-9 * max(1.0, abs(y)) for x, y in zip(a, b))
    diffs = [(i, a, b) for i, (a, b) in enumerate(zip(pts, want)) if not same(a, b)]
    assert not diffs, f"{len(diffs)} points differ from the buffer-path control, first: {diffs[:3]}"


# ---------------------------------------------------------------------------
# 2. one hour at full fidelity
# ---------------------------------------------------------------------------
@pytest.fixture(scope="module")
def one_hour_hf() -> Path:
    """1 h stereo 24-bit: 220 Hz floor at 0.03, a 0.9 wideband burst at 30:00
    and a 12 kHz tone burst at 0.8 at 59:00 (the 8 kHz buffer cannot see it)."""
    path = perf_media_dir() / "long60min_hf_burst24.wav"
    if path.exists():
        return path
    with sf.SoundFile(str(path), "w", samplerate=SR, channels=2, subtype="PCM_24") as f:
        for start in range(0, 3600 * SR, SR * 10):
            t = (np.arange(SR * 10) + start) / SR
            y = FLOOR_AMP * np.sin(2 * np.pi * 220 * t)
            wb = (t >= WB_BURST[0]) & (t < WB_BURST[1])
            y[wb] = BURST_AMP * np.sin(2 * np.pi * 220 * t[wb])
            hf = (t >= HF_BURST[0]) & (t < HF_BURST[1])
            y[hf] = HF_AMP * np.sin(2 * np.pi * 12000 * t[hf])
            f.write(np.repeat(y[:, None], 2, axis=1).astype(np.float32))
    return path


def test_apply_one_hour_hf_burst_at_full_fidelity(sess, one_hour_hf):
    media = one_hour_hf
    clear_project(sess)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=600)
    assert_no_loading(sess, 2.0)   # 8g: Dynamics works with NO working buffer (this is the point)
    rss0 = _rss_mb(sess)

    # Open the panel (analysis streams in the background) and press Apply
    # BEFORE it lands: the apply is pending and must fire when it does,
    # without freezing the main thread meanwhile.
    send_command(sess, CM_APPLY_DYNAMICS)
    sess.wait_until(lambda: locate_apply_button(sess, SHOTS / "panel1h.png") is not None, timeout=15)
    ax, ay = locate_apply_button(sess, SHOTS / "panel1h.png")
    h = window_handle_lua()
    click_lua = (f"local h = {h} if not h then return nil end "
                 f'reaper.JS_WindowMessage_Send(h,"WM_LBUTTONDOWN",1,0,{ax},{ay}) '
                 f'reaper.JS_WindowMessage_Send(h,"WM_LBUTTONUP",0,0,{ax},{ay}) return true')
    t0 = time.monotonic()
    m = measure_after(sess, click_lua, loaded_marker=media.stem, max_wait=120, quiet=1.0)
    sess.wait_until(lambda: len(take_envelope_points(sess)) > 4, timeout=120)
    m["apply_to_points_s"] = round(time.monotonic() - t0, 2)
    m["rss_before_mb"] = round(rss0, 1)
    m["rss_after_mb"] = round(_rss_mb(sess), 1)
    m["points"] = len(take_envelope_points(sess))
    _record("dynstream.wav60_apply", m)

    # Ground truth: the track output. Per-window gain vs the source so item
    # volume and auto-makeup cancel; only the envelope shape remains.
    w_wb = (WB_BURST[0] + 0.08, WB_BURST[1] - 0.08)
    w_hf = (HF_BURST[0] + 0.08, HF_BURST[1] - 0.08)
    w_floor_a = (WB_BURST[1] + 1.0, WB_BURST[1] + 3.0)
    w_floor_b = (HF_BURST[1] + 1.0, HF_BURST[1] + 3.0)
    rms = track_rms_windows(sess, [w_wb, w_hf, w_floor_a, w_floor_b])
    src = [BURST_AMP / np.sqrt(2), HF_AMP / np.sqrt(2), FLOOR_AMP / np.sqrt(2), FLOOR_AMP / np.sqrt(2)]
    g_wb, g_hf, g_fa, g_fb = (db(o) - db(s) for o, s in zip(rms, src))
    print(f"\n[dynstream] gains: wideband {g_wb:+.1f}, 12k {g_hf:+.1f}, floor {g_fa:+.1f}/{g_fb:+.1f} dB")
    assert g_wb - g_fa <= -6.0, f"wideband burst not compressed: {g_wb:+.1f} vs floor {g_fa:+.1f}"
    assert g_hf - g_fb <= -6.0, (
        f"12 kHz burst not compressed ({g_hf:+.1f} vs floor {g_fb:+.1f}): the detector saw band-limited audio")
    assert abs(g_fa - g_fb) <= 1.0, (g_fa, g_fb)
    assert m["max_stall"] <= 0.35, f"Apply froze REAPER while the analysis streamed: {m}"
    assert m["rss_after_mb"] - m["rss_before_mb"] < 400, f"analysis memory: {m}"


# ---------------------------------------------------------------------------
# 3. knob drag on the 1-h item stays at input rate (test_perf_slider, 1 h)
# ---------------------------------------------------------------------------
def test_thresh_knob_drag_tracks_input_on_one_hour_item(sess, one_hour_hf):
    media = one_hour_hf
    clear_project(sess)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=600)

    if locate_apply_button(sess, SHOTS / "probe1h.png") is None:   # 2058 toggles the panel
        send_command(sess, CM_APPLY_DYNAMICS)
    sess.wait_until(lambda: locate_apply_button(sess, SHOTS / "probe1h.png") is not None, timeout=15)
    _wait_title_settled(sess, media.stem, timeout=120)
    px, py = _panel_origin(sess)

    tx, ty = tab_center(3)
    click_client(sess, int(px + tx), int(py + ty))
    lx, ly = cell_center(1, 0)
    click_client(sess, int(px + lx), int(py + ly))
    sess.wait_until(lambda: len(take_envelope_points(sess)) > 4, timeout=60)
    before = take_envelope_points(sess)
    cx, cy = tab_center(0)
    click_client(sess, int(px + cx), int(py + cy))
    kx, ky = cell_center(0, 0)
    kx, ky = int(px + kx), int(py + ky)

    steps = 24
    drag_lua = _knob_drag_lua(kx, ky, ky + 90, steps)
    m = measure_after(sess, drag_lua.replace("return reaper.time_precise() - t0",
                                             "DRAG_T = reaper.time_precise() - t0 return true"),
                      loaded_marker="\x00never", max_wait=10, quiet=0.5)
    drag_t = float(sess.eval("return DRAG_T or -1"))
    m["per_move"] = round(drag_t / steps, 4)
    m["drag_total"] = round(drag_t, 3)
    _record("slider.thresh_live_1h", m)

    sess.wait_until(lambda: take_envelope_points(sess) != before, timeout=60)
    assert m["per_move"] <= PER_MOVE_BUDGET, f"knob lags the mouse on 1 h: {m}"
    assert m["max_stall"] <= STALL_BUDGET, f"drag froze REAPER on 1 h: {m}"
