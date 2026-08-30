"""The gain knob bubble sits UNDER the premium panels (s20, user): it was
drawn over the Hard Limiter's header and took the clicks meant for the panel.

Oracle: a drag that starts on the bubble where it overlaps the Hard Limiter's
header must drag the PANEL (its persisted offset lim_off_x changes), not the
knob. Driver: the bubble rect from the ui_state mirror ("gain=l,t,r,b"), the
panel from the pixel-located Apply button (ComputeLimiterLayout @ 480x266).
"""
from __future__ import annotations

import time
from pathlib import Path

import pytest

from conftest import (capture, clear_project, drag_client, ensure_window, key_sync,
                      locate_apply_button, perf_media_dir, send_command, wait_audio_loaded,
                      write_long_wav)

CM_APPLY_LIMITER = 2176      # edit_view.h ContextMenuID (compiled enum)
VK_ESCAPE = 0x1B
SHOTS = Path("/tmp/sneakpeak-reaproof-shots/gain_zorder")
PANEL_W, PANEL_H, PAD, HEADER_H, FOOTER_H = 480.0, 266.0, 16.0, 44.0, 44.0
APPLY_CENTER = (PANEL_W - PAD - 50.0, PANEL_H - FOOTER_H + 9.0 + 13.0)
OPEN = ('reaper.defer(function() reaper.Main_OnCommand('
        'reaper.NamedCommandLookup("_SneakPeak_OpenStandalone"), 0) end) return true')


def _ui(sess) -> dict[str, str]:
    raw = str(sess.eval('return reaper.GetExtState("SneakPeak", "ui_state")'))
    return dict(tok.split("=", 1) for tok in raw.split() if "=" in tok)


def _lim_off(sess) -> tuple[str, str]:
    return (str(sess.eval('return reaper.GetExtState("SneakPeak", "lim_off_x")')),
            str(sess.eval('return reaper.GetExtState("SneakPeak", "lim_off_y")')))


def test_a_drag_on_the_bubble_over_the_limiter_header_moves_the_panel(sess):
    media = write_long_wav(perf_media_dir() / "zorder_30s.wav", minutes=0.5)
    clear_project(sess)
    ensure_window(sess)
    sess.eval(f'reaper.SetExtState("SneakPeak", "open_path", "{media.as_posix()}", false) return true')
    sess.eval(OPEN)
    time.sleep(1.0)
    wait_audio_loaded(sess, media.stem, timeout=60)
    sess.eval('reaper.DeleteExtState("SneakPeak", "lim_off_x", true) reaper.DeleteExtState("SneakPeak", "lim_off_y", true) return true')
    SHOTS.mkdir(parents=True, exist_ok=True)
    try:
        send_command(sess, CM_APPLY_LIMITER)
        sess.wait_until(lambda: locate_apply_button(sess, SHOTS / "probe.png") is not None, timeout=10)
        ax, ay = locate_apply_button(sess, SHOTS / "probe.png")
        px, py = int(round(ax - APPLY_CENTER[0])), int(round(ay - APPLY_CENTER[1]))
        g = [int(v) for v in _ui(sess).get("gain", "0,0,0,0").split(",")]
        assert g[2] > g[0], f"the gain bubble is not on screen: {g}"
        # a point on the bubble that lies over the panel's header, clear of its controls
        x, y = g[2] - 10, (g[1] + g[3]) // 2
        hx, hy = x - px, y - py
        if not (0 <= hx < PANEL_W and 0 <= hy < HEADER_H):
            pytest.skip(f"the bubble does not overlap the limiter header in this window: bubble {g}, panel origin {(px, py)}")
        before = _lim_off(sess)
        capture(sess, SHOTS / "1_before.png")
        drag_client(sess, x, y, x + 50, y + 30)
        time.sleep(0.5)
        capture(sess, SHOTS / "2_after.png")
        after = _lim_off(sess)
        assert after != before and after != ("", ""), \
            f"the drag on the bubble did not move the limiter panel (lim_off {before} -> {after}): the bubble is still on top"
    finally:
        key_sync(sess, VK_ESCAPE, settle=0.4)
        sess.eval('reaper.DeleteExtState("SneakPeak", "lim_off_x", true) reaper.DeleteExtState("SneakPeak", "lim_off_y", true) return true')
