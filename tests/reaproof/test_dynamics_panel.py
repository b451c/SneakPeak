"""Dynamics panel controls (v2.5.0 audit, increment A7.5).

The De-Ess threshold knob (D.Thr) gets the hard-left "Auto" detent G.Thr
already had: the knob bottom snaps to the engine's Auto sentinel (-100 = the
band's average level), which was unreachable from the slider (min -60).
Oracle: the params string Apply writes to the item's P_EXT carries dst=-100.
Control (4d729d0): dst=-60.0 (the knob minimum).
"""
from __future__ import annotations

import time
from pathlib import Path

from conftest import (CM_APPLY_DYNAMICS, SELECT_ITEM0, burst_fixture, clear_project, click_client,
                      command_sync, ensure_window, insert_item_unselected, locate_apply_button,
                      wait_audio_loaded, wait_main_thread_idle)
from test_perf_slider import APPLY_CENTER, _knob_drag_lua, _panel_origin, cell_center, tab_center

SHOTS = Path("/tmp/sneakpeak-reaproof-shots/dynamics_panel")
PEXT_KEY = "P_EXT:SneakPeak_Dynamics"


def _dyn_pext(sess) -> str:
    return str(sess.eval('local it = reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), 0) '
                         f'local ok, s = reaper.GetSetMediaItemInfo_String(it, "{PEXT_KEY}", "", false) '
                         'return ok and s or ""'))


def test_deess_threshold_auto_detent(sess):
    SHOTS.mkdir(parents=True, exist_ok=True)
    media = burst_fixture("dynpanel_10s.wav", seconds=10, channels=2)
    clear_project(sess)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=60)
    command_sync(sess, CM_APPLY_DYNAMICS, settle=1.0)               # shows the panel
    sess.wait_until(lambda: locate_apply_button(sess, SHOTS / "panel.png") is not None, timeout=15)
    px, py = _panel_origin(sess)
    tx, ty = tab_center(2)                                          # DE-ESS tab
    click_client(sess, int(px + tx), int(py + ty))
    time.sleep(0.3)
    kx, ky = cell_center(0, 2)                                      # D.Thr (De-Ess / LEVELS)
    kx, ky = int(px + kx), int(py + ky)
    sess.eval(_knob_drag_lua(kx, ky, ky + 400, 24))                 # far past the bottom of the range
    time.sleep(0.5)
    sess.eval('reaper.DeleteExtState("SneakPeak", "last_toast", false)')
    ax, ay = APPLY_CENTER
    click_client(sess, int(px + ax), int(py + ay))                  # Apply writes the params to P_EXT
    sess.wait_until(lambda: str(sess.eval('return reaper.GetExtState("SneakPeak", "last_toast")')).startswith(("Applied", "Envelope")), timeout=20)
    wait_main_thread_idle(sess, timeout=60)
    pext = _dyn_pext(sess)
    print(f"\n[dynpanel] P_EXT after the hard-left D.Thr drag + Apply: {pext!r}")
    assert "dst=" in pext, "Apply wrote no dynamics params to the item"
    assert "dst=-100" in pext, "D.Thr at hard-left is not the Auto sentinel (no detent)"
