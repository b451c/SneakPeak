"""Input, keyboard and lifecycle guards (v2.5.0 audit, increment A5).

A5.1 - the keyboard hook ate every listed key while SneakPeak was focused,
even ones it does nothing with (a bare arrow): REAPER never saw its own
shortcut. A5.2 - Ctrl+Y never reached the editor's redo. A5.3 - a
right-click during a drag left the drag (and its undo block) open. A5.5 - a
window rect saved off-screen restored off-screen. A5.4 - the update check
ran curl on the UI thread (up to 5 s frozen). Control (cb48cd5): cursor
unchanged / no redo / selection keeps following / window at -5000 / stall.
"""
from __future__ import annotations

import sys
import time
from pathlib import Path

import pytest

from conftest import (SELECT_ITEM0, WAVE_Y, burst_fixture, clear_project, click_client,
                      client_size, ensure_window, insert_item_unselected, toggle_window,
                      wait_audio_loaded, window_handle_lua, window_visible)

SHOTS = Path("/tmp/sneakpeak-reaproof-shots/input")
KVK_RIGHT, KVK_Y, KVK_Z = 124, 16, 6      # macOS virtual key codes


def _front_and_key(sess, kvk: int, *, cmd: bool = False):
    """A REAL key event (CGEvent) to the frontmost REAPER: the only way into
    REAPER's accelerator hook, where the spec's behaviour lives."""
    import subprocess
    import Quartz
    subprocess.run(["osascript", "-e", 'tell application "System Events" to set frontmost of '
                    f'(first process whose unix id is {sess.handle.pid}) to true'], capture_output=True)
    time.sleep(0.3)
    for down in (True, False):
        ev = Quartz.CGEventCreateKeyboardEvent(None, kvk, down)
        if cmd:
            Quartz.CGEventSetFlags(ev, Quartz.kCGEventFlagMaskCommand)
        Quartz.CGEventPost(Quartz.kCGHIDEventTap, ev)
        time.sleep(0.05)
    time.sleep(0.4)


def _mac_windows(sess) -> list[str]:
    """Names of the on-screen windows of the REAPER process (a modal shows up here)."""
    import Quartz
    out = []
    for w in Quartz.CGWindowListCopyWindowInfo(Quartz.kCGWindowListOptionOnScreenOnly, Quartz.kCGNullWindowID) or []:
        if w.get("kCGWindowOwnerPID") == sess.handle.pid:
            b = w.get("kCGWindowBounds", {})
            out.append(f"{w.get('kCGWindowName', '')!r} {int(b.get('Width', 0))}x{int(b.get('Height', 0))} layer {w.get('kCGWindowLayer')}")
    return out


def _focus_editor(sess):
    click_client(sess, 400, WAVE_Y)     # a click gives our window the focus the hook checks
    time.sleep(0.3)


@pytest.mark.skipif(sys.platform != "darwin", reason="real key events (CGEvent) - macOS leg")
def test_a_bare_arrow_key_reaches_reaper(sess):
    media = burst_fixture("input_10s.wav", seconds=10, channels=2)
    clear_project(sess)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=60)
    _focus_editor(sess)
    sess.eval("reaper.SetEditCurPos(2.0, false, false) return true")
    before = float(sess.eval("return reaper.GetCursorPosition()"))

    _front_and_key(sess, KVK_RIGHT)

    after = float(sess.eval("return reaper.GetCursorPosition()"))
    print(f"\n[input] Right arrow: edit cursor {before:.4f} -> {after:.4f}; windows {_mac_windows(sess)}")
    time.sleep(1.0)
    print(f"[input] windows 1 s later: {_mac_windows(sess)}")
    try:
        clear_project(sess)
    except Exception as e:   # noqa: BLE001 - diagnose the post-key hang seen on the control
        print(f"[input] clear_project after the key: {e!r}; windows {_mac_windows(sess)}")
        raise
    assert after > before, "the Right arrow was swallowed by SneakPeak (REAPER's cursor did not move)"


@pytest.mark.skipif(sys.platform != "darwin", reason="real key events (CGEvent) - macOS leg")
def test_ctrl_y_redoes_in_standalone(sess):
    import numpy as np
    import soundfile as sf
    import tempfile
    from test_standalone_guards import _command_sync, _key_sync, _open_standalone
    CM_UNDO, CM_SELECT_ALL, VK_SPACE = 2000, 2007, 0x20
    media = burst_fixture("input_sa_10s.wav", seconds=10, channels=2)
    clear_project(sess)
    ensure_window(sess)
    _open_standalone(sess, media)
    wait_audio_loaded(sess, media.name, timeout=60)
    time.sleep(0.5)
    _focus_editor(sess)
    # an edit: Reverse (Standalone in-memory), then undo, then Ctrl+Y (Cmd+Y on the mac keyboard)
    sess.eval('reaper.defer(function() reaper.Main_OnCommand(reaper.NamedCommandLookup("_SneakPeak_Reverse"), 0) end) return true')
    from conftest import dismiss_native_modal
    dismiss_native_modal(sess, timeout=3)   # builds before A4.4 prompt here; harmless otherwise
    time.sleep(1.0)
    _command_sync(sess, CM_UNDO)
    time.sleep(0.5)
    _front_and_key(sess, KVK_Y, cmd=True)
    time.sleep(0.8)

    preview = Path(tempfile.gettempdir()) / f"sneakpeak_preview_{sess.handle.pid}.wav"
    preview.unlink(missing_ok=True)
    _command_sync(sess, CM_SELECT_ALL, settle=0.3)
    _key_sync(sess, VK_SPACE, settle=0.2)
    sess.wait_until(preview.exists, timeout=10)
    time.sleep(0.5)
    _key_sync(sess, VK_SPACE, settle=0.3)
    got = sf.read(str(preview), dtype="float64", always_2d=True)[0]
    want = sf.read(str(media), dtype="float64", always_2d=True)[0]
    assert np.abs(got[:4410] - want[::-1][:4410]).max() < 1e-4, "Ctrl+Y did not redo the reverse"


