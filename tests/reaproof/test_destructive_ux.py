"""Destructive ITEM edits ask first and refuse first (UX audit 2026-08-29, F1-F3).

F1 - Gain on a selection rewrites the item's source file through the same
in-place job as Reverse and DC Remove, but was the only destructive command
without a confirmation. F2 - every precondition (WAV source, no section or
reversed take, no job running) is evaluated BEFORE the prompt and reported on
the control: the user never answers Yes and gets refused by a second box.
F3 - the Hard Limiter on an item limits the item's window of the file in place
(trimmed items, downsampled buffers), like Reverse/Gain/DC, instead of
demanding the whole file.
Real-voice fixtures (memory feedback_real_voice_fixtures): the refusals run on
a copy of the 22-minute 22 kHz mono MP3 (a lazy item: no buffer at all), the
limiter on a copy of the 1.9-minute 48 kHz voice WAV.
Control 2328139: Gain writes without asking; Reverse/DC prompt Yes/No and then
pop the "WAV only" box; Gain on the MP3 pops the box at once; the limiter on a
trimmed WAV refuses ("needs the whole file"), on a 5-minute item refuses
("too long"), and its panel never opens on the lazy MP3.
"""
from __future__ import annotations

import hashlib
import itertools
import shutil
import time
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

from conftest import (SELECT_ITEM0, burst_fixture, capture, clear_project, dismiss_native_modal,
                      ensure_window, insert_item_unselected, locate_apply_button, perf_media_dir,
                      send_command, wait_audio_loaded, wait_loaded, wait_main_thread_idle,
                      window_title)

# edit_view.h enum ContextMenuID (compiled 2026-08-29: CM_LAST 2264)
CM_SELECT_ALL, CM_REVERSE, CM_GAIN_UP, CM_DC_REMOVE, CM_APPLY_LIMITER = 2007, 2012, 2013, 2015, 2176
SHOTS = Path("/tmp/sneakpeak-reaproof-shots/destructive_ux")
MP3 = Path("/Volumes/@Basic/PRODUKCJA/SONDA_corpus/mined_PL_2026-07/zachlebem_01_64kb.mp3")
WAV = Path("/Volumes/@Basic/PRODUKCJA/SONDA_corpus/qa_PL_2026-07/Sesja-pyt-rezysera/Media/01-260728_0159.wav")


def _fire(cmd: int) -> str:
    """A menu command SENT from inside the defer loop: a native confirmation
    then blocks that loop, which is the heartbeat stall dismiss_native_modal
    keys on (a posted command is pumped by our window, where the modal's
    nested run loop keeps REAPER's timers ticking)."""
    return ('reaper.defer(function() reaper.JS_WindowMessage_Send(SP_WINDOW(), "WM_COMMAND", '
            f'{cmd}, 0, 0, 0) end) return true')


def _sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _last_toast(sess) -> str:
    return str(sess.eval('return reaper.GetExtState("SneakPeak", "last_toast")'))


def _clear_toast(sess):
    sess.eval('reaper.DeleteExtState("SneakPeak", "last_toast", false)')


def _wait_job(sess, timeout=240):
    """F5: the write runs on a worker - wait for the job's title to clear,
    never for an idle main thread (idle at once since F5)."""
    sess.wait_until(lambda: "..." not in window_title(sess), timeout=timeout)
    wait_main_thread_idle(sess, timeout=timeout)
    time.sleep(1.0)


def _load(sess, media: Path, *, lazy=False, trim_s: float | None = None):
    clear_project(sess)
    insert_item_unselected(sess, media)
    if trim_s is not None:
        sess.eval("local it = reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), 0) "
                  f"reaper.SetMediaItemInfo_Value(it, 'D_LENGTH', {trim_s}) reaper.UpdateArrange() return true")
    ensure_window(sess)
    sess.eval(SELECT_ITEM0, hang_timeout=180)
    if lazy:
        wait_loaded(sess, media.stem, timeout=120)   # a lazy item never loads a buffer
    else:
        wait_audio_loaded(sess, media.stem, timeout=120)
    time.sleep(0.5)
    _clear_toast(sess)


_N = itertools.count(1)


def _corpus(original: Path) -> Path:
    """The corpus file: the dev Mac's volume, else a copy under
    perf_media_dir()/corpus (the VM legs), else the test is skipped."""
    if original.exists():
        return original
    alt = perf_media_dir() / "corpus" / original.name
    if alt.exists():
        return alt
    pytest.skip(f"real-voice corpus file missing: {original.name}")


def _real_mp3() -> Path:
    """A fresh copy per call: Windows keeps the previous test's copy open in
    REAPER's decoder pool, and copying over it is a sharing violation."""
    mp3 = perf_media_dir() / f"ux_real_zachlebem_64kb_{next(_N)}.mp3"
    shutil.copy(_corpus(MP3), mp3)
    return mp3


