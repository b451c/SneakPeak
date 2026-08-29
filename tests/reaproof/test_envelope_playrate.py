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


def _assert_dip_on_the_burst_at_rate_1_3(sess):
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


def test_apply_dynamics_dip_lands_on_the_burst_at_rate_1_3(sess, burst_wav):
    clear_project(sess)
    info = insert_item(sess, burst_wav, playrate=RATE)
    assert abs(info["rate"] - RATE) < 1e-9
    assert abs(info["len"] - SRC_SECONDS / RATE) < 0.01, info

    ensure_window(sess)
    wait_loaded(sess, "floor_burst")
    apply_dynamics(sess, SHOTS / "rate13")
    _assert_dip_on_the_burst_at_rate_1_3(sess)


def test_envelope_follows_rate_change(sess, burst_wav):
    """A6.3: the item is loaded at rate 1.0 and its rate is changed to 1.3
    while SneakPeak shows it (Item properties, a script - an undo block, as
    REAPER's project-state counter ignores bare API setters). SneakPeak polled
    position, length and channel mode but not the playrate, so Apply Dynamics
    still wrote the envelope in item time (the forum #107 bug, back through a
    stale cache): the dip landed early, in the quiet region. Control (c4a276e):
    phantom dip, points end near the item length."""
    clear_project(sess)
    insert_item(sess, burst_wav, playrate=1.0)
    ensure_window(sess)
    wait_loaded(sess, "floor_burst")
    sess.eval(f"""
      reaper.Undo_BeginBlock()
      local it = reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), 0)
      local tk = reaper.GetActiveTake(it)
      reaper.SetMediaItemTakeInfo_Value(tk, "D_PLAYRATE", {RATE})
      reaper.SetMediaItemLength(it, {SRC_SECONDS} / {RATE}, false)
      reaper.Undo_EndBlock("spec: rate {RATE}", -1)
      reaper.UpdateArrange() return true""")
    import time
    time.sleep(1.5)                                    # the poll sees the new rate
    apply_dynamics(sess, SHOTS / "rate_change")
    _assert_dip_on_the_burst_at_rate_1_3(sess)


def test_apply_two_segments_different_rates(sess, burst_wav):
    """Audit coverage gap: a SET view over two items of the same source at
    rates 1.3 and 0.7; Apply Dynamics must put each dip on ITS burst in
    project time (per-segment take timebase) and nowhere else."""
    from conftest import CM_TRACK_VIEW, mode_from_capture, send_command
    import time
    RATE_B, POS_B = 0.7, 8.0
    clear_project(sess)
    insert_item(sess, burst_wav, playrate=RATE)
    sess.eval(f"""
      local tr = reaper.GetTrack(0, 0)
      reaper.SetOnlyTrackSelected(tr)
      reaper.SetEditCurPos({POS_B}, false, false)
      reaper.InsertMedia("{burst_wav.as_posix()}", 0)
      local it = reaper.GetTrackMediaItem(tr, 1)
      local tk = reaper.GetActiveTake(it)
      reaper.SetMediaItemTakeInfo_Value(tk, "B_PPITCH", 0)
      reaper.SetMediaItemTakeInfo_Value(tk, "D_PLAYRATE", {RATE_B})
      reaper.SetMediaItemLength(it, {SRC_SECONDS} / {RATE_B}, false)
      reaper.SetMediaItemInfo_Value(it, "D_POSITION", {POS_B})
      reaper.SelectAllMediaItems(0, true)
      reaper.UpdateArrange() return true""")
    ensure_window(sess)
    time.sleep(1.0)
    send_command(sess, CM_TRACK_VIEW)
    time.sleep(1.5)
    assert mode_from_capture(sess, SHOTS / "two_rates_set.png") == "SET"
    apply_dynamics(sess, SHOTS / "two_rates")
    # per-item project-time windows: burst = source 4-5 s / rate, floors around it
    a0, a1 = BURST[0] / RATE, BURST[1] / RATE                       # 3.08 .. 3.85
    b0, b1 = POS_B + BURST[0] / RATE_B, POS_B + BURST[1] / RATE_B   # 13.71 .. 15.14
    wins = [(a0 + 0.08, a1 - 0.08), (0.5, 2.2), (a1 + 0.8, SRC_SECONDS / RATE - 0.3),
            (b0 + 0.1, b1 - 0.1), (POS_B + 0.5, POS_B + 4.5), (b1 + 1.0, POS_B + SRC_SECONDS / RATE_B - 0.5)]
    rms = track_rms_windows(sess, wins)
    src = [BURST_AMP / math.sqrt(2), FLOOR_AMP / math.sqrt(2), FLOOR_AMP / math.sqrt(2)] * 2
    ga_burst, ga_before, ga_after, gb_burst, gb_before, gb_after = (db(o) - db(s) for o, s in zip(rms, src))
    print(f"\n[rates] item A (1.3): burst {ga_burst:+.1f} floor {ga_before:+.1f}/{ga_after:+.1f} dB; "
          f"item B (0.7): burst {gb_burst:+.1f} floor {gb_before:+.1f}/{gb_after:+.1f} dB")
    assert ga_burst - ga_after <= -6.0, "item A (rate 1.3): its burst was not compressed"
    assert gb_burst - gb_after <= -6.0, "item B (rate 0.7): its burst was not compressed"
    assert abs(ga_before - ga_after) <= 1.0 and abs(gb_before - gb_after) <= 1.0, "a phantom dip in a quiet region"


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
