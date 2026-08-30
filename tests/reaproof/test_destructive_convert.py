"""Destructive edits on non-WAV items convert the source first ("Convert & go",
v2.5, 2026-08-29 s19), and the WAV formats are checked from the header.

A compressed source (MP3, FLAC, AAC, ...) has no samples to rewrite in place,
so Reverse / DC Remove / Gain-on-selection / the Hard Limiter on such an item
used to refuse ("WAV only - use Edit Copy"). Now they ask ONE question: the
source is decoded to a WAV next to it (lossless, in the background, Esc
cancels), every take that used the compressed file is pointed at the WAV
(Replace Source, one REAPER undo point), and the edit runs on the WAV with no
second prompt. The compressed original is never touched. WAV sources: 32-bit
integer PCM is edited in place like 16/24-bit, and a WAV the in-place editor
cannot rewrite (8-bit) is refused BEFORE the prompt, from the header.
Real-voice fixture (memory feedback_real_voice_fixtures): the limiter case runs
on a copy of the 22-minute 22 kHz mono MP3 (a lazy item).
Control eb4f703: MP3 items refuse with a toast (no prompt, no WAV); the limiter
panel greys Apply on them; a 32-bit int WAV passes the prompt and then fails in
WavInplace::Open ("Write failed - restored"); an 8-bit WAV prompts first.
"""
from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import time
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

from conftest import (SELECT_ITEM0, clear_project, db, dismiss_native_modal, ensure_window,
                      insert_item_unselected, locate_apply_button, perf_media_dir, send_command,
                      track_rms_windows, wait_audio_loaded, wait_destructive_job, wait_loaded,
                      wait_main_thread_idle, window_title)

# edit_view.h enum ContextMenuID (compiled 2026-08-29: CM_LAST 2264)
CM_SELECT_ALL, CM_REVERSE, CM_GAIN_UP, CM_DC_REMOVE, CM_APPLY_LIMITER = 2007, 2012, 2013, 2015, 2176
SHOTS = Path("/tmp/sneakpeak-reaproof-shots/destructive_convert")
# Real-voice corpus (not in the repo): SNEAKPEAK_CORPUS/<name>, else perf_media_dir()/corpus, else skip.
MP3 = Path(os.environ.get("SNEAKPEAK_CORPUS", "")) / "zachlebem_01_64kb.mp3"
SR = 44100


def _fire(cmd: int) -> str:
    return ('reaper.defer(function() reaper.JS_WindowMessage_Send(SP_WINDOW(), "WM_COMMAND", '
            f'{cmd}, 0, 0, 0) end) return true')


def _sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _last_toast(sess) -> str:
    return str(sess.eval('return reaper.GetExtState("SneakPeak", "last_toast")'))


def _source_name(sess) -> str:
    return str(sess.eval("""
      local it = reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), 0)
      local src = reaper.GetMediaItemTake_Source(reaper.GetActiveTake(it))
      return reaper.GetMediaSourceFileName(src)"""))


def _burst(seconds: float, channels: int) -> np.ndarray:
    """The burst_fixture signal: a quiet 220 Hz tone with one loud burst at 0.5-1.5 s."""
    t = np.arange(int(seconds * SR)) / SR
    y = 0.03 * np.sin(2 * np.pi * 220 * t)
    burst = (t >= 0.5) & (t < 1.5)
    y[burst] = 0.9 * np.sin(2 * np.pi * 220 * t[burst])
    return np.repeat(y[:, None], channels, axis=1)


def _burst_wav(name: str, subtype: str, seconds=30.0, channels=2) -> Path:
    out = perf_media_dir() / name
    out.unlink(missing_ok=True)
    tmp = out.with_suffix(".tmp.wav")
    sf.write(str(tmp), _burst(seconds, channels), SR, subtype=subtype)
    tmp.rename(out)
    return out


def _burst_mp3(name: str, seconds=30.0, channels=2) -> Path:
    if shutil.which("ffmpeg") is None:
        pytest.skip("ffmpeg not installed - the MP3 fixture cannot be encoded here")
    wav = _burst_wav(name + ".src.wav", "PCM_16", seconds, channels)
    out = perf_media_dir() / name
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", str(wav), "-c:a", "libmp3lame",
                    "-b:a", "192k", str(out)], check=True)
    wav.unlink(missing_ok=True)
    return out


def _corpus(original: Path) -> Path:
    """The corpus file: the dev Mac's volume, else a copy under perf_media_dir()/corpus
    (the VM legs), else the test is skipped."""
    if original.exists():
        return original
    alt = perf_media_dir() / "corpus" / original.name
    if alt.exists():
        return alt
    pytest.skip(f"real-voice corpus file missing: {original.name}")


def _remove_converted(media: Path):
    """The WAVs a previous Convert & go left next to a compressed fixture."""
    if media.suffix.lower() == ".wav":
        return
    for p in media.parent.glob(f"{media.stem}*.wav"):
        if p.stem == media.stem or p.stem.startswith(media.stem + "_"):
            p.unlink()