def test_right_click_does_not_interrupt_a_selection_drag(sess):
    """Posted mouse messages: button down, moves, a right-click, more moves.
    The selection end must not follow the moves after the right-click's
    release... it must keep following (the drag is still live) - what must
    NOT happen is the drag ending or the right-click acting."""
    media = burst_fixture("input_drag_10s.wav", seconds=10, channels=2)
    clear_project(sess)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=60)
    h = window_handle_lua()
    def post(msg, w, x, y):
        sess.eval(f'local h = {h} reaper.JS_WindowMessage_Post(h, "{msg}", {w}, 0, {x}, {y}) return true')
    post("WM_LBUTTONDOWN", 1, 200, WAVE_Y); time.sleep(0.1)
    for x in range(210, 320, 10):
        post("WM_MOUSEMOVE", 1, x, WAVE_Y); time.sleep(0.03)
    time.sleep(0.3)
    post("WM_RBUTTONDOWN", 2, 320, WAVE_Y); time.sleep(0.1)
    post("WM_RBUTTONUP", 0, 320, WAVE_Y); time.sleep(0.5)
    for x in range(330, 500, 10):
        post("WM_MOUSEMOVE", 1, x, WAVE_Y); time.sleep(0.03)
    time.sleep(0.3)
    post("WM_LBUTTONUP", 0, 500, WAVE_Y); time.sleep(0.5)
    s0, s1 = sess.eval("local s, e = reaper.GetSet_LoopTimeRange2(0, false, false, 0, 0, false) return {s, e}")
    print(f"\n[input] selection after drag + right-click: {float(s0):.3f}-{float(s1):.3f} s")
    if sys.platform == "darwin":
        _front_and_key(sess, 53)   # Escape: closes the context menu the control opened mid-drag
    # the mirror into REAPER's time selection: a drag from 200 to 500 px on a 10 s
    # item ends near 6.5 s; a drag cut short at the right-click ends near 4 s
    assert float(s1) - float(s0) > 3.5, "the right-click ended the drag early (selection too short)"


def test_window_restores_onto_a_monitor(sess):
    """A saved rect far off-screen must come back on a monitor. macOS
    constrains a window to a screen by itself (GREEN on the control there);
    the RED/GREEN evidence for the clamp is the Windows VM leg."""
    sess.eval('reaper.SetExtState("SneakPeak", "win_rect", "-5000 -5000 800 400", true) return true')
    if window_visible(sess):
        toggle_window(sess)
        sess.wait_until(lambda: not window_visible(sess), timeout=10)
    toggle_window(sess)
    sess.wait_until(lambda: window_visible(sess), timeout=10)
    time.sleep(0.5)
    l, t, r, b = sess.eval(f"local h = {window_handle_lua()} local _, l, t, r, b = reaper.JS_Window_GetRect(h) return {{l, t, r, b}}")
    print(f"\n[input] restored window rect: {l},{t}-{r},{b}")
    assert int(r) > 0 and int(b) > 0 and int(l) > -800 and int(t) > -400, f"window restored off-screen: {l},{t}-{r},{b}"
    sess.eval('reaper.DeleteExtState("SneakPeak", "win_rect", true) return true')


def _system_dpi() -> int:
    """The real system DPI (python.exe is DPI-unaware: ask on a per-monitor-aware thread)."""
    if sys.platform != "win32":
        return 96
    import ctypes
    u32 = ctypes.windll.user32
    try:
        u32.SetThreadDpiAwarenessContext.restype = ctypes.c_void_p
        prev = u32.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))   # PER_MONITOR_AWARE_V2
        try:
            return int(u32.GetDpiForSystem())
        finally:
            u32.SetThreadDpiAwarenessContext(ctypes.c_void_p(prev))
    except (AttributeError, OSError):
        return 96


def test_default_floating_window_is_800x400(sess):
    """No saved rect: the floating window opens with an 800x400 client (times
    the DPI). The Win32 DLGTEMPLATE takes cx/cy in dialog units (about 2 px
    each with the system font), so the control opened a ~1600x800 window
    (1532 wide on the VM's 1512 px screen) - every client coordinate the
    specs use (WAVE_Y, VERSION_LABEL, the drag scale) was off there. SWELL
    takes pixels: GREEN on the macOS control; the Windows leg is the RED."""
    if window_visible(sess):
        toggle_window(sess)      # Destroy() saves the current rect ...
        sess.wait_until(lambda: not window_visible(sess), timeout=10)
    sess.eval('reaper.DeleteExtState("SneakPeak", "win_rect", true) return true')   # ... which we drop
    toggle_window(sess)
    sess.wait_until(lambda: window_visible(sess), timeout=10)
    time.sleep(0.5)
    w, h = client_size(sess)
    dpi = _system_dpi()
    want_w, want_h = round(800 * dpi / 96), round(400 * dpi / 96)
    print(f"\n[input] default floating client: {w}x{h} (want {want_w}x{want_h} at {dpi} dpi)")
    assert abs(w - want_w) <= 4 and abs(h - want_h) <= 4, \
        f"default floating window is {w}x{h}, not {want_w}x{want_h} (dialog units instead of pixels?)"
