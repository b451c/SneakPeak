"""Amplitude/order edits must show in the open spectrogram (v2.5 s18).

The spectrogram is computed from the display buffer on a fixed dBFS scale
(-90..0, spectral_view.cpp), so a gain change brightens it and a reverse
mirrors it - IF the edit drops the old spectrum. Standalone Gain / Normalize /
Reverse / DC Remove only invalidated the waveform: the pane kept the pre-edit
picture. And every bare ClearSpectrum (opening another Standalone file,
switching its tabs) left the pane on a frozen "Computing spectrum... 0%":
a short file recomputes faster than one timer tick and the repaint pump keyed
on a flag the clear never re-armed (the s16 gap, fixed then for six edit
sites only). The fix keys the pump on a per-clear generation instead.

Oracles read the pane's client pixels against thresholds taken from the
"before" capture of the same session (the colour profile cancels):
- gain: rows of the 100 Hz ridge at or above the pre-gain core level grow
  (the Hann main lobe's skirt crosses a fixed level further out at +12 dB);
- reverse: the tone lives in the first 2 s of 6 - the centroid of the lit
  columns moves from the left third to the right third.
RED on the control (85aada8): every "after" equals its "before".
"""
from __future__ import annotations

import itertools
import time
from pathlib import Path

import numpy as np
import soundfile as sf

from conftest import (SELECT_ITEM0, capture, clear_project, client_size, command_sync, dismiss_native_modal,
                      ensure_window, insert_item_unselected, perf_media_dir, send_command, wait_audio_loaded,
                      wait_main_thread_idle, window_title)

SHOTS = Path("/tmp/sneakpeak-reaproof-shots/spectral_edits")
CM_TOGGLE_SPECTRAL = 2028      # compiled from enum ContextMenuID (CM_UNDO = 2000), 2026-08-29
CM_REVERSE = 2012
CM_GAIN_UP = 2013              # +3 dB
CM_UNDO = 2000
CORE = 0.85
LIT = 0.5
SR = 44100
OPEN = ('reaper.defer(function() reaper.Main_OnCommand('
        'reaper.NamedCommandLookup("_SneakPeak_OpenStandalone"), 0) end) return true')
REVERSE = ('reaper.defer(function() reaper.Main_OnCommand('
           'reaper.NamedCommandLookup("_SneakPeak_Reverse"), 0) end) return true')
_N = itertools.count(1)


def _tone_fixture(tag: str, *, seconds_on: float, amp: float) -> Path:
    """6 s mono 24-bit: a 100 Hz tone at `amp` for the first `seconds_on` s,
    silence after. Unique name per call (a Standalone tab re-activates on the
    same path; the ITEM-mode reverse rewrites the file)."""
    media = perf_media_dir() / f"tone100_{tag}_{next(_N)}.wav"
    t = np.arange(6 * SR) / SR
    y = amp * np.sin(2 * np.pi * 100.0 * t)
    y[int(seconds_on * SR):] = 0.0
    sf.write(str(media), y.astype(np.float32), SR, subtype="PCM_24")
    return media


def _pane(s, out: Path) -> np.ndarray:
    """Luminance of the spectral pane (rows x columns), geometry as in
    test_spectral_fft: content 48..ch-86, waveform 55 %, 5 px splitter."""
    cap = capture(s, out)
    cw, ch = client_size(s)
    titlebar = cap.height - ch
    content_top, content_bot = 48, ch - 86
    wave_h = int((content_bot - content_top) * 0.55) - 2
    pane_top, pane_bot = content_top + wave_h + 5, content_bot
    band = cap.image[titlebar + pane_top:titlebar + pane_bot, 4:cw - 46, :3].astype(int)
    return band.max(axis=2)


def _ridge_rows_at(lum: np.ndarray, level: float) -> float:
    """Median rows per column at or above an ABSOLUTE luminance level."""
    return float(np.median((lum >= level).sum(axis=0)))


def _lit_centroid(lum: np.ndarray, level: float) -> float:
    """Centroid (0..1 of the pane width) of the columns carrying a pixel at or
    above `level`; 0.5 when nothing is lit."""
    cols = np.nonzero((lum >= level).any(axis=0))[0]
    return float(cols.mean() / lum.shape[1]) if cols.size else 0.5


def _wait_pane(s, tag: str, timeout: float = 20.0) -> np.ndarray:
    """The pane after its compute landed: a ridge in most columns. The frozen
    "Computing spectrum... 0%" overlay lights a few middle columns only."""
    lum = None
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        lum = _pane(s, SHOTS / f"{tag}.png")
        # columns carrying a ridge (>= 3 lit rows): the overlay's progress bar is
        # 1-2 rows, its text spans ~20 % of the width - a tone over a third of the
        # file lights a third of the columns with 9+ rows each
        ridge_cols = ((lum >= LIT * lum.max()).sum(axis=0) >= 3).mean() if lum.max() > 40 else 0.0
        if ridge_cols >= 0.25:
            return lum
        time.sleep(0.5)
    raise AssertionError(f"the spectrogram never painted ({tag}): the pane keeps the frozen overlay or is blank")


def _settle(s, tag: str, seconds: float = 3.0) -> np.ndarray:
    """Give a recompute time to land (a few seconds, like the repaint spec),
    then read the pane."""
    time.sleep(seconds)
    return _pane(s, SHOTS / f"{tag}.png")


def _open_standalone(s, media: Path):
    clear_project(s)
    ensure_window(s)
    s.eval(f'reaper.SetExtState("SneakPeak", "open_path", "{media.as_posix()}", false) return true')
    s.eval(OPEN)
    wait_audio_loaded(s, media.name, timeout=60)
    time.sleep(0.5)


