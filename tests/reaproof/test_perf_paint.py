"""Paint profile during playback (audit A9.1 - MEASURE FIRST).

Every timer tick of a playing transport invalidates the whole window
(UpdatePlaybackFollow), so playback is a steady stream of full repaints:
OnPaint (the waveform, two passes per channel with Envelope_Evaluate per
column when a take envelope exists) + OnPaintOverlay (the premium panel via
UiCanvas::RenderPanel). This spec records what that costs on a 20-minute
item carrying the dense envelope Apply Dynamics writes, with the panel open,
on the largest window the 2x display holds:

  paint.playback_20min = {max_gap, mean_gap, ticks (bridge heartbeat, REAPER's
  clock), dpr, points, sample shares of the profiled symbols over the play
  window (`sample <pid> 10 -file`, main thread, inclusive), top inclusive
  frames inside OnPaint and inside our dylib}

No budget: the A9.2-A9.5 rows compare their AFTER numbers against the BEFORE
this spec recorded (plan_v250_audit_fixes.md A9). The raw sample file is kept
under /tmp/sneakpeak-perf-profiles/.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import threading
import time
from pathlib import Path

import numpy as np

from conftest import (CM_APPLY_DYNAMICS, SELECT_ITEM0, capture, clear_project,
                      click_client, client_size, ensure_window, insert_item_unselected,
                      perf_media_dir, send_command,
                      take_envelope_points, wait_audio_loaded,
                      wait_main_thread_idle, window_handle_lua, window_title,
                      write_long_wav)

RESULTS = Path("/tmp/sneakpeak-perf-results.json")
PROFILES = Path("/tmp/sneakpeak-perf-profiles")
SHOTS = Path("/tmp/sneakpeak-reaproof-shots/paint")
WINDOW_W, WINDOW_H = 1400, 800      # the 2x display is 1496x967 points; 1600x900 does not fit
PLAY_SECONDS = 10.0
LABEL = os.environ.get("SNEAKPEAK_PAINT_LABEL", "current")     # names the record + profile file
WANT_SCALE = float(os.environ.get("SNEAKPEAK_PAINT_SCALE", "2"))  # display backing scale to run on
PROFILED = ["SneakPeak::OnPaint", "WaveformView::Paint", "WaveformView::DrawWaveformChannel",
            "Envelope_Evaluate", "SneakPeak::OnPaintOverlay", "UiCanvas::RenderPanel",
            "SneakPeak::DrawDynamicsCurve", "WaveformView::DrawVolumeEnvelope"]
PLAY, STOP = 1007, 1016


def _record(name: str, m: dict):
    data = json.loads(RESULTS.read_text()) if RESULTS.exists() else {}
    data[name] = m
    RESULTS.write_text(json.dumps(data, indent=1))
    print(f"\n[perf] {name}: {json.dumps(m, indent=1)}")


def _place_window(sess) -> float:
    """Move/resize the floating window onto the 2x display when there is one
    (else the main display) and return the backing scale it landed on, read
    back from the window's real bounds via CGWindowList."""
    import Quartz
    displays = Quartz.CGGetActiveDisplayList(8, None, None)[1]

    def scale_of(d):
        mode = Quartz.CGDisplayCopyDisplayMode(d)
        return Quartz.CGDisplayModeGetPixelWidth(mode) / Quartz.CGDisplayModeGetWidth(mode)
    target = min(displays, key=lambda d: (abs(scale_of(d) - WANT_SCALE), -scale_of(d)))
    b = Quartz.CGDisplayBounds(target)
    x, y = int(b.origin.x) + 40, int(b.origin.y) + 60
    # SWELL maps a y below the main display back onto it, so the move goes
    # through System Events (global top-left coordinates, like Quartz).
    subprocess.run(["osascript", "-e", 'tell application "System Events" to tell process "REAPER"',
                    "-e", f'set position of window "SneakPeak" to {{{x}, {y}}}',
                    "-e", f'set size of window "SneakPeak" to {{{WINDOW_W}, {WINDOW_H}}}',
                    "-e", "end tell"], capture_output=True, text=True, timeout=20)
    time.sleep(0.5)
    pid = sess.handle.pid
    for w in Quartz.CGWindowListCopyWindowInfo(Quartz.kCGWindowListOptionOnScreenOnly, Quartz.kCGNullWindowID):
        if w.get("kCGWindowOwnerPID") != pid or "SneakPeak" not in str(w.get("kCGWindowName", "")):
            continue
        r = w["kCGWindowBounds"]
        print(f"\n[paint] window {w.get('kCGWindowName')!r} bounds {dict(r)} (asked {x},{y} {WINDOW_W}x{WINDOW_H})")
        cx, cy = r["X"] + r["Width"] / 2, r["Y"] + r["Height"] / 2
        for d in displays:
            db = Quartz.CGDisplayBounds(d)
            if db.origin.x <= cx < db.origin.x + db.size.width and db.origin.y <= cy < db.origin.y + db.size.height:
                return scale_of(d)
    return 0.0


def _locate_apply(sess, out: Path) -> tuple[int, int] | None:
    """conftest.locate_apply_button for any backing scale: the capture is in
    pixels (2x on a Retina display), the click wants client points."""
    from scipy import ndimage
    cap = capture(sess, out)
    img = cap.image.astype(int)
    r, g, b = img[..., 0], img[..., 1], img[..., 2]
    mask = (r > 170) & (g > 110) & (g < 200) & (b < 90)
    labels, n = ndimage.label(mask)
    if n == 0:
        return None
    sizes = ndimage.sum(mask, labels, range(1, n + 1))
    best = int(np.argmax(sizes)) + 1
    if sizes[best - 1] < 800:
        return None
    ys, xs = np.nonzero(labels == best)
    cw, ch = client_size(sess)
    scale = img.shape[1] / cw if cw else 1.0
    return int(xs.mean() / scale), int(ys.mean() / scale - (img.shape[0] / scale - ch))


