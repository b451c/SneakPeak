"""Spectrogram FFT size (v2.5 row 15 #3, design_fft_size.md).

The spectrogram was fixed at 2048 points. Settings > View gains FFT 512 / 1024 /
2048 / 4096 (context-menu ids CM_SPECTRAL_FFT_BASE + i, persisted as ExtState
SneakPeak/spectral_fft); a change recomputes the visible spectrogram.

Observable: the client pixels of the spectral pane. A steady 100 Hz tone paints
one horizontal ridge; its core (rows within ~20 dB of the peak = the Hann main
lobe, +-1.75 bins = +-1.75 * sr / N Hz) spans 62-138 Hz at 2048 and 81-119 Hz
at 4096 - 16 and 8 rows on the log axis of a 130 px pane, phase-independent
(offline simulation of the paint path agrees to the row). At 512 the lobe
reaches the 20 Hz floor and the tone's mirror image around DC, so the width
there depends on the tone phase: that leg only checks the direction. The
threshold is relative to the pane's own peak, so the capture's colour profile
cancels out; the sidelobes (-31 dB and below, whose width in BINS does not
shrink with N) stay out. RED on a build without the option: the command ids
fall through the `id < CM_LAST` gate, every measurement equals the default and
the ExtState stays empty.
"""
from __future__ import annotations

import time
from pathlib import Path

import numpy as np
import soundfile as sf

from conftest import (SELECT_ITEM0, capture, clear_project, client_size, ensure_window,
                      insert_item_unselected, perf_media_dir, send_command, wait_audio_loaded)

SHOTS = Path("/tmp/sneakpeak-reaproof-shots/fft")
CM_TOGGLE_SPECTRAL = 2028          # edit_view.h enum ContextMenuID (CM_UNDO = 2000)
CM_SPECTRAL_FFT_BASE = 2260        # + 0..3 = 512 / 1024 / 2048 / 4096 (evaluated 2026-08-29)
SIZES = (512, 1024, 2048, 4096)
CORE = 0.85                        # ridge core: luminance >= 0.85 x the pane's peak (~-20 dB)


def _tone_fixture() -> Path:
    """6 s mono 100 Hz sine at 0.5, 24-bit. Never edited (no pristine copy needed)."""
    media = perf_media_dir() / "tone100_mono_6s.wav"
    if not media.exists():
        sr = 44100
        t = np.arange(6 * sr) / sr
        sf.write(str(media), (0.5 * np.sin(2 * np.pi * 100.0 * t)).astype(np.float32),
                 sr, subtype="PCM_24")
    return media


def _pane_rows(s, out: Path) -> np.ndarray:
    """Ridge-core row count per pane column (UI scale 1 geometry as in
    test_lazy_buffer: mode bar 20 + ruler 28 above, minimap 20 + scrollbar 14 +
    bottom panel 52 below, waveform 55 % of the content, 5 px splitter; dB
    column excluded). Core = luminance within CORE of the pane's peak."""
    cap = capture(s, out)
    cw, ch = client_size(s)
    titlebar = cap.height - ch
    content_top, content_bot = 48, ch - 86
    wave_h = int((content_bot - content_top) * 0.55) - 2
    pane_top, pane_bot = content_top + wave_h + 5, content_bot
    band = cap.image[titlebar + pane_top:titlebar + pane_bot, 4:cw - 46, :3].astype(int)
    lum = band.max(axis=2)
    return (lum >= CORE * lum.max()).sum(axis=0)


def _ridge_rows(s, tag: str, timeout: float = 30.0) -> float:
    """Median lit rows per column once the spectrogram is painted and stable
    (two captures 0.5 s apart agree; the progress overlay never does)."""
    last = None
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        rows = _pane_rows(s, SHOTS / f"{tag}.png")
        med = float(np.median(rows))
        if med >= 3 and last is not None and abs(med - last) <= 1:
            return med
        last = med
        time.sleep(0.5)
    raise AssertionError(f"spectrogram never settled for {tag} (last median rows {last})")


def _ext(s, key: str) -> str:
    return str(s.eval(f'return reaper.GetExtState("SneakPeak", "{key}")'))


def test_fft_size_changes_the_ridge_thickness_and_persists(sess):
    media = _tone_fixture()
    clear_project(sess)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=30)
    SHOTS.mkdir(parents=True, exist_ok=True)

    send_command(sess, CM_TOGGLE_SPECTRAL)
    rows0 = _ridge_rows(sess, "fft_default")            # 2048 (the shipped default)

    send_command(sess, CM_SPECTRAL_FFT_BASE + SIZES.index(4096))
    time.sleep(0.5)                                     # the recompute clears the pane first
    rows4096 = _ridge_rows(sess, "fft_4096")
    send_command(sess, CM_SPECTRAL_FFT_BASE + SIZES.index(512))
    time.sleep(0.5)
    rows512 = _ridge_rows(sess, "fft_512")
    ext512 = _ext(sess, "spectral_fft")
    send_command(sess, CM_SPECTRAL_FFT_BASE + SIZES.index(2048))
    time.sleep(0.5)
    rows2048 = _ridge_rows(sess, "fft_2048")
    ext2048 = _ext(sess, "spectral_fft")
    send_command(sess, CM_TOGGLE_SPECTRAL)              # leave the view as we found it

    m = {"rows_default": rows0, "rows_4096": rows4096, "rows_512": rows512,
         "rows_2048": rows2048, "ext_512": ext512, "ext_2048": ext2048}
    print(f"\n[fft] {m}")
    # 4096 halves the core exactly (offline simulation of the paint path: 16 -> 8 rows,
    # phase-independent). At 512 the tone sits in bin 1.16, so its mirror image around
    # DC and the 20 Hz display floor make the width phase-dependent (31-46 rows simulated,
    # ~27 measured over the column median): a direction check, not a ratio.
    assert rows4096 <= 0.65 * rows0, f"FFT 4096 did not sharpen the 100 Hz ridge: {m}"
    assert rows512 >= 1.5 * rows0, f"FFT 512 did not widen the 100 Hz ridge: {m}"
    assert abs(rows2048 - rows0) <= 0.15 * rows0 + 1, f"FFT 2048 is not the default look: {m}"
    assert ext512 == "512" and ext2048 == "2048", f"the FFT size did not persist: {m}"
