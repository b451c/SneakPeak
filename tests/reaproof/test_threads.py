"""Threading consistency (v2.5.0 audit, increment A8).

A8.1 - every REAPER audio-accessor call (create / validate / state-changed /
destroy / read) now takes one process-wide lock (AudioStream::ApiLock), so
the main thread's loader, change poll and exports can never be inside the
accessor API while the dynamics trace worker reads its own accessor. The
hazard was never proven (SDK: create/destroy main thread only, reads have no
stated rule); this stress keeps REAPER's main loop under observation while
the two overlap 20 times: selecting an item with the Dynamics panel open
starts the trace job on the worker and the item load on the main thread.
"""
from __future__ import annotations

import json
import threading
import time
from pathlib import Path

from conftest import (CM_APPLY_DYNAMICS, DESELECT_ALL, SELECT_ITEM0, burst_fixture, clear_project,
                      command_sync, ensure_window, insert_item_unselected, locate_apply_button,
                      wait_audio_loaded, wait_main_thread_idle)

SHOTS = Path("/tmp/sneakpeak-reaproof-shots/threads")
RESULTS = Path("/tmp/sneakpeak-perf-results.json")
STALL_BUDGET = 0.1


def _record(name: str, m: dict):
    data = json.loads(RESULTS.read_text()) if RESULTS.exists() else {}
    data[name] = m
    RESULTS.write_text(json.dumps(data, indent=1))
    print(f"\n[perf] {name}: {m}")


def test_select_short_item_with_dynamics_x20(sess):
    SHOTS.mkdir(parents=True, exist_ok=True)
    media = burst_fixture("threads_10s.wav", seconds=10, channels=2)
    clear_project(sess)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=60)
    if locate_apply_button(sess, SHOTS / "panel.png") is None:
        command_sync(sess, CM_APPLY_DYNAMICS, settle=1.0)           # the panel: every select traces
        sess.wait_until(lambda: locate_apply_button(sess, SHOTS / "panel.png") is not None, timeout=15)
    wait_main_thread_idle(sess, timeout=60)

    hb = sess.bridge.heartbeat
    samples: list[tuple[int, float]] = []
    stop = threading.Event()

    def probe():
        last = None
        while not stop.is_set():
            try:
                d = json.loads(hb.read_text(encoding="utf-8", errors="replace"))
                tick, t = int(d["tick"]), float(d["t"])
                if tick != last:
                    samples.append((tick, t))
                    last = tick
            except (OSError, ValueError, KeyError, TypeError):
                pass
            time.sleep(0.003)

    th = threading.Thread(target=probe, daemon=True)
    th.start()
    time.sleep(0.3)
    t0 = float(sess.eval("return reaper.time_precise()"))
    for _ in range(20):
        sess.eval(DESELECT_ALL)
        time.sleep(0.15)
        sess.eval(SELECT_ITEM0)                                     # trace job (worker) + item load (main)
        time.sleep(0.35)
    wait_main_thread_idle(sess, timeout=60)
    t1 = float(sess.eval("return reaper.time_precise()"))
    stop.set()
    th.join(timeout=2)
    max_stall = 0.0
    for (k0, ta), (k1, tb) in zip(samples, samples[1:]):
        if tb <= t0 or k1 <= k0:
            continue
        max_stall = max(max_stall, (tb - ta) / (k1 - k0))
    alive = bool(sess.eval("return reaper.GetPlayState() ~= nil"))
    m = {"max_stall": round(max_stall, 3), "ticks": len(samples), "elapsed": round(t1 - t0, 2), "alive": alive}
    _record("threads.select_x20_dynamics", m)
    assert alive and len(samples) > 20, f"REAPER's main loop stopped during the stress: {m}"
    assert max_stall <= STALL_BUDGET, f"a select with the trace worker running stalled the main thread: {m}"
