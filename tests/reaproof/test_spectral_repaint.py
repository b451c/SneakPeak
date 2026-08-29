"""A Standalone edit must reveal its recomputed spectrogram (v2.5 s16 follow-up).

Every Standalone edit (heal, click repair, reverse, gain, DC, undo/redo) drops
the spectrogram and the next paint recomputes it on a worker. The window is
repainted by a 33 ms timer pump that fires while the compute is seen running -
on a short file the compute finishes before the first tick ever sees it, so the
"Computing spectrum... 0%" overlay stays frozen over a finished spectrogram
until something else repaints (found on the FFT-size increment, fixed there
for the FFT command only).

Observable: the client pixels of the spectral pane after Repair Clicks and
after its Undo - the 100 Hz ridge must be back within a few seconds; RED on a
build where the pane keeps the frozen overlay (no ridge rows at all).
"""
from __future__ import annotations

import time
from pathlib import Path

import numpy as np
import soundfile as sf

from conftest import (capture, clear_project, client_size, command_sync, drag_client,
                      ensure_window, perf_media_dir, send_command, wait_audio_loaded)

SHOTS = Path("/tmp/sneakpeak-reaproof-shots/repaint")
CM_TOGGLE_SPECTRAL = 2028      # edit_view.h enum ContextMenuID (CM_UNDO = 2000)
CM_UNDO = 2000
CM_REPAIR_CLICKS = 2173        # evaluated by compiling the enum, 2026-08-29
CORE = 0.85
WAVE_Y = 120                   # inside the waveform lane with the spectral pane open (55 % split)
OPEN = ('reaper.defer(function() reaper.Main_OnCommand('
        'reaper.NamedCommandLookup("_SneakPeak_OpenStandalone"), 0) end) return true')


def _click_fixture() -> Path:
    """6 s mono 100 Hz tone at 0.5 with three one-sample clicks at 1.7 / 1.8 /
    1.9 s (inside the spec's ~1.6-2.2 s drag), 24-bit. Standalone edits stay in
    memory, so the file is never rewritten."""
    media = perf_media_dir() / "tone100_clicks_6s.wav"
    if not media.exists():
        sr = 44100
        t = np.arange(6 * sr) / sr
        y = 0.5 * np.sin(2 * np.pi * 100.0 * t)
        for at in (1.7, 1.8, 1.9):
            y[int(at * sr)] = 0.95
        sf.write(str(media), y.astype(np.float32), sr, subtype="PCM_24")
    return media


def _ridge_rows(s, out: Path) -> float:
    """Median ridge-core rows per pane column (geometry as in test_spectral_fft)."""
    cap = capture(s, out)
    cw, ch = client_size(s)
    titlebar = cap.height - ch
    content_top, content_bot = 48, ch - 86
    wave_h = int((content_bot - content_top) * 0.55) - 2
    pane_top, pane_bot = content_top + wave_h + 5, content_bot
    band = cap.image[titlebar + pane_top:titlebar + pane_bot, 4:cw - 46, :3].astype(int)
    lum = band.max(axis=2)
    return float(np.median((lum >= CORE * lum.max()).sum(axis=0)))


def _wait_ridge(s, tag: str, timeout: float) -> float:
    last = 0.0
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        last = _ridge_rows(s, SHOTS / f"{tag}.png")
        if last >= 3:
            return last
        time.sleep(0.5)
    return last


def _toast(s) -> str:
    return str(s.eval('return reaper.GetExtState("SneakPeak", "last_toast")'))


def test_standalone_repair_and_undo_reveal_the_recomputed_spectrogram(sess):
    media = _click_fixture()
    clear_project(sess)
    ensure_window(sess)
    sess.eval('reaper.DeleteExtState("SneakPeak", "last_toast", false)')
    sess.eval(f'reaper.SetExtState("SneakPeak", "open_path", "{media.as_posix()}", false) return true')
    sess.eval(OPEN)
    wait_audio_loaded(sess, media.name, timeout=60)
    time.sleep(0.5)
    SHOTS.mkdir(parents=True, exist_ok=True)

    send_command(sess, CM_TOGGLE_SPECTRAL)              # posted: before any Send in this session
    rows0 = _wait_ridge(sess, "open", timeout=20)
    assert rows0 >= 3, f"the spectrogram never painted after opening the pane ({rows0} rows)"

    drag_client(sess, 200, WAVE_Y, 275, WAVE_Y)         # ~1.6-2.2 s: the three clicks
    command_sync(sess, CM_REPAIR_CLICKS)
    toast = _toast(sess)
    assert toast.startswith("Repaired"), f"click repair did not run: {toast!r}"
    rows_after = _wait_ridge(sess, "after_repair", timeout=4)

    command_sync(sess, CM_UNDO)
    rows_undo = _wait_ridge(sess, "after_undo", timeout=4)

    m = {"rows_open": rows0, "rows_after_repair": rows_after, "rows_after_undo": rows_undo, "toast": toast}
    print(f"\n[repaint] {m}")
    assert rows_after >= 3, f"the pane kept the frozen overlay after Repair Clicks: {m}"
    assert rows_undo >= 3, f"the pane kept the frozen overlay after Undo: {m}"
