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
