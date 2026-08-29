"""One-hour item: the upper bound of the long-file mandate (v2.5 increment 8d).

Measure first. Same probes as test_perf_select (main-thread stall from the
bridge heartbeat, first paint from the window title), extended to what a user
does AFTER the load - zoom in/out/fit, cursor jumps - plus REAPER's resident
memory before and after the background load. The working buffer for one hour
of stereo at the 8 kHz downsample floor is ~460 MB of doubles: the number is
recorded so the memory model in the plan is measured, not guessed.
"""
from __future__ import annotations

import json
import subprocess
from pathlib import Path

import pytest

from conftest import (DESELECT_ALL, SELECT_ITEM0, clear_project, ensure_window,
                      insert_item_unselected, measure_after, perf_media_dir,
                      wait_audio_loaded, window_handle_lua, write_long_wav)

RESULTS = Path("/tmp/sneakpeak-perf-results.json")
STALL_BUDGET = 0.25       # s - anything longer reads as a hang
FIRST_PAINT_BUDGET = 0.5  # s - waveform on screen within half a second
RSS_CEILING_MB = 50       # 8g: no working buffer on select (was 1500 = catastrophe guard; +409 MB measured before)
VK_HOME, VK_END = 0x24, 0x23


def _record(name: str, m: dict):
    data = json.loads(RESULTS.read_text()) if RESULTS.exists() else {}
    data[name] = m
    RESULTS.write_text(json.dumps(data, indent=1))
    print(f"\n[perf] {name}: {m}")


def _rss_mb(sess) -> float:
    out = subprocess.check_output(["ps", "-o", "rss=", "-p", str(sess.handle.pid)])
    return int(out.strip()) / 1024.0


def _action(*names: str) -> str:
    calls = " ".join(f'reaper.Main_OnCommand(reaper.NamedCommandLookup("_SneakPeak_{n}"), 0)' for n in names)
    return calls + " return true"


def _key(vk: int) -> str:
    return (f"local h = {window_handle_lua()} "
            f'reaper.JS_WindowMessage_Post(h, "WM_KEYDOWN", {vk}, 0, 0, 0) '
            f'reaper.JS_WindowMessage_Post(h, "WM_KEYUP", {vk}, 0, 0, 0) return true')


@pytest.fixture(scope="module")
def one_hour():
    return write_long_wav(perf_media_dir() / "long60min_stereo.wav", minutes=60)


def test_select_one_hour_item_without_freeze(sess, one_hour):
    clear_project(sess)
    insert_item_unselected(sess, one_hour)
    ensure_window(sess)
    rss0 = _rss_mb(sess)

    first = measure_after(sess, SELECT_ITEM0, loaded_marker=one_hour.stem, max_wait=600)
    first["rss_before_mb"] = round(rss0, 1)
    first["rss_after_mb"] = round(_rss_mb(sess), 1)
    _record("wav60.select", first)

    sess.eval(DESELECT_ALL)
    again = measure_after(sess, SELECT_ITEM0, loaded_marker=one_hour.stem, max_wait=600)
    _record("wav60.reselect", again)

    assert first["max_stall"] <= STALL_BUDGET, f"first select froze: {first}"
    assert first["t_loaded"] is not None, f"audio never finished loading: {first}"
    if first["t_first"] is not None:
        assert first["t_first"] <= FIRST_PAINT_BUDGET, f"waveform late: {first}"
    assert again["max_stall"] <= STALL_BUDGET, f"reselect froze: {again}"
    assert not again["seen_loading"], f"reselect re-decoded the item: {again}"
    assert not first["seen_loading"], f"8g: a one-hour item must not decode a buffer on select: {first}"
    assert first["rss_after_mb"] - first["rss_before_mb"] < RSS_CEILING_MB, f"memory blew up: {first}"


@pytest.mark.parametrize("label,lua", [
    ("zoom_in_x3", _action("ZoomIn", "ZoomIn", "ZoomIn")),
    ("zoom_out", _action("ZoomOut")),
    ("zoom_fit", _action("ZoomFit")),
    ("jump_end", _key(VK_END)),
    ("jump_home", _key(VK_HOME)),
])
def test_view_actions_after_load_do_not_freeze(sess, one_hour, label, lua):
    """Runs on the item loaded by the select spec (module order); each action
    must repaint without a main-thread stall - the buffer path recomputes the
    per-pixel peaks over up to ~4096 subsamples per column, so this is where a
    1-hour buffer would show if that bound ever slipped."""
    wait_audio_loaded(sess, one_hour.stem, timeout=600)
    m = measure_after(sess, lua, loaded_marker=one_hour.stem, max_wait=15, quiet=1.0)
    _record(f"wav60.{label}", m)
    assert m["max_stall"] <= STALL_BUDGET, f"{label} froze REAPER: {m}"


