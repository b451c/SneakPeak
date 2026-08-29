"""Split-band de-esser in Standalone (v2.5.0 row 15 #1).

The v2.4 de-esser is wideband: the band-filtered sidechain drives a gain
reduction the take volume envelope applies to the WHOLE signal. In Standalone
the new WIDE / SPLIT switch (footer gap of the DE-ESS tab) makes Apply cut only
the detected band: `y = x - (1 - g) * B(x)`, B = the detector filter.
Oracle: a 220 Hz bed under 6 kHz bursts. After a SPLIT Apply the bed's
amplitude inside a burst equals its amplitude before it, while the 6 kHz
burst is cut by about the -10 dB range clamp.
Control (637bb79): no switch - the click lands in an empty footer gap, Apply
ducks wideband and the bed drops ~10 dB inside every burst.
"""
from __future__ import annotations

import itertools
import time
from pathlib import Path

import numpy as np
import soundfile as sf

from conftest import (CM_APPLY_DYNAMICS, click_client, command_sync, dismiss_native_modal,
                      locate_apply_button, perf_media_dir, wait_main_thread_idle)
from test_perf_slider import APPLY_CENTER, F_MID, PAD, PILL_X, TAB_W, tab_center
from test_standalone_guards import SAVE, _last_toast, _open_and_wait

SHOTS = Path("/tmp/sneakpeak-reaproof-shots/deess_split")
SR = 44100
BED_HZ, BED_AMP = 220.0, 0.25
SIB_HZ, SIB_AMP, SIB_LEN = 6000.0, 0.5, 0.100
BURSTS = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0]
_N = itertools.count(1)

# --- premium panel geometry (ui_render.cpp ComputeDynLayout @ 480x300) ---
# Stage-power dots: {tabSeg.x + 4, tabSeg.y + 4, 16, 16}, tabSeg y = fMid - 12.
def stage_power_center(i: int) -> tuple[float, float]:
    x = PILL_X + sum(TAB_W[:i]) + 2.0 * i
    return (x + 12.0, F_MID)

# WIDE / SPLIT (De-Ess tab) = the PEAK/RMS footer-gap math: two 44 px halves
# 2 px apart, centred between Apply's right edge (pad + 84) and the tab pill.
def split_center(half: int) -> tuple[float, float]:
    gap_l, gap_r = PAD + 84.0, PILL_X
    rx = gap_l + ((gap_r - gap_l) - 2.0 * 44.0 - 2.0) * 0.5
    return (rx + half * 46.0 + 22.0, F_MID)


def _fixture() -> Path:
    """Mono float32: a 220 Hz bed with eight 100 ms 6 kHz bursts (2 ms raised-
    cosine edges). Unique name per call: re-opening a path that is already a
    Standalone tab activates THAT tab (its buffer already processed and saved)."""
    path = perf_media_dir() / f"deess_split_10s_{next(_N)}.wav"
    t = np.arange(10 * SR) / SR
    y = BED_AMP * np.sin(2 * np.pi * BED_HZ * t)
    edge = int(0.002 * SR)
    for t0 in BURSTS:
        a, b = int(t0 * SR), int((t0 + SIB_LEN) * SR)
        env = np.ones(b - a)
        ramp = 0.5 - 0.5 * np.cos(np.pi * np.arange(edge) / edge)
        env[:edge], env[-edge:] = ramp, ramp[::-1]
        y[a:b] += SIB_AMP * env * np.sin(2 * np.pi * SIB_HZ * t[a:b])
    sf.write(str(path), y.astype("float32"), SR, subtype="FLOAT")
    return path


def _amp(x: np.ndarray, t0: float, t1: float, hz: float) -> float:
    """Least-squares amplitude of a sine at `hz` over [t0, t1) - leakage-free
    (a windowless projection would smear the other tone in)."""
    a, b = int(t0 * SR), int(t1 * SR)
    seg = x[a:b]
    t = np.arange(a, b) / SR
    basis = np.stack([np.sin(2 * np.pi * hz * t), np.cos(2 * np.pi * hz * t)], axis=1)
    c, *_ = np.linalg.lstsq(basis, seg, rcond=None)
    return float(np.hypot(c[0], c[1]))