def _real_wav_hot(name: str) -> Path:
    """The voice WAV normalized to -0.05 dBFS so a -1 dBTP ceiling has work to
    do on every phrase (24-bit, as the original)."""
    out = perf_media_dir() / name
    y, sr = sf.read(str(_corpus(WAV)), dtype="float64", always_2d=True)
    y = y / np.abs(y).max() * 10 ** (-0.05 / 20)
    sf.write(str(out), y, sr, subtype="PCM_24")
    return out


# --- F1: Gain on a selection asks ------------------------------------------------

def test_gain_on_a_selection_asks_before_rewriting_the_file(sess):
    media = burst_fixture("ux_gain_sel_30s.wav", seconds=30, channels=2)
    _load(sess, media)
    sha0 = _sha(media)
    send_command(sess, CM_SELECT_ALL)
    time.sleep(0.3)

    sess.eval(_fire(CM_GAIN_UP))
    confirmed = dismiss_native_modal(sess, timeout=6)
    _wait_job(sess)

    assert confirmed, "Gain on a selection rewrote the file without asking (Reverse and DC Remove ask)"
    assert _sha(media) != sha0, "after Yes the selection must be gained in the file"


# --- F2: refusals come before any prompt -----------------------------------------

@pytest.mark.parametrize("name, cmd, select", [
    ("Reverse", CM_REVERSE, False),
    ("DC Offset Remove", CM_DC_REMOVE, False),
    ("Gain on selection", CM_GAIN_UP, True),
])
def test_destructive_edit_on_an_mp3_item_is_refused_before_any_prompt(sess, name, cmd, select):
    """The 22-minute MP3 (lazy: no buffer). Control: Reverse/DC ask Yes/No and
    THEN pop the 'WAV only' box; Gain pops the box at once. Fixed: one toast
    naming the format and the way out (Edit Copy), no dialog, bytes untouched."""
    mp3 = _real_mp3()
    _load(sess, mp3, lazy=True)
    sha0 = _sha(mp3)
    if select:
        send_command(sess, CM_SELECT_ALL)
        time.sleep(0.3)

    sess.eval(_fire(cmd))
    modal = dismiss_native_modal(sess, timeout=4)
    try:
        assert not modal, f"{name}: a dialog appeared - the refusal must come first, on the control"
        toast = _last_toast(sess)
        assert "MP3" in toast and "Edit Copy" in toast, f"{name}: refusal toast missing: {toast!r}"
        assert _sha(mp3) == sha0, f"{name}: the MP3's bytes changed"
    finally:
        dismiss_native_modal(sess, timeout=3)   # a control's second box would block the next test
        wait_main_thread_idle(sess, timeout=60)


# --- F3: the Hard Limiter works on the item's window -------------------------------

def _press_apply(sess, tag: str) -> bool:
    """Open the HARD LIMITER panel and press its Apply (the real user path);
    returns whether the destructive confirmation appeared (answered Yes)."""
    SHOTS.mkdir(parents=True, exist_ok=True)
    send_command(sess, CM_APPLY_LIMITER)
    try:
        sess.wait_until(lambda: locate_apply_button(sess, SHOTS / f"{tag}_1_panel.png") is not None,
                        timeout=10)
    except Exception:
        raise AssertionError(f"the Hard Limiter panel did not open (toast {_last_toast(sess)!r})")
    x, y = locate_apply_button(sess, SHOTS / f"{tag}_1_panel.png")
    sess.eval(f"""reaper.defer(function()
        local h = SP_WINDOW()
        reaper.JS_WindowMessage_Send(h, "WM_LBUTTONDOWN", 1, 0, {x}, {y})
        reaper.JS_WindowMessage_Send(h, "WM_LBUTTONUP", 0, 0, {x}, {y})
      end) return true""")
    time.sleep(0.4)
    capture(sess, SHOTS / f"{tag}_2_pressed.png")
    confirmed = dismiss_native_modal(sess, timeout=6)
    _wait_job(sess, timeout=600)
    capture(sess, SHOTS / f"{tag}_3_after.png")
    return confirmed


def _peak_db(path: Path, t0: float, t1: float) -> float:
    with sf.SoundFile(str(path)) as f:
        f.seek(int(t0 * f.samplerate))
        y = f.read(int((t1 - t0) * f.samplerate), dtype="float64", always_2d=True)
    return float(20 * np.log10(max(np.abs(y).max(), 1e-9)))


def _samples(path: Path, t0: float, t1: float) -> np.ndarray:
    with sf.SoundFile(str(path)) as f:
        f.seek(int(t0 * f.samplerate))
        return f.read(int((t1 - t0) * f.samplerate), dtype="float64", always_2d=True)


