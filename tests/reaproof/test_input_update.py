"""Update check off the UI thread (v2.5.0 audit A5.4) - own module: the spec
runs its own REAPER session (SNEAKPEAK_UPDATE_URL points curl at a dead
address) and on Windows JS_Window_ListFind is process-agnostic, so it must
not share a module with a live `sess`.

Clicking the version label used to run curl synchronously (-m 5): the main
thread stopped for the whole request. Now the request runs on a worker and
the reply lands as a toast. Ground truth: the bridge heartbeat (REAPER's
own clock) after the click - longest gap < 0.1 s. Control (0a444ad): the
click blocks for the real GitHub round trip (hundreds of ms) or 5 s.
"""
from __future__ import annotations

import json
import os
import threading
import time
from pathlib import Path

from conftest import DYLIB, SP_WINDOW_LUA, ensure_window, wait_main_thread_idle

VERSION_LABEL = (640, 13)   # "v2.5.0" in the mode bar (client coords) of the 800x400 spec window


def _heartbeat_gaps(sess, seconds: float) -> float:
    """Longest gap between consecutive heartbeat ticks (REAPER clock) over `seconds`."""
    run_dir = Path(sess.profile.run_dir)
    hb = run_dir / "_reaproof" / "heartbeat.json"
    if not hb.exists():
        hb = next(run_dir.rglob("heartbeat.json"))
    last = None
    worst = 0.0
    t0 = time.monotonic()
    while time.monotonic() - t0 < seconds:
        try:
            t = float(json.loads(hb.read_text())["t"])
        except Exception:   # noqa: BLE001 - a half-written file
            time.sleep(0.005)
            continue
        if last is not None and t > last:
            worst = max(worst, t - last)
        last = t
        time.sleep(0.005)
    return worst


def test_update_check_does_not_block_the_main_thread():
    from reaproof.runner.session import ReaperSession
    saved = os.environ.get("SNEAKPEAK_UPDATE_URL")
    os.environ["SNEAKPEAK_UPDATE_URL"] = "http://10.255.255.1/releases"   # non-routable: curl -m 5 times out
    try:
        s = ReaperSession("sneakpeak-update", extensions=[DYLIB]).start()
    finally:
        if saved is None:
            os.environ.pop("SNEAKPEAK_UPDATE_URL", None)
        else:
            os.environ["SNEAKPEAK_UPDATE_URL"] = saved
    try:
        s.eval(SP_WINDOW_LUA)
        ensure_window(s)
        time.sleep(1.0)
        result = {}
        def probe():
            result["worst"] = _heartbeat_gaps(s, 7.0)
        th = threading.Thread(target=probe, daemon=True)
        th.start()
        time.sleep(0.5)
        x, y = VERSION_LABEL
        s.eval(f'reaper.defer(function() local h = SP_WINDOW() '
               f'reaper.JS_WindowMessage_Send(h, "WM_LBUTTONDOWN", 1, 0, {x}, {y}) '
               f'reaper.JS_WindowMessage_Send(h, "WM_LBUTTONUP", 0, 0, {x}, {y}) end) return true')
        th.join()
        wait_main_thread_idle(s, timeout=30)
        toast = s.eval('return reaper.GetExtState("SneakPeak", "last_toast")')
        print(f"\n[update] longest main-thread gap after the click: {result['worst']:.3f} s; toast {toast!r}")
        assert result["worst"] < 0.1, f"the update check froze the main thread for {result['worst']:.2f} s"
    finally:
        s.stop()
