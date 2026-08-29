"""Standalone channel solo (s20, user report): clicking the L/R badge used to
STOP the preview (every click stops it) and change nothing audible (solo went
through the take pan, and a Standalone file has no take). The badge now keeps
the preview running, the preview file carries the playing channel alone (a
badge mutes ITS channel - the green badge = playing - so the other side is
silent, the layout kept) and playback restarts at its position.

Oracles: the preview temp WAV (sneakpeak_preview_<pid>.wav in REAPER's temp
dir) - after a solo its other channel is silent (RED on the control, where both
channels keep playing); the ui_state mirror (preview=1 after the badge click);
the badges themselves located by their active green fill (RGB 0,160,60).
"""
from __future__ import annotations

import os
import tempfile
import time
from pathlib import Path

import numpy as np
import soundfile as sf
from scipy import ndimage

from conftest import (capture, clear_project, click_client, client_size, ensure_window, key_sync,
                      perf_media_dir, wait_audio_loaded)

VK_SPACE = 0x20
SHOTS = Path("/tmp/sneakpeak-reaproof-shots/standalone_solo")
OPEN = ('reaper.defer(function() reaper.Main_OnCommand('
        'reaper.NamedCommandLookup("_SneakPeak_OpenStandalone"), 0) end) return true')


def _fixture() -> Path:
    """8 s stereo: L = 220 Hz, R = 440 Hz, both at 0.5 - a solo is a channel
    going silent in the preview file."""
    path = perf_media_dir() / "solo_lr_8s.wav"
    if not path.exists():
        sr = 44100
        t = np.arange(8 * sr) / sr
        y = np.stack([0.5 * np.sin(2 * np.pi * 220 * t), 0.5 * np.sin(2 * np.pi * 440 * t)], axis=1)
        sf.write(str(path), y.astype(np.float32), sr, subtype="PCM_24")
    return path


def _ui(sess) -> dict[str, str]:
    raw = str(sess.eval('return reaper.GetExtState("SneakPeak", "ui_state")'))
    return dict(tok.split("=", 1) for tok in raw.split() if "=" in tok)


def _preview_file(sess) -> Path | None:
    name = f"sneakpeak_preview_{sess.handle.pid}.wav"
    for d in (os.environ.get("TMPDIR"), tempfile.gettempdir(), "/tmp"):
        if d and (Path(d) / name).exists():
            return Path(d) / name
    return None


def _channel_rms(path: Path) -> tuple[float, float]:
    y, _ = sf.read(str(path), dtype="float64", always_2d=True)
    assert y.shape[1] == 2, f"the preview file is not stereo: {y.shape}"
    return float(np.sqrt((y[:, 0] ** 2).mean())), float(np.sqrt((y[:, 1] ** 2).mean()))


def _badges(sess, out: Path) -> list[tuple[int, int]]:
    """Client centres of the active (green) L/R badges, top to bottom."""
    cap = capture(sess, out)
    img = cap.image.astype(int)
    r, g, b = img[..., 0], img[..., 1], img[..., 2]
    mask = (r < 20) & (g > 140) & (g < 215) & (b > 45) & (b < 95)   # (0,160,60) fill + (0,200,80) border
    labels, n = ndimage.label(mask)
    cw, ch = client_size(sess)
    scale = cap.image.shape[1] / float(cw)
    titlebar = cap.height - ch * scale
    out_pts = []
    for i in range(1, n + 1):
        ys, xs = np.nonzero(labels == i)
        if len(xs) < 80:
            continue
        out_pts.append((int(xs.mean() / scale), int((ys.mean() - titlebar) / scale)))
    return sorted(out_pts, key=lambda p: p[1])


def test_channel_badge_keeps_the_preview_playing_with_its_channel_silent(sess):
    media = _fixture()
    clear_project(sess)
    ensure_window(sess)
    sess.eval(f'reaper.SetExtState("SneakPeak", "open_path", "{media.as_posix()}", false) return true')
    sess.eval(OPEN)
    time.sleep(1.0)
    wait_audio_loaded(sess, media.stem, timeout=60)
    SHOTS.mkdir(parents=True, exist_ok=True)
    try:
        key_sync(sess, VK_SPACE, settle=0.8)          # preview from the cursor (0 s)
        sess.wait_until(lambda: _preview_file(sess) is not None, timeout=5)
        l0, r0 = _channel_rms(_preview_file(sess))
        assert l0 > 0.3 and r0 > 0.3, f"precondition: both channels in the preview file ({l0:.2f}, {r0:.2f})"

        badges = _badges(sess, SHOTS / "1_playing.png")
        assert len(badges) >= 2, f"the L/R badges were not found on screen: {badges}"
        click_client(sess, *badges[0])                # L badge: mute L -> R plays alone
        time.sleep(1.0)
        capture(sess, SHOTS / "2_mute_l.png")
        l1, r1 = _channel_rms(_preview_file(sess))
        assert l1 < 0.01 and r1 > 0.3, f"L muted: the preview file still carries L ({l1:.2f}, {r1:.2f})"
        assert _ui(sess).get("preview") == "1", f"the badge click stopped the preview: {_ui(sess)}"

        click_client(sess, *badges[0])                # L back: both play
        time.sleep(1.0)
        l2, r2 = _channel_rms(_preview_file(sess))
        assert l2 > 0.3 and r2 > 0.3, f"L back: the preview file lost a channel ({l2:.2f}, {r2:.2f})"
        assert _ui(sess).get("preview") == "1", f"the second badge click stopped the preview: {_ui(sess)}"
    finally:
        if _ui(sess).get("preview") == "1":
            key_sync(sess, VK_SPACE, settle=0.5)