def test_limiter_on_a_trimmed_item_limits_only_its_window_of_the_file(sess):
    """A 30 s item of the 1.9-minute voice WAV, Gain +6 dB into -1 dBTP:
    inside the window (past the 20 ms handoff ramps) every sample sits at or
    under the ceiling, the rest of the file is byte-identical. Control:
    'Hard Limiter needs the whole file' toast, file unchanged."""
    media = _real_wav_hot("ux_real_voice_trim.wav")
    _load(sess, media, trim_s=30.0)
    sha0 = _sha(media)
    tail0 = _samples(media, 30.5, 60.0)
    pk0 = _peak_db(media, 0.1, 29.9)
    assert pk0 > -6.5, f"precondition: +6 dB must push the window ({pk0:.2f} dBFS) over -1 dBTP"
    sess.eval('reaper.SetExtState("SneakPeak", "lim_gain", "6000", true) return true')   # milli-dB
    try:
        confirmed = _press_apply(sess, "trim")
    finally:
        sess.eval('reaper.SetExtState("SneakPeak", "lim_gain", "0", true) return true')

    toast = _last_toast(sess)
    assert confirmed, f"the limiter did not reach its confirmation (toast {toast!r})"
    assert _sha(media) != sha0, f"the file was not rewritten (toast {toast!r})"
    assert toast.startswith("Limited"), f"result toast missing: {toast!r}"
    pk1 = _peak_db(media, 0.1, 29.9)
    assert pk1 <= -0.95, f"the item's window still peaks at {pk1:.2f} dBFS (ceiling -1 dBTP)"
    tail1 = _samples(media, 30.5, 60.0)
    assert np.array_equal(tail0, tail1), "audio outside the item's window changed"


def test_limiter_on_a_downsampled_item_limits_the_file_at_its_own_rate(sess):
    """A 5-minute stereo item (over the 10M-frame cap: lazy, and downsampled
    once a panel asks for its buffer): the limiter must edit the file in place
    at 44.1 kHz. Control: the panel never opens on the lazy item (a silent
    no-op); with a buffer it refused 'Item too long for the Hard Limiter'."""
    media = burst_fixture("ux_long5min_burst24.wav", seconds=300, channels=2)
    _load(sess, media)
    sha0 = _sha(media)
    info0 = sf.info(str(media))
    pk0 = _peak_db(media, 0.4, 1.6)
    assert pk0 > -1.5, f"precondition: the burst peaks at {pk0:.2f} dBFS"
    sess.eval('reaper.SetExtState("SneakPeak", "lim_gain", "6000", true) return true')
    try:
        confirmed = _press_apply(sess, "long")
    finally:
        sess.eval('reaper.SetExtState("SneakPeak", "lim_gain", "0", true) return true')

    toast = _last_toast(sess)
    assert confirmed, f"the limiter did not reach its confirmation (toast {toast!r})"
    assert _sha(media) != sha0, f"the file was not rewritten (toast {toast!r})"
    info1 = sf.info(str(media))
    assert (info1.samplerate, info1.channels, info1.subtype, info1.frames) == \
           (info0.samplerate, info0.channels, info0.subtype, info0.frames), f"format changed: {info0} -> {info1}"
    pk1 = _peak_db(media, 0.4, 1.6)
    assert pk1 <= -0.95, f"the burst still peaks at {pk1:.2f} dBFS (ceiling -1 dBTP)"


def test_limiter_panel_on_an_mp3_item_shows_the_reason_instead_of_a_box(sess):
    """A 60 s item of the MP3 (lazy). Control: the panel never opens (a silent
    no-op) - or, once a buffer exists, Apply pops the 'WAV only' box. Fixed:
    the panel opens, its Apply is greyed with the reason in the footer (the
    footer status is mirrored in ExtState SneakPeak/lim_apply_status), and the
    press yields the same reason as a toast - no dialog, bytes untouched."""
    mp3 = _real_mp3()
    _load(sess, mp3, lazy=True, trim_s=60.0)
    sha0 = _sha(mp3)
    sess.eval('reaper.DeleteExtState("SneakPeak", "lim_apply_status", false)')
    SHOTS.mkdir(parents=True, exist_ok=True)

    send_command(sess, CM_APPLY_LIMITER)
    status = lambda: str(sess.eval('return reaper.GetExtState("SneakPeak", "lim_apply_status")'))
    try:
        sess.wait_until(lambda: "MP3" in status(), timeout=10)
    except Exception:
        toast = _last_toast(sess)
        raise AssertionError(f"the panel did not open with the reason (status {status()!r}, toast {toast!r})")
    time.sleep(0.5)
    assert locate_apply_button(sess, SHOTS / "mp3_1_panel.png") is None, "Apply is still lit amber"
    assert "Edit Copy" in status(), status()
    modal = dismiss_native_modal(sess, timeout=3)
    assert not modal, "a dialog appeared"
    assert _sha(mp3) == sha0, "the MP3's bytes changed"