def _spectral_on(s):
    """The pane toggle is POSTED (before any Send in this session - macOS lore)
    and the view pref sticks for the module's later tabs/items."""
    if not getattr(s, "_spectral_on", False):
        send_command(s, CM_TOGGLE_SPECTRAL)
        s._spectral_on = True


def test_standalone_gain_brightens_the_spectrogram(sess):
    SHOTS.mkdir(parents=True, exist_ok=True)
    media = _tone_fixture("gain", seconds_on=6.0, amp=0.05)          # -26 dBFS
    _open_standalone(sess, media)
    _spectral_on(sess)
    before = _wait_pane(sess, "gain_before")
    level = CORE * before.max()
    rows0 = _ridge_rows_at(before, level)
    assert rows0 >= 3, f"the spectrogram never painted after opening the pane ({rows0} rows)"

    try:
        for _ in range(4):                                             # +12 dB
            command_sync(sess, CM_GAIN_UP, settle=0.3)
        after = _settle(sess, "gain_after")
    finally:
        for _ in range(4):                                             # a dirty tab blocks REAPER's quit
            command_sync(sess, CM_UNDO, settle=0.2)
    rows1 = _ridge_rows_at(after, level)
    m = {"rows_before": rows0, "rows_after_at_the_same_level": rows1, "max_before": int(before.max()),
         "max_after": int(after.max())}
    print(f"\n[spectral_edits] gain +12 dB: {m}")
    assert rows1 >= rows0 + 3, f"the spectrogram did not follow the Standalone gain: {m}"


def test_standalone_reverse_mirrors_the_spectrogram(sess):
    media = _tone_fixture("rev_sa", seconds_on=2.0, amp=0.3)
    _open_standalone(sess, media)
    _spectral_on(sess)
    before = _wait_pane(sess, "rev_sa_before")
    level = LIT * before.max()
    c0 = _lit_centroid(before, level)
    assert c0 < 0.45, f"fixture: the tone should light the left third, centroid {c0:.2f}"

    try:
        command_sync(sess, CM_REVERSE, settle=0.3)
        after = _settle(sess, "rev_sa_after")
    finally:
        command_sync(sess, CM_UNDO, settle=0.2)                        # a dirty tab blocks REAPER's quit
    c1 = _lit_centroid(after, level)
    print(f"\n[spectral_edits] standalone reverse: centroid {c0:.2f} -> {c1:.2f}")
    assert c1 > 0.55, f"the spectrogram did not follow the Standalone reverse: centroid {c0:.2f} -> {c1:.2f}"


def test_item_mode_reverse_job_mirrors_the_spectrogram(sess):
    media = _tone_fixture("rev_item", seconds_on=2.0, amp=0.3)
    clear_project(sess)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=60)
    # LoadSelectedItem hides the pane (spectral is per item) - open it for this
    # item; a Send, since a Post no longer arrives after this session's Sends.
    command_sync(sess, CM_TOGGLE_SPECTRAL, settle=0.5)
    before = _wait_pane(sess, "rev_item_before")
    level = LIT * before.max()
    c0 = _lit_centroid(before, level)
    assert c0 < 0.45, f"fixture: the tone should light the left third, centroid {c0:.2f}"

    sess.eval(REVERSE)
    assert dismiss_native_modal(sess), "the destructive confirmation never appeared"
    sess.wait_until(lambda: "..." not in window_title(sess), timeout=120)   # F5: the job's title clears
    wait_main_thread_idle(sess, timeout=60)
    after = _settle(sess, "rev_item_after")
    c1 = _lit_centroid(after, level)
    print(f"\n[spectral_edits] item reverse (job): centroid {c0:.2f} -> {c1:.2f}")
    assert c1 > 0.55, f"the spectrogram did not follow the destructive reverse: centroid {c0:.2f} -> {c1:.2f}"


def test_opening_another_standalone_file_shows_its_spectrogram(sess):
    """The pane is already open (module state): a second Standalone open goes
    through FinishStandaloneLoad's ClearSpectrum - on the control the pane
    stays on the frozen overlay (no ridge at all)."""
    media = _tone_fixture("open2", seconds_on=2.0, amp=0.3)
    _open_standalone(sess, media)
    _spectral_on(sess)
    lum = _wait_pane(sess, "open2")
    c = _lit_centroid(lum, LIT * lum.max())
    print(f"\n[spectral_edits] second open: centroid {c:.2f}")
    assert c < 0.45, f"the pane shows something other than the new file's left-third tone: centroid {c:.2f}"


def test_switching_standalone_tabs_shows_that_tabs_spectrogram(sess):
    """RestoreStandaloneState (a click on the tab strip) clears the spectrum
    the same bare way. Tab 1 = the gain file (undone: clean): a tone over the whole 6 s."""
    from conftest import click_client
    assert "open2" in window_title(sess), f"module order: expected the open2 tab active, title {window_title(sess)!r}"
    click_client(sess, 150, 9)                                         # first tab of the strip
    sess.wait_until(lambda: "tone100_gain_" in window_title(sess), timeout=10)
    lum = _wait_pane(sess, "tab1")
    lit_cols = float((lum >= LIT * lum.max()).any(axis=0).mean())
    print(f"\n[spectral_edits] tab switch to the gain file: lit columns {lit_cols:.2f}")
    assert lit_cols > 0.8, f"the pane does not show the whole-file tone of tab 1: lit columns {lit_cols:.2f}"

