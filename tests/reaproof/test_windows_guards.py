"""Windows correctness (v2.5.0 audit, increment A3) - runs on the Windows VM leg.

A3.1 - the in-place editors opened the source through the ANSI C runtime, so a
file under a non-ASCII path (user name, folder) was "not found" and the edit
failed. A3.2 - temp files were deleted with the ANSI remove(), which does not
find them under a non-ASCII %TEMP%: undo copies leaked. A3.3 - off-screen
bitmaps were deleted while still selected into their DC (GDI refuses
silently): one bitmap leaked per resize. Control (cb48cd5): write failure /
leftover undo file / GDI object count climbing with every resize.
"""
from __future__ import annotations

import hashlib
import shutil
import sys
import time
from pathlib import Path

import pytest

from conftest import (SELECT_ITEM0, burst_fixture, clear_project, db, dismiss_native_modal,
                      ensure_window, insert_item_unselected, perf_media_dir, send_command,
                      track_rms_windows, wait_audio_loaded, wait_main_thread_idle)

pytestmark = pytest.mark.skipif(sys.platform != "win32", reason="Windows-only guards (ANSI CRT, GDI)")

CM_UNDO, CM_TOGGLE_SPECTRAL = 2000, 2028   # edit_view.h enum ContextMenuID
REVERSE = ('reaper.defer(function() reaper.Main_OnCommand('
           'reaper.NamedCommandLookup("_SneakPeak_Reverse"), 0) end) return true')


def _sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _diag(s, tag: str):
    """On a timeout the assertion never runs: print what the process shows."""
    import ctypes
    import ctypes.wintypes
    from conftest import window_title
    u32 = ctypes.windll.user32
    pid = s.handle.pid
    caps = []
    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def cb(hwnd, _):
        wp = ctypes.c_ulong()
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(wp))
        if wp.value == pid and u32.IsWindowVisible(hwnd):
            buf = ctypes.create_unicode_buffer(512)
            u32.GetWindowTextW(hwnd, buf, 512)
            rc = ctypes.wintypes.RECT()
            u32.GetWindowRect(hwnd, ctypes.byref(rc))
            caps.append(f"{buf.value!r} {rc.right - rc.left}x{rc.bottom - rc.top}")
        return True
    u32.EnumWindows(cb, 0)
    try:
        title = window_title(s)
    except Exception as e:   # noqa: BLE001
        title = f"<{e!r}>"
    try:
        items = s.eval("return reaper.CountTrackMediaItems(reaper.GetTrack(0, 0))")
    except Exception as e:   # noqa: BLE001
        items = f"<{e!r}>"
    print(f"\n[diag {tag}] title={title!r} items={items} visible windows={caps}")


def _close_error_box(pid: int, wait: float = 8.0) -> bool:
    """An MB_OK 'SneakPeak' box (the control's "Failed to write WAV file")
    would block the session: close it so the assertion, not a timeout,
    reports. Matched by exact caption AND a small frame - our own window is
    captioned 'SneakPeak' too while nothing is loaded."""
    import ctypes
    from ctypes import wintypes
    u32 = ctypes.windll.user32
    t0 = time.monotonic()
    while time.monotonic() - t0 < wait:
        found = []
        @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        def cb(hwnd, _):
            wp = wintypes.DWORD()
            u32.GetWindowThreadProcessId(hwnd, ctypes.byref(wp))
            if wp.value == pid and u32.IsWindowVisible(hwnd):
                buf = ctypes.create_unicode_buffer(128)
                u32.GetWindowTextW(hwnd, buf, 128)
                rc = wintypes.RECT()
                u32.GetWindowRect(hwnd, ctypes.byref(rc))
                w = rc.right - rc.left
                # a message box: small, captioned by the app, never our editor
                # ('SneakPeak: <file>') nor REAPER's About/main window
                if w < 600 and not buf.value.startswith("SneakPeak:") and "About" not in buf.value \
                        and "REAPER v" not in buf.value and "Operation" not in buf.value \
                        and "Peaks" not in buf.value:   # not the confirm, not REAPER's peak-build progress
                    found.append((hwnd, buf.value, w))
            return True
        u32.EnumWindows(cb, 0)
        if found:
            cls = ctypes.create_unicode_buffer(64)
            u32.GetClassNameW(found[0][0], cls, 64)
            print(f"\n[box] closing {found[0][1]!r} class {cls.value!r} ({found[0][2]} px wide)")
            u32.PostMessageW(found[0][0], 0x0010, 0, 0)   # WM_CLOSE: a message box ends as its default/cancel button
            time.sleep(0.5)
            return True
        time.sleep(0.2)
    return False