def _load(sess, media: Path, *, lazy=False, trim_s: float | None = None):
    clear_project(sess)
    _remove_converted(media)
    insert_item_unselected(sess, media)
    if trim_s is not None:
        sess.eval("local it = reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), 0) "
                  f"reaper.SetMediaItemInfo_Value(it, 'D_LENGTH', {trim_s}) reaper.UpdateArrange() return true")
    ensure_window(sess)
    sess.eval(SELECT_ITEM0, hang_timeout=180)
    if lazy:
        wait_loaded(sess, media.stem, timeout=120)
    else:
        wait_audio_loaded(sess, media.stem, timeout=120)
    time.sleep(0.5)
    sess.eval('reaper.DeleteExtState("SneakPeak", "last_toast", false)')


def _convert_state(sess) -> str:
    return str(sess.eval('return reaper.GetExtState("SneakPeak", "convert_state")'))


def _wait_convert_and_edit(sess, wav: Path, timeout=600):
    """The conversion pump ("Converting to WAV... N%") writes the WAV, the source
    swap follows (the item plays the WAV a moment later on Windows) and the
    edit's own job runs from the pending phase: wait for the file, for the
    take to play it, then for the job's title to show and clear (F5 lore)."""
    sess.wait_until(lambda: wav.exists(), timeout=timeout)
    try:
        sess.wait_until(lambda: _source_name(sess).endswith(wav.name), timeout=60)
    except Exception:
        raise AssertionError(f"the item does not play the WAV after the conversion (source {_source_name(sess)!r}, "
                             f"convert_state {_convert_state(sess)!r}, toast {_last_toast(sess)!r})")
    _wait_edit_job(sess, timeout=timeout)


JOB_TITLES = ("saving the pre-edit copy", "Reversing", "Removing DC", "Applying gain", "Limiting")


def _wait_edit_job(sess, timeout=600):
    """The edit's job by ITS titles, not by any "...": with the panel open the
    pump reloads the long WAV's buffer afterwards ("Loading... N%"), minutes on
    the VM (leg 7 timed out on that, the edit long done)."""
    def job_running():
        t = window_title(sess)
        return any(m in t for m in JOB_TITLES)
    try:
        sess.wait_until(job_running, timeout=8)
    except Exception:
        pass   # finished within one round trip
    try:
        sess.wait_until(lambda: not job_running(), timeout=timeout)
    except Exception:
        raise AssertionError(f"the edit job did not finish in {timeout}s: title {window_title(sess)!r}, "
                             f"convert_state {_convert_state(sess)!r}, toast {_last_toast(sess)!r}")
    time.sleep(1.5)


# --- non-WAV items convert first -----------------------------------------------------

def test_reverse_on_an_mp3_item_converts_the_source_and_reverses_the_wav(sess):
    """A 30 s burst MP3 (burst at 0.5-1.5 s). Reverse: one prompt (Yes), the MP3
    is decoded to <name>.wav next to it, the item plays the WAV, the WAV is
    reversed (the burst at the tail), the MP3's bytes are untouched. Control:
    'Cannot rewrite the file: the source is MP3' toast, no prompt, no WAV."""
    mp3 = _burst_mp3("convert_burst_30s.mp3")
    wav = mp3.with_suffix(".wav")
    _load(sess, mp3)
    sha0 = _sha(mp3)
    w_head, w_tail = (0.6, 1.4), (28.6, 29.4)
    head, tail = track_rms_windows(sess, [w_head, w_tail])
    assert db(head) > db(tail) + 20, f"precondition: burst not at the head ({db(head):.1f} / {db(tail):.1f})"

    sess.eval(_fire(CM_REVERSE))
    confirmed = dismiss_native_modal(sess, timeout=6)
    toast_early = _last_toast(sess)
    assert confirmed, f"no conversion prompt - the MP3 item was refused ({toast_early!r})"
    _wait_convert_and_edit(sess, wav)

    assert wav.exists(), "the converted WAV was not written next to the MP3"
    assert _source_name(sess).endswith(wav.name), f"the item still plays {_source_name(sess)}"
    assert _sha(mp3) == sha0, "the MP3's bytes changed"
    info = sf.info(str(wav))
    assert info.samplerate == SR and info.channels == 2, f"converted WAV format: {info}"
    head, tail = track_rms_windows(sess, [w_head, w_tail])
    assert db(tail) > db(head) + 20, f"the item was not reversed after the conversion ({db(head):.1f} / {db(tail):.1f})"
    assert not dismiss_native_modal(sess, timeout=2), "a second dialog appeared after the conversion"


