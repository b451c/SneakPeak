"""Forum #107 (Lunar Ladder): envelopes written by SneakPeak must land at the
right PROJECT time on items whose playrate is not 1.0.

REAPER keeps take-envelope point times in the take's own timebase
(item time * playrate). v2.4.0 wrote plain item time, so on a 1.3x item the
compression dip landed at 1/1.3 of the intended position (early) - in a
quiet region - while the loud burst it was meant to tame played untouched.

Ground truth = the TRACK audio accessor: REAPER itself renders the item with
the take envelope applied. We compare per-window output/source gain, so item
volume and auto-makeup cancel out and only the envelope shape remains.

Negative control (must go RED): run with SNEAKPEAK_DYLIB=<pre-fix binary>.
"""
from __future__ import annotations

import math
from pathlib import Path

import pytest

from conftest import (apply_dynamics, clear_project, db, ensure_window,
                      insert_item, take_envelope_points, track_rms_windows,
                      wait_loaded, write_floor_burst_wav)

SHOTS = Path("/tmp/sneakpeak-reaproof-shots")
RATE = 1.3
SRC_SECONDS = 10.0
BURST = (4.0, 5.0)            # source seconds
FLOOR_AMP, BURST_AMP = 0.03, 0.9


@pytest.fixture(scope="module")
def burst_wav(tmp_path_factory):
    p = tmp_path_factory.mktemp("media") / "floor_burst.wav"
    write_floor_burst_wav(p, seconds=SRC_SECONDS, floor_amp=FLOOR_AMP,
                          burst_amp=BURST_AMP, burst=BURST)
    return p


def test_apply_dynamics_dip_lands_on_the_burst_at_rate_1_3(sess, burst_wav):
    clear_project(sess)
    info = insert_item(sess, burst_wav, playrate=RATE)
    assert abs(info["rate"] - RATE) < 1e-9
    assert abs(info["len"] - SRC_SECONDS / RATE) < 0.01, info

    ensure_window(sess)
    wait_loaded(sess, "floor_burst")
    apply_dynamics(sess, SHOTS / "rate13")

    # Project-time windows (source time / rate), with attack/release margins.
    b0, b1 = BURST[0] / RATE, BURST[1] / RATE          # 3.077 .. 3.846
    w_burst = (b0 + 0.08, b1 - 0.08)
    w_floor_before = (0.5, 2.2)
    w_floor_after = (b1 + 0.8, SRC_SECONDS / RATE - 0.3)
    w_bug = (b0 / RATE + 0.05, b1 / RATE - 0.05)        # where v2.4.0 put the dip
    rms = track_rms_windows(sess, [w_burst, w_floor_before, w_floor_after, w_bug])
    src = [BURST_AMP / math.sqrt(2)] + [FLOOR_AMP / math.sqrt(2)] * 3
    g_burst, g_before, g_after, g_bug = (db(o) - db(s) for o, s in zip(rms, src))

    # 1) the burst is the ONLY thing that gets reduced
    assert g_burst - g_after <= -6.0, (
        f"burst not compressed: burst {g_burst:+.1f} dB vs floor {g_after:+.1f} dB")
    # 2) no phantom dip in the quiet region where an item-time envelope would land
    assert abs(g_bug - g_before) <= 1.0, (
        f"phantom dip at {w_bug}: {g_bug:+.1f} dB vs floor {g_before:+.1f} dB")
    # 3) the quiet floor is treated uniformly before and after the burst
    assert abs(g_before - g_after) <= 1.0, (g_before, g_after)

    # 4) API-level cross-check: points span the TAKE timebase (source seconds),
    #    not the item length in project seconds.
    pts = take_envelope_points(sess)
    last = max(t for t, _ in pts)
    assert last > SRC_SECONDS - 0.5, (
        f"points end at {last:.2f} s take-time; expected ~{SRC_SECONDS}")


def test_apply_dynamics_unchanged_at_rate_1_0(sess, burst_wav):
    """Regression guard: rate 1.0 is the identity mapping - same oracle."""
    clear_project(sess)
    insert_item(sess, burst_wav, playrate=1.0)
    ensure_window(sess)
    wait_loaded(sess, "floor_burst")
    apply_dynamics(sess, SHOTS / "rate10")
    w_burst = (BURST[0] + 0.08, BURST[1] - 0.08)
    w_floor = (6.0, 9.5)
    rms = track_rms_windows(sess, [w_burst, w_floor])
    g_burst = db(rms[0]) - db(BURST_AMP / math.sqrt(2))
    g_floor = db(rms[1]) - db(FLOOR_AMP / math.sqrt(2))
    assert g_burst - g_floor <= -6.0, (g_burst, g_floor)
