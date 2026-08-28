"""Long-file responsiveness: selecting an item must not freeze REAPER.

v2.4.0 decodes the WHOLE take synchronously on every (re)select
(profile_2026-07-09_longfile.md: 7-10 s on a 17-min AAC). The v2.5 target:
the waveform appears from REAPER's own peaks at once and the audio streams in
the background - the main thread never stalls longer than a frame budget.

Metrics come from the bridge heartbeat (REAPER's defer loop): the longest
gap between two heartbeat writes IS the longest main-thread freeze.
"""
from __future__ import annotations

import json
from pathlib import Path

import pytest

from conftest import (DESELECT_ALL, SELECT_ITEM0, clear_project, ensure_window,
                      insert_item_unselected, measure_after, perf_media_dir,
                      wait_loaded, write_long_aac, write_long_wav)

RESULTS = Path("/tmp/sneakpeak-perf-results.json")
STALL_BUDGET = 0.25       # s - anything longer reads as a hang
FIRST_PAINT_BUDGET = 0.5  # s - waveform on screen (title left idle) within half a second


def _record(name: str, m: dict):
    data = json.loads(RESULTS.read_text()) if RESULTS.exists() else {}
    data[name] = m
    RESULTS.write_text(json.dumps(data, indent=1))
    print(f"\n[perf] {name}: {m}")


@pytest.fixture(scope="module")
def long_wav():
    return write_long_wav(perf_media_dir() / "long20min_stereo.wav", minutes=20)


@pytest.fixture(scope="module")
def long_aac():
    return write_long_aac(perf_media_dir() / "long17min.m4a", minutes=17)


@pytest.mark.parametrize("media_fixture,label", [("long_wav", "wav20"), ("long_aac", "aac17")])
def test_select_and_reselect_long_item(sess, request, media_fixture, label):
    media = request.getfixturevalue(media_fixture)
    clear_project(sess)
    insert_item_unselected(sess, media)
    ensure_window(sess)

    first = measure_after(sess, SELECT_ITEM0, loaded_marker=media.stem)
    _record(f"{label}.select", first)
    wait_loaded(sess, media.stem)

    sess.eval(DESELECT_ALL)
    again = measure_after(sess, SELECT_ITEM0, loaded_marker=media.stem)
    _record(f"{label}.reselect", again)

    # The freeze is the primary metric (a synchronous decode may not retitle
    # the window at all - the pre-hybrid build keeps the old title while it
    # blocks); first-paint latency is asserted whenever the title moved.
    assert first["max_stall"] <= STALL_BUDGET, f"first select froze: {first}"
    assert again["max_stall"] <= STALL_BUDGET, f"reselect froze: {again}"
    assert first["t_loaded"] is not None, f"audio never finished loading: {first}"
    for m in (first, again):
        if m["t_first"] is not None:
            assert m["t_first"] <= FIRST_PAINT_BUDGET, f"waveform late: {m}"