def _wait_title_settled(sess, name: str, timeout: float):
    def done():
        t = window_title(sess)
        return name in t and "Loading" not in t and "Analysing" not in t
    sess.wait_until(done, timeout=timeout)


def parse_sample(text: str, symbols: list[str]) -> dict:
    """Inclusive sample counts on the main thread of a `sample` call graph.
    Returns {total, shares{symbol: fraction}, top_onpaint[(frame, fraction)],
    top_dylib[(frame, fraction)]}. A symbol at several nodes is summed."""
    node_re = re.compile(r"^([ +!:|]*)(\d+) (.+?)  \(in (.+?)\)")   # depth = prefix length
    in_main = False
    total = 0
    counts: dict[str, int] = {s: 0 for s in symbols}
    dylib: dict[str, int] = {}
    onpaint: dict[str, int] = {}
    onpaint_col = None
    for line in text.splitlines():
        root = re.match(r"^\s{4}(\d+) Thread_", line)
        if root:
            in_main = total == 0            # sample lists the main thread first ("Thread_N: reaper")
            if in_main:
                total = int(root.group(1))
            continue
        if not in_main:
            continue
        m = node_re.match(line)
        if not m:
            continue
        col, n, frame, image = len(m.group(1)), int(m.group(2)), m.group(3), m.group(4)
        for s in symbols:
            if frame.startswith(s + "(") and "::$_" not in frame:   # the function itself, not its lambdas
                counts[s] += n
        if image == "reaper_sneakpeak.dylib":
            dylib[frame] = dylib.get(frame, 0) + n
        if onpaint_col is not None and col <= onpaint_col:
            onpaint_col = None
        if onpaint_col is None:
            if "SneakPeak::OnPaint(" in frame:
                onpaint_col = col
        elif col > onpaint_col:
            onpaint[frame] = onpaint.get(frame, 0) + n
    if not total:
        return {"total": 0, "shares": {}, "top_onpaint": [], "top_dylib": []}

    def top(d, k=8):
        return [(f[:70], round(c / total, 3)) for f, c in sorted(d.items(), key=lambda kv: -kv[1])[:k]]
    return {"total": total,
            "shares": {s: round(c / total, 3) for s, c in counts.items()},
            "top_onpaint": top(onpaint), "top_dylib": top(dylib)}


def test_playback_paint_profile_20min_dense_envelope(sess):
    media = write_long_wav(perf_media_dir() / "long20min_stereo.wav", minutes=20)
    SHOTS.mkdir(parents=True, exist_ok=True)
    PROFILES.mkdir(parents=True, exist_ok=True)
    clear_project(sess)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    dpr = _place_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=120)

    # the Dynamics panel stays open; Apply writes the dense envelope (pending
    # until the streamed analysis lands, as in test_dynamics_stream)
    send_command(sess, CM_APPLY_DYNAMICS)
    sess.wait_until(lambda: _locate_apply(sess, SHOTS / "panel.png") is not None, timeout=15)
    _wait_title_settled(sess, media.stem, timeout=300)
    click_client(sess, *_locate_apply(sess, SHOTS / "panel.png"))
    sess.wait_until(lambda: len(take_envelope_points(sess)) > 4, timeout=120)
    wait_main_thread_idle(sess, timeout=60)
    points = len(take_envelope_points(sess))
    capture(sess, SHOTS / "applied.png")

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
    profile = PROFILES / f"paint_playback_20min_{LABEL}.txt"
    t_play = float(sess.eval(f"reaper.SetEditCurPos(0, false, false) reaper.Main_OnCommand({PLAY}, 0) "
                             "return reaper.time_precise()"))
    sampler = subprocess.Popen(["sample", str(sess.handle.pid), str(int(PLAY_SECONDS)), "-mayDie",
                                "-file", str(profile)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(PLAY_SECONDS + 0.5)
    t_stop = float(sess.eval(f"reaper.Main_OnCommand({STOP}, 0) return reaper.time_precise()"))
    stop.set()
    th.join(timeout=2)
    sampler.wait(timeout=120)
    playing_state = int(sess.eval("return reaper.GetPlayState()"))

    gaps = []
    for (k0, t0), (k1, t1) in zip(samples, samples[1:]):
        if t1 <= t_play or t0 >= t_stop or k1 <= k0:
            continue
        gaps.append((t1 - t0) / (k1 - k0))
    prof = parse_sample(profile.read_text(errors="replace"), PROFILED) if profile.exists() else {}
    m = {"max_gap": round(max(gaps), 3) if gaps else None,
         "mean_gap": round(sum(gaps) / len(gaps), 4) if gaps else None,
         "ticks": len(gaps) + 1, "play_s": round(t_stop - t_play, 2),
         "dpr": dpr, "window": [WINDOW_W, WINDOW_H], "points": points,
         "sample_total": prof.get("total"), "shares": prof.get("shares"),
         "top_onpaint": prof.get("top_onpaint"), "top_dylib": prof.get("top_dylib"),
         "profile": str(profile)}
    _record(f"paint.playback_20min.{LABEL}", m)

    assert playing_state == 0, "transport did not stop"
    assert points > 1000, f"the envelope is not dense enough to profile: {m}"
    assert len(gaps) >= 20, f"REAPER's main loop barely ticked during playback: {m}"
    assert prof.get("total"), f"sample produced no main-thread call graph: {m}"