def _db(x: float, ref: float) -> float:
    return 20.0 * np.log10(max(x, 1e-12) / ref)


def _panel_origin(sess) -> tuple[int, int]:
    sess.wait_until(lambda: locate_apply_button(sess, SHOTS / "panel.png") is not None, timeout=15)
    ax, ay = locate_apply_button(sess, SHOTS / "panel.png")
    return int(round(ax - APPLY_CENTER[0])), int(round(ay - APPLY_CENTER[1]))


def test_split_band_apply_ducks_only_the_band_in_standalone(sess):
    SHOTS.mkdir(parents=True, exist_ok=True)
    media = _fixture()
    src = sf.read(str(media), dtype="float64", always_2d=True)[0][:, 0]
    _open_and_wait(sess, media)
    command_sync(sess, CM_APPLY_DYNAMICS, settle=1.0)          # shows the panel (defaults: fresh REAPER)
    px, py = _panel_origin(sess)

    def click(pt: tuple[float, float], settle: float = 0.3):
        click_client(sess, int(round(px + pt[0])), int(round(py + pt[1])))
        time.sleep(settle)

    click(tab_center(2))                                       # DE-ESS tab
    click(stage_power_center(0))                               # COMP power dot: bypass -> wideband gain 1.0
    click(stage_power_center(2))                               # DE-ESS power dot: dsEnable (6 kHz BP, Q 2, -10 dB range)
    click(split_center(1))                                     # SPLIT (control: empty gap, no-op)
    sess.eval('reaper.DeleteExtState("SneakPeak", "last_toast", false)')
    click(APPLY_CENTER, settle=0.1)
    sess.wait_until(lambda: _last_toast(sess).startswith("Dynamics applied"), timeout=30)
    apply_toast = _last_toast(sess)
    wait_main_thread_idle(sess, timeout=60)

    sess.eval('reaper.DeleteExtState("SneakPeak", "last_toast", false)')
    sess.eval(SAVE)                                            # overwrite the fixture with the buffer
    dismiss_native_modal(sess, timeout=10)                     # "Overwrite original file?" -> Yes
    sess.wait_until(lambda: _last_toast(sess).startswith("Saved"), timeout=30)
    wait_main_thread_idle(sess, timeout=60)

    got = sf.read(str(media), dtype="float64", always_2d=True)[0][:, 0]
    assert len(got) == len(src), f"{len(got)} frames after Apply + Save, expected {len(src)}"

    rows = []
    for t0 in BURSTS:
        bed_before = _amp(got, t0 - 0.4, t0 - 0.1, BED_HZ)
        bed_in = _amp(got, t0 + 0.03, t0 + 0.09, BED_HZ)
        sib_in = _amp(got, t0 + 0.03, t0 + 0.09, SIB_HZ)
        rows.append((t0, _db(bed_before, BED_AMP), _db(bed_in, bed_before), _db(sib_in, SIB_AMP)))
    print(f"\n[deess_split] toast {apply_toast!r}")
    for t0, bed_idle, bed_duck, sib_cut in rows:
        print(f"[deess_split] burst @{t0:.0f}s: bed idle {bed_idle:+.2f} dB, bed inside burst "
              f"{bed_duck:+.2f} dB, 6 kHz {sib_cut:+.2f} dB")
    for t0, bed_idle, bed_duck, sib_cut in rows:
        assert abs(bed_idle) < 0.1, f"@{t0:.0f}s the bed changed by {bed_idle:+.2f} dB with the de-esser idle"
        assert -12.0 < sib_cut < -6.0, f"@{t0:.0f}s the 6 kHz burst is {sib_cut:+.2f} dB (expected about -10 dB)"
        assert abs(bed_duck) < 0.5, (f"@{t0:.0f}s the 220 Hz bed ducks {bed_duck:+.2f} dB inside the burst - "
                                     "the de-esser is wideband, not split-band")