def _wait_loaded_w(s, name: str, timeout: float = 60.0):
    """wait_audio_loaded through GetWindowTextW: on Windows JS_Window_GetTitle
    hands the bridge an ANSI best-fit caption ('sciezka a' for 'ścieżka ą'),
    so the conftest title oracle cannot see a non-ASCII name."""
    from conftest import _cg_window_title
    since = None
    def done():
        nonlocal since
        t = _cg_window_title(s.handle.pid)
        if name not in t or "Loading" in t:
            since = None
            return False
        since = since or time.monotonic()
        return time.monotonic() - since >= 1.0
    s.wait_until(done, timeout=timeout + 1.0)


def _settle(sess, seconds: float = 60.0) -> bool:
    """Wait for the main thread while closing any error box that comes up on
    the way (the control's "Failed to write WAV file" blocks it). Returns
    whether such a box was seen."""
    t0 = time.monotonic()
    boxed = False
    while time.monotonic() - t0 < seconds:
        if _close_error_box(sess.handle.pid, wait=0.3):
            boxed = True
            continue
        try:
            wait_main_thread_idle(sess, timeout=3)
            return boxed
        except Exception:   # noqa: BLE001  - still blocked: look for the box again
            continue
    _diag(sess, "settle timeout")
    raise TimeoutError("REAPER main thread did not come back")


def _reverse(sess, media: Path):
    sess.eval(REVERSE)
    assert dismiss_native_modal(sess, timeout=15), "the Reverse confirmation never appeared"
    boxed = _settle(sess, 60.0)
    time.sleep(1.0)
    return boxed


def test_reverse_on_a_non_ascii_path_edits_the_file(sess):
    src = burst_fixture("guard_ansi_30s.wav", seconds=30, channels=2)
    folder = perf_media_dir() / "próba ł"
    folder.mkdir(exist_ok=True)
    media = folder / "ścieżka ą.wav"
    shutil.copyfile(src, media)
    clear_project(sess)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    try:
        _wait_loaded_w(sess, media.stem, timeout=60)
    except Exception:
        _diag(sess, "load non-ascii")
        raise
    time.sleep(0.5)
    sha0 = _sha(media)
    w_head, w_tail = (0.6, 1.4), (28.6, 29.4)

    boxed = _reverse(sess, media)

    assert not boxed, "the write failed with an error box (ANSI path)"
    assert _sha(media) != sha0, "the file under the non-ASCII path was not edited"
    head, tail = track_rms_windows(sess, [w_head, w_tail])
    assert db(tail) > db(head) + 20, f"not reversed: head {db(head):.1f} tail {db(tail):.1f}"


def _gdi_objects(pid: int) -> int:
    import ctypes
    k32, u32 = ctypes.windll.kernel32, ctypes.windll.user32
    h = k32.OpenProcess(0x0400 | 0x1000, False, pid)   # QUERY_INFORMATION | QUERY_LIMITED
    assert h, "OpenProcess failed"
    try:
        return int(u32.GetGuiResources(h, 0))          # GR_GDIOBJECTS
    finally:
        k32.CloseHandle(h)


def test_gdi_objects_stay_flat_across_resizes(sess):
    """Spectral view + premium chrome re-create their off-screen bitmaps on
    every size change: 100 size toggles must not grow the GDI object count."""
    _close_error_box(sess.handle.pid, wait=1.0)
    media = burst_fixture("guard_gdi_30s.wav", seconds=30, channels=2)
    clear_project(sess)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=60)
    send_command(sess, CM_TOGGLE_SPECTRAL)
    time.sleep(1.5)

    from conftest import client_size
    def resize(w, h):
        sess.eval(f"local h = SP_WINDOW() if h then reaper.JS_Window_Resize(h, {w}, {h}) end return true")
        time.sleep(0.06)

    resize(800, 400); time.sleep(0.3); small = client_size(sess)
    resize(900, 450); time.sleep(0.3); big = client_size(sess)
    print(f"\n[gdi] client size {small} -> {big}")
    assert big != small, f"JS_Window_Resize had no effect ({small} == {big})"
    for i in range(6):                     # warm-up: caches, fonts, first surfaces
        resize(*((900, 450) if i % 2 else (800, 400)))
    time.sleep(0.5)
    g0 = _gdi_objects(sess.handle.pid)
    for i in range(100):
        resize(*((900, 450) if i % 2 else (800, 400)))
    time.sleep(0.5)
    g1 = _gdi_objects(sess.handle.pid)
    print(f"\n[gdi] objects before {g0} after 100 resizes {g1} (delta {g1 - g0})")
    assert g1 - g0 <= 8, f"GDI objects leak on resize: {g0} -> {g1}"
