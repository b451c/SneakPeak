"""Windows: temp files under a non-ASCII %TEMP% (v2.5.0 audit A3.2).

Own module: this spec runs its own REAPER with TMP/TEMP redirected, and on
Windows JS_Window_ListFind sees every process's windows - a second REAPER
alive next to the module-scoped `sess` of another spec file makes SP_WINDOW()
pick the wrong editor. A module of its own means the previous session has been
torn down before this one starts.
"""
from __future__ import annotations

import sys
import time
from pathlib import Path

import pytest

from conftest import (burst_fixture, clear_project, db, ensure_window, perf_media_dir,
                      send_command, track_rms_windows, wait_audio_loaded, wait_main_thread_idle)
from test_windows_guards import CM_UNDO, _diag, _reverse  # noqa: E402

pytestmark = pytest.mark.skipif(sys.platform != "win32", reason="Windows-only (ANSI CRT remove)")


def test_undo_copy_is_removed_under_a_non_ascii_temp():
    """Own REAPER session with TMP/TEMP under an accented folder: Reverse
    writes sneakpeak_undo_*.wav there, Undo restores and must remove it."""
    import os
    from conftest import DYLIB, SP_WINDOW_LUA
    from reaproof.runner.session import ReaperSession
    tmp = perf_media_dir() / "tëmp ą"
    tmp.mkdir(exist_ok=True)
    saved = {n: os.environ.get(n) for n in ("TMP", "TEMP")}
    for n in saved:
        os.environ[n] = str(tmp)
    try:
        s = ReaperSession("sneakpeak-utf8tmp", extensions=[DYLIB]).start()
    finally:
        for n, v in saved.items():
            if v is None:
                os.environ.pop(n, None)
            else:
                os.environ[n] = v
    try:
        s.eval(SP_WINDOW_LUA)
        _run_utf8_temp_body(s, tmp)
    except Exception:
        _diag(s, "utf8 temp session")
        raise
    finally:
        s.stop()


def _run_utf8_temp_body(s, tmp: Path):
    from conftest import insert_item
    if True:
        clear_project(s)
        media = burst_fixture("guard_utf8tmp_30s.wav", seconds=30, channels=2)
        ensure_window(s)
        insert_item(s, media)            # inserted selected: SneakPeak loads it
        wait_audio_loaded(s, media.stem, timeout=60)
        time.sleep(0.5)
        before = sorted(p.name for p in tmp.glob("sneakpeak_*"))
        w_head, w_tail = (0.6, 1.4), (28.6, 29.4)

        _reverse(s, media)
        head, tail = track_rms_windows(s, [w_head, w_tail])
        assert db(tail) > db(head) + 20, "precondition: the reverse did not land"
        during = sorted(p.name for p in tmp.glob("sneakpeak_undo_*"))
        assert during, f"no undo copy under the accented temp folder: {tmp}"

        send_command(s, CM_UNDO)
        wait_main_thread_idle(s, timeout=120)
        wait_audio_loaded(s, media.stem, timeout=60)
        time.sleep(1.0)
        head, tail = track_rms_windows(s, [w_head, w_tail])
        assert db(head) > db(tail) + 20, "undo did not restore the original"
        after = sorted(p.name for p in tmp.glob("sneakpeak_*"))
        assert after == before, f"temp files left behind under the accented folder: {sorted(set(after) - set(before))}"


