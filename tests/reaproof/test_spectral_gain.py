"""The spectrogram follows the amplitude (s20, user): the item volume (the gain
knob in ITEM mode = D_VOL) shifts the spectrogram's colours like it scales the
waveform, and a gain baked into a Standalone buffer from the knob recomputes
the spectrogram (Gain / Normalize / Reverse already did since s18).

Oracles: the mean brightness of the spectral pane in a window capture (rows
58..83% of the client: the spectral pane in the 800x428 test window with the
view split), the ExtState ui_state mirror ("gain=l,t,r,b" = the knob bubble).
"""
from __future__ import annotations

import time
from pathlib import Path

import numpy as np

from conftest import (SELECT_ITEM0, capture, clear_project, client_size, drag_client, ensure_window,
                      insert_item_unselected, perf_media_dir, send_command, wait_audio_loaded,
                      window_title, write_long_wav)

CM_TOGGLE_SPECTRAL = 2028   # edit_view.h ContextMenuID (compiled enum)
SHOTS = Path("/tmp/sneakpeak-reaproof-shots/spectral_gain")
OPEN = ('reaper.defer(function() reaper.Main_OnCommand('
        'reaper.NamedCommandLookup("_SneakPeak_OpenStandalone"), 0) end) return true')


def _ui(sess) -> dict[str, str]:
    raw = str(sess.eval('return reaper.GetExtState("SneakPeak", "ui_state")'))
    return dict(tok.split("=", 1) for tok in raw.split() if "=" in tok)


def _brightness(sess, out: Path) -> float:
    cap = capture(sess, out)
    cw, ch = client_size(sess)
    scale = cap.image.shape[1] / float(cw)
    titlebar = int(round(cap.height - ch * scale))
    y0, y1 = titlebar + int(0.58 * ch * scale), titlebar + int(0.83 * ch * scale)
    band = cap.image[y0:y1, :, :3].astype(float)
    return float(band.max(axis=2).mean())


def _spectral_on(sess):
    if _ui(sess).get("spectral") != "1":
        send_command(sess, CM_TOGGLE_SPECTRAL)
    sess.wait_until(lambda: _ui(sess).get("spectral") == "1", timeout=5)
    sess.wait_until(lambda: "Computing" not in window_title(sess), timeout=60)
    time.sleep(1.0)


def _spectral_off(sess):
    if _ui(sess).get("spectral") == "1":
        send_command(sess, CM_TOGGLE_SPECTRAL)


def test_item_volume_shifts_the_spectrogram(sess):
    """-12 dB of item volume (D_VOL 0.25, set from the API like the knob does)
    darkens the spectrogram; 0 dB brings it back."""
    media = write_long_wav(perf_media_dir() / "specgain_15s.wav", minutes=0.25)
    clear_project(sess)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=60)
    SHOTS.mkdir(parents=True, exist_ok=True)
    set_vol = lambda v: sess.eval("local it = reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), 0) "
                                  f"reaper.SetMediaItemInfo_Value(it, 'D_VOL', {v}) reaper.UpdateArrange() return true")
    try:
        _spectral_on(sess)
        b0 = _brightness(sess, SHOTS / "item_1_unity.png")
        assert b0 > 40, f"precondition: the spectral pane is dark at unity ({b0:.1f})"
        set_vol(0.25)
        time.sleep(1.5)
        b1 = _brightness(sess, SHOTS / "item_2_minus12.png")
        assert b1 < b0 * 0.85, f"-12 dB of item volume left the spectrogram as bright: {b0:.1f} -> {b1:.1f}"
        set_vol(1.0)
        time.sleep(1.5)
        b2 = _brightness(sess, SHOTS / "item_3_unity_again.png")
        assert abs(b2 - b0) < b0 * 0.05, f"back at unity the spectrogram differs: {b0:.1f} -> {b2:.1f}"
    finally:
        set_vol(1.0)
        _spectral_off(sess)


def test_standalone_knob_gain_recomputes_the_spectrogram(sess):
    """Dragging the gain knob down in Standalone bakes the gain into the buffer
    on release - the spectrogram must darken with it."""
    media = write_long_wav(perf_media_dir() / "specgain_sa_15s.wav", minutes=0.25)
    clear_project(sess)
    ensure_window(sess)
    sess.eval(f'reaper.SetExtState("SneakPeak", "open_path", "{media.as_posix()}", false) return true')
    sess.eval(OPEN)
    time.sleep(1.0)
    wait_audio_loaded(sess, media.stem, timeout=60)
    SHOTS.mkdir(parents=True, exist_ok=True)
    try:
        _spectral_on(sess)
        b0 = _brightness(sess, SHOTS / "sa_1_before.png")
        assert b0 > 40, f"precondition: the spectral pane is dark ({b0:.1f})"
        g = _ui(sess).get("gain", "0,0,0,0").split(",")
        l, t, r, b = (int(v) for v in g)
        assert r > l and b > t, f"the gain knob bubble is not on screen: {g}"
        kx, ky = l + (b - t) // 2, (t + b) // 2       # the knob: a circle at the bubble's left
        drag_client(sess, kx, ky, kx, ky + 80)        # down = quieter; the release bakes it
        sess.wait_until(lambda: "Computing" not in window_title(sess), timeout=60)
        time.sleep(1.5)
        b1 = _brightness(sess, SHOTS / "sa_2_after.png")
        assert b1 < b0 * 0.9, f"the baked gain left the spectrogram as bright: {b0:.1f} -> {b1:.1f}"
    finally:
        _spectral_off(sess)
