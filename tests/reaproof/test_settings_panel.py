"""The Settings panel stays put (s20, user): a click outside no longer closes
it, it drags like the other premium panels (offsets remembered), and it floats
over the whole content area - waveform + spectral pane - instead of being
squeezed into the waveform pane.

Oracles: ExtState SneakPeak/ui_state ("... settings=1,l,t,r,b ..." = open with
the panel's client rect; "settings=0,0,0,0,0" = closed), the persisted
set_off_x / set_off_y, and the panel's height (kSettingsH 570 at scale 1.0 -
the fit-clamp shrinks it when its area is not tall enough).
"""
from __future__ import annotations

import time
from pathlib import Path

from conftest import (SELECT_ITEM0, burst_fixture, capture, clear_project, click_client,
                      client_size, drag_client, ensure_window, insert_item_unselected, key_sync,
                      send_command, wait_audio_loaded, window_handle_lua, window_title)

CM_SETTINGS, CM_TOGGLE_SPECTRAL = 2167, 2028   # edit_view.h ContextMenuID (compiled enum)
VK_ESCAPE = 0x1B
SHOTS = Path("/tmp/sneakpeak-reaproof-shots/settings_panel")
SETTINGS_W, SETTINGS_H, HEADER_H = 320.0, 570.0, 44.0   # ui_theme.h kSettingsW/H, kHeaderH


def _ui(sess) -> dict[str, str]:
    raw = str(sess.eval('return reaper.GetExtState("SneakPeak", "ui_state")'))
    return dict(tok.split("=", 1) for tok in raw.split() if "=" in tok)


def _settings(sess) -> tuple[bool, int, int, int, int]:
    v = _ui(sess).get("settings", "0,0,0,0,0").split(",")
    return v[0] == "1", int(v[1]), int(v[2]), int(v[3]), int(v[4])


def _wait_settings(sess, open_: bool, timeout: float = 5.0):
    sess.wait_until(lambda: _settings(sess)[0] == open_, timeout=timeout)


def _forget_offsets(sess):
    sess.eval('reaper.DeleteExtState("SneakPeak", "set_off_x", true) '
              'reaper.DeleteExtState("SneakPeak", "set_off_y", true) return true')


def _offsets(sess) -> tuple[str, str]:
    return (str(sess.eval('return reaper.GetExtState("SneakPeak", "set_off_x")')),
            str(sess.eval('return reaper.GetExtState("SneakPeak", "set_off_y")')))


def _load(sess, name: str):
    media = burst_fixture(name, seconds=10, channels=2)
    clear_project(sess)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=60)


def _close(sess):
    if _settings(sess)[0]:
        key_sync(sess, VK_ESCAPE, settle=0.4)


def _resize_window(sess, w: int, h: int) -> tuple[int, int, int, int] | None:
    """Outer-window resize through JS_Window_SetPosition; returns the previous
    (x, y, w, h) or None when the client did not follow (skip the spec)."""
    old = sess.eval(f"local h = {window_handle_lua()} if not h then return nil end "
                    "local _, x, y, r, b = reaper.JS_Window_GetRect(h) "
                    "return x .. ',' .. y .. ',' .. (r - x) .. ',' .. (b - y)")
    if not old:
        return None
    x, y, ow, oh = (int(v) for v in str(old).split(","))
    sess.eval(f"local h = {window_handle_lua()} if h then reaper.JS_Window_SetPosition(h, {x}, {y}, {w}, {h}) end return true")
    time.sleep(1.0)
    cw, ch = client_size(sess)
    return (x, y, ow, oh) if ch > oh + 100 else None


def test_click_outside_keeps_the_panel_open_and_a_drag_moves_it(sess):
    """Open, click beside the panel -> still open; drag its header by (60, 40)
    -> the rect moves by that, the offsets persist; Esc + reopen -> same place.
    In a tall window: in the default 800x428 one the panel is taller than its
    area and the clamp pins it vertically."""
    import pytest
    _load(sess, "settings_click_10s.wav")
    _forget_offsets(sess)
    SHOTS.mkdir(parents=True, exist_ok=True)
    old = _resize_window(sess, 1000, 900)
    if old is None:
        pytest.skip("JS_Window_SetPosition did not resize the window here")
    try:
        send_command(sess, CM_SETTINGS)
        _wait_settings(sess, True)
        _, l, t, r, b = _settings(sess)
        capture(sess, SHOTS / "1_open.png")

        click_client(sess, max(4, l // 2), (t + b) // 2)   # beside the panel, on the waveform
        time.sleep(0.8)
        assert _settings(sess)[0], "a click outside the Settings panel closed it"
        assert _settings(sess)[1:] == (l, t, r, b), "a click outside moved the Settings panel"

        S = (r - l) / SETTINGS_W                          # the fit-clamped scale
        hx, hy = (l + r) // 2, t + int(HEADER_H * 0.5 * S)  # the header, clear of the close box
        drag_client(sess, hx, hy, hx + 60, hy + 40)
        time.sleep(0.5)
        capture(sess, SHOTS / "2_dragged.png")
        _, l2, t2, r2, b2 = _settings(sess)
        assert abs((l2 - l) - 60) <= 2 and abs((t2 - t) - 40) <= 2, \
            f"the drag did not move the panel by (60, 40): {(l, t)} -> {(l2, t2)}"
        assert _offsets(sess) == ("60", "40"), f"the offsets were not persisted: {_offsets(sess)}"

        key_sync(sess, VK_ESCAPE, settle=0.4)
        _wait_settings(sess, False)
        send_command(sess, CM_SETTINGS)
        _wait_settings(sess, True)
        assert _settings(sess)[1:] == (l2, t2, r2, b2), \
            f"the panel forgot its place on reopen: {(l2, t2)} -> {_settings(sess)[1:3]}"
    finally:
        _close(sess)
        _forget_offsets(sess)
        x, y, w, h = old
        sess.eval(f"local h = {window_handle_lua()} if h then reaper.JS_Window_SetPosition(h, {x}, {y}, {w}, {h}) end return true")


def test_the_panel_floats_over_the_spectral_pane_too(sess):
    """A tall window with the spectral view open: the panel keeps its full
    height (its area = waveform + spectral panes); the waveform pane alone would
    fit-clamp it to little more than half."""
    import pytest
    _load(sess, "settings_spectral_10s.wav")
    _forget_offsets(sess)
    old = _resize_window(sess, 1000, 900)
    if old is None:
        pytest.skip("JS_Window_SetPosition did not resize the window here")
    try:
        send_command(sess, CM_TOGGLE_SPECTRAL)
        sess.wait_until(lambda: "Computing" not in window_title(sess), timeout=60)
        time.sleep(0.5)
        send_command(sess, CM_SETTINGS)
        _wait_settings(sess, True)
        _, l, t, r, b = _settings(sess)
        capture(sess, SHOTS / "3_spectral.png")
        assert b - t >= SETTINGS_H - 4, \
            f"the Settings panel is squeezed into the waveform pane: {b - t} px tall (expected {SETTINGS_H:.0f})"
    finally:
        _close(sess)
        if _ui(sess).get("spectral") == "1":
            send_command(sess, CM_TOGGLE_SPECTRAL)
        x, y, w, h = old
        sess.eval(f"local h = {window_handle_lua()} if h then reaper.JS_Window_SetPosition(h, {x}, {y}, {w}, {h}) end return true")