def test_limiter_on_an_mp3_item_converts_then_limits(sess):
    """A 60 s item of the real 22-minute MP3 (lazy). The panel opens with Apply
    enabled and the footer saying it converts first; Apply -> one prompt -> the
    whole MP3 is decoded to a WAV, the item points at it, and the item's window
    of the WAV is limited (peak under the -1 dBTP ceiling, +6 dB gain in).
    Control: Apply greyed ('the source is MP3 - WAV only'), no amber button."""
    mp3 = perf_media_dir() / "convert_zachlebem.mp3"
    shutil.copy(_corpus(MP3), mp3)
    wav = mp3.with_suffix(".wav")
    _load(sess, mp3, lazy=True, trim_s=60.0)
    sha0 = _sha(mp3)
    sess.eval('reaper.DeleteExtState("SneakPeak", "lim_apply_status", false) '
              'reaper.DeleteExtState("SneakPeak", "lim_footer_note", false) '
              'reaper.SetExtState("SneakPeak", "lim_gain", "6000", true) return true')
    SHOTS.mkdir(parents=True, exist_ok=True)
    try:
        send_command(sess, CM_APPLY_LIMITER)
        try:
            sess.wait_until(lambda: locate_apply_button(sess, SHOTS / "mp3_1_panel.png") is not None,
                            timeout=10)
        except Exception:
            status = str(sess.eval('return reaper.GetExtState("SneakPeak", "lim_apply_status")'))
            raise AssertionError(f"Apply is not enabled on the MP3 item (status {status!r}, toast {_last_toast(sess)!r})")
        note = str(sess.eval('return reaper.GetExtState("SneakPeak", "lim_footer_note")'))
        assert "CONVERT" in note.upper() and "MP3" in note.upper(), f"footer note: {note!r}"
        x, y = locate_apply_button(sess, SHOTS / "mp3_1_panel.png")
        sess.eval(f"""reaper.defer(function()
            local h = SP_WINDOW()
            reaper.JS_WindowMessage_Send(h, "WM_LBUTTONDOWN", 1, 0, {x}, {y})
            reaper.JS_WindowMessage_Send(h, "WM_LBUTTONUP", 0, 0, {x}, {y})
          end) return true""")
        confirmed = dismiss_native_modal(sess, timeout=6)
        assert confirmed, f"no conversion prompt after Apply ({_last_toast(sess)!r})"
        _wait_convert_and_edit(sess, wav, timeout=900)
    finally:
        sess.eval('reaper.SetExtState("SneakPeak", "lim_gain", "0", true) return true')

    toast = _last_toast(sess)
    assert wav.exists(), f"no converted WAV (toast {toast!r})"
    assert _source_name(sess).endswith(wav.name), f"the item still plays {_source_name(sess)}"
    assert _sha(mp3) == sha0, "the MP3's bytes changed"
    assert toast.startswith("Limited"), f"result toast missing: {toast!r} (convert_state {_convert_state(sess)!r})"
    with sf.SoundFile(str(wav)) as f:
        f.seek(int(0.1 * f.samplerate))
        y = f.read(int(59.8 * f.samplerate), dtype="float64", always_2d=True)
    pk = float(20 * np.log10(max(np.abs(y).max(), 1e-9)))
    assert pk <= -0.95, f"the item's window of the WAV still peaks at {pk:.2f} dBFS"


# --- WAV depths: 32-bit int works in place, 8-bit is refused from the header ----------

def test_reverse_on_a_32bit_pcm_wav_item_edits_in_place(sess):
    media = _burst_wav("convert_pcm32_30s.wav", "PCM_32")
    _load(sess, media)
    assert sf.info(str(media)).subtype == "PCM_32"
    w_head, w_tail = (0.6, 1.4), (28.6, 29.4)

    sess.eval(_fire(CM_REVERSE))
    assert dismiss_native_modal(sess, timeout=6), "the Reverse confirmation never appeared"
    wait_destructive_job(sess, timeout=240)

    toast = _last_toast(sess)
    assert "failed" not in toast.lower() and "restored" not in toast.lower(), f"the 32-bit write failed: {toast!r}"
    assert sf.info(str(media)).subtype == "PCM_32", "the bit depth changed"
    head, tail = track_rms_windows(sess, [w_head, w_tail])
    assert db(tail) > db(head) + 20, f"the 32-bit item was not reversed ({db(head):.1f} / {db(tail):.1f})"


def test_reverse_on_an_8bit_wav_item_is_refused_before_the_prompt(sess):
    media = _burst_wav("convert_pcm8_30s.wav", "PCM_U8")
    _load(sess, media)
    sha0 = _sha(media)

    sess.eval(_fire(CM_REVERSE))
    modal = dismiss_native_modal(sess, timeout=4)
    try:
        assert not modal, "a dialog appeared - an 8-bit WAV must be refused from the header, before the prompt"
        toast = _last_toast(sess)
        assert "8-bit" in toast, f"refusal toast missing: {toast!r}"
        assert _sha(media) == sha0, "the 8-bit file's bytes changed"
    finally:
        dismiss_native_modal(sess, timeout=3)
        wait_main_thread_idle(sess, timeout=60)