def _apply_on_one_hour(sess, one_hour) -> dict:
    """A7.1 (measure first): Apply Dynamics on the one-hour item. The worker
    had computed the curve for the current knobs; Apply then recomputed every
    trace point on the main thread (ComputeCompression + the RDP simplify)
    before writing the envelope. The panel is opened first and the 1-h trace
    left to stream (title "Analyzing dynamics... N%"; its time is recorded as
    t_trace), then the Apply button is clicked: longest main-thread stall
    between the click and the "Applied N points" toast, on REAPER's clock
    (bridge heartbeat) = perf.apply_1h. BEFORE on the b1c97ed control, AFTER
    on the fix."""
    import threading
    import time
    from conftest import CM_APPLY_DYNAMICS, command_sync, locate_apply_button, window_title
    clear_project(sess)
    insert_item_unselected(sess, one_hour)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, one_hour.stem, timeout=600)
    shots = Path("/tmp/sneakpeak-reaproof-shots/perf_1h")
    shots.mkdir(parents=True, exist_ok=True)
    wall_open = time.monotonic()
    command_sync(sess, CM_APPLY_DYNAMICS, settle=1.0)      # shows the Dynamics panel; the trace starts
    sess.wait_until(lambda: locate_apply_button(sess, shots / "panel.png") is not None, timeout=30)
    # the trace streams on a worker ("Analyzing dynamics... N%" in the title;
    # a fast disk finishes the hour before the panel is even located): wait
    # until the title has been plain for 3 s, recording the trace time if seen
    seen_analysing = False
    t_trace = None
    plain_since = None
    while time.monotonic() - wall_open < 1800:
        title = window_title(sess)
        if "Analyzing" in title:
            seen_analysing = True
            plain_since = None
        else:
            if seen_analysing and t_trace is None:
                t_trace = round(time.monotonic() - wall_open, 1)
            plain_since = plain_since or time.monotonic()
            if time.monotonic() - plain_since > 3.0:
                break
        time.sleep(0.25)
    assert plain_since is not None, f"the dynamics trace of the 1-h item never finished (title {window_title(sess)!r})"
    x, y = locate_apply_button(sess, shots / "panel.png")
    sess.eval('reaper.DeleteExtState("SneakPeak", "last_toast", false)')

    hb = sess.bridge.heartbeat
    samples: list[tuple[int, float]] = []
    stop = threading.Event()

    def probe():
        last = None
        while not stop.is_set():
            try:
                d = json.loads(hb.read_text(encoding="utf-8", errors="replace"))
                tick, t = int(d["tick"]), float(d["t"])
                if tick != last:
                    samples.append((tick, t))
                    last = tick
            except (OSError, ValueError, KeyError, TypeError):
                pass
            time.sleep(0.003)

    th = threading.Thread(target=probe, daemon=True)
    th.start()
    time.sleep(0.3)
    t_click = float(sess.eval(
        f'local h = {window_handle_lua()} '
        f'reaper.JS_WindowMessage_Send(h, "WM_LBUTTONDOWN", 1, 0, {x}, {y}) '
        f'reaper.JS_WindowMessage_Send(h, "WM_LBUTTONUP", 0, 0, {x}, {y}) '
        'return reaper.time_precise()', hang_timeout=300))
    toast = ""
    wall0 = time.monotonic()
    while time.monotonic() - wall0 < 300:
        toast = str(sess.eval('return reaper.GetExtState("SneakPeak", "last_toast")'))
        if toast.startswith(("Applied", "Envelope simplified")):
            break
        time.sleep(0.5)
    t_done = float(sess.eval("return reaper.time_precise()"))
    stop.set()
    th.join(timeout=2)
    max_stall = 0.0
    for (k0, t0), (k1, t1) in zip(samples, samples[1:]):
        if t1 <= t_click or k1 <= k0:
            continue
        max_stall = max(max_stall, (t1 - t0) / (k1 - k0))
    points = int(sess.eval("local it = reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), 0) "
                           "local env = reaper.GetTakeEnvelopeByName(reaper.GetActiveTake(it), 'Volume') "
                           "return env and reaper.CountEnvelopePoints(env) or 0"))
    m = {"max_stall": round(max_stall, 3), "t_toast": round(t_done - t_click, 2), "t_trace": t_trace,
         "toast": toast, "ticks": len(samples), "points": points}
    _record("perf.apply_1h", m)
    assert toast.startswith("Applied") or toast.startswith("Envelope simplified"), f"Apply never finished: {m}"
    return m


def test_apply_dynamics_on_one_hour_item(sess, one_hour):
    """A7.1 (measured first): longest main-thread stall between the Apply click
    and its toast on the one-hour item. 0.699 s on the b1c97ed control (the RDP
    simplify of 3.6 M trace points on the main thread), 0.051 s with the curve
    built on the analysis worker."""
    m = _apply_on_one_hour(sess, one_hour)
    assert m["max_stall"] <= STALL_BUDGET, f"Apply Dynamics froze REAPER on the 1-h item: {m}"


def test_apply_point_budget_on_one_hour_item(sess, one_hour):
    """A7.2: the hour simplifies to 29844 points at 0.3 dB (control d1d4b5b),
    which REAPER's envelope handles slowly; the tolerance now widens until the
    curve is under 20000 points and the toast says so."""
    m = _apply_on_one_hour(sess, one_hour)
    assert m["points"] <= 20000, f"the envelope got {m['points']} points (budget 20000): {m}"
    assert "tolerance" in m["toast"], f"no tolerance toast although the budget applied: {m}"
