"""Standalone safety (v2.5.0 audit, increment A4).

Files enter Standalone through the new scriptable action
`_SneakPeak_OpenStandalone` (path in ExtState SneakPeak/open_path) - the
drag-drop route is not scriptable. A4.1 - a >2-channel file was silently
loaded as stereo and Save wrote it back with channels 3+ gone. A4.2 - a
file over the 1 GB buffer cap was allocated anyway (or overflowed the frame
math). A4.4 - Paste in Standalone took no undo snapshot and touched a null
item. A4.5 - One-Shot LUFS-I on a PCM Standalone file clipped instead of
limiting. A4.6 - One-Shot REGIONS mode in Standalone sliced by a project the
file is not in. Control (cb48cd5): loaded as stereo / loads or crashes /
undo does nothing / clipped run / regions run.
"""
from __future__ import annotations

import time
from pathlib import Path

import numpy as np
import soundfile as sf

from conftest import (burst_fixture, clear_project, ensure_window, mode_from_capture,
                      perf_media_dir, rss_mb, send_command, wait_audio_loaded,
                      wait_main_thread_idle, write_long_wav)

CM_UNDO, CM_PASTE, CM_COPY, CM_SELECT_ALL = 2000, 2003, 2002, 2007   # edit_view.h ContextMenuID
SHOTS = Path("/tmp/sneakpeak-reaproof-shots/standalone_guards")

OPEN = ('reaper.defer(function() reaper.Main_OnCommand('
        'reaper.NamedCommandLookup("_SneakPeak_OpenStandalone"), 0) end) return true')


def _last_toast(sess) -> str:
    return str(sess.eval('return reaper.GetExtState("SneakPeak", "last_toast")'))


def _send_sync(sess, msg: str, wparam: int, lo: int = 0, hi: int = 0, settle: float = 0.5):
    """JS_WindowMessage_Send from inside the defer loop. Once a Send has been
    used on our window, POSTED messages (send_command / press_key) are no
    longer delivered to it on macOS (measured 2026-08-29), so every command
    and key in these specs goes this way; a modal it raises blocks the defer
    loop, which is exactly the stall dismiss_native_modal keys on."""
    sess.eval(f'reaper.defer(function() reaper.JS_WindowMessage_Send(SP_WINDOW(), "{msg}", '
              f'{wparam}, 0, {lo}, {hi}) end) return true')
    time.sleep(settle)


def _command_sync(sess, cmd: int, settle: float = 0.5):
    _send_sync(sess, "WM_COMMAND", cmd, settle=settle)


def _key_sync(sess, vk: int, settle: float = 0.5):
    _send_sync(sess, "WM_KEYDOWN", vk, settle=settle)


def _open_standalone(sess, path: Path):
    sess.eval('reaper.DeleteExtState("SneakPeak", "last_toast", false)')
    sess.eval(f'reaper.SetExtState("SneakPeak", "open_path", "{path.as_posix()}", false) return true')
    sess.eval(OPEN)
    time.sleep(1.0)


def _mode(sess) -> str:
    SHOTS.mkdir(parents=True, exist_ok=True)
    return mode_from_capture(sess, SHOTS / "mode.png")


def test_standalone_refuses_a_multichannel_file(sess):
    clear_project(sess)
    ensure_window(sess)
    media = burst_fixture("sa_6ch_10s.wav", seconds=10, channels=6)
    _open_standalone(sess, media)
    time.sleep(1.5)
    mode = _mode(sess)
    toast = _last_toast(sess)
    assert mode != "STANDALONE", f"a 6-channel file was loaded into Standalone (mode {mode})"
    assert "mono and stereo" in toast and "6 channels" in toast, f"refusal toast missing: {toast!r}"
    assert sf.info(str(media)).channels == 6


def test_standalone_refuses_a_file_over_the_buffer_cap(sess):
    """65 min mono 16-bit at 44.1k = 340 MB on disk, 1.37 GB of doubles."""
    media = write_long_wav(perf_media_dir() / "sa_long65min_mono.wav", minutes=65, channels=1)
    clear_project(sess)
    ensure_window(sess)
    rss0 = rss_mb(sess)
    _open_standalone(sess, media)
    time.sleep(3.0)
    wait_main_thread_idle(sess, timeout=60)
    mode = _mode(sess)
    delta = rss_mb(sess) - rss0
    toast = _last_toast(sess)
    print(f"\n[standalone] over-cap: mode {mode}, rss delta {delta:.0f} MB, toast {toast!r}")
    assert mode != "STANDALONE", "an over-cap file was loaded into Standalone"
    assert delta < 50, f"the over-cap file allocated its buffer anyway (+{delta:.0f} MB)"
    assert "min max" in toast, f"cap toast missing: {toast!r}"


# --- A4.4: Standalone edits keep their own undo ------------------------------
VK_SPACE, VK_ESCAPE = 0x20, 0x1B
REVERSE = ('reaper.defer(function() reaper.Main_OnCommand('
           'reaper.NamedCommandLookup("_SneakPeak_Reverse"), 0) end) return true')
PASTE = ('reaper.defer(function() reaper.JS_WindowMessage_Send(SP_WINDOW(), "WM_COMMAND", '
         f'{CM_PASTE}, 0, 0, 0) end) return true')


def _preview_of_whole_buffer(sess) -> np.ndarray:
    """Standalone Play writes the selection (or the tail from the cursor) as a
    temp WAV before playing it - select all first, so the file IS the buffer."""
    import tempfile
    preview = Path(tempfile.gettempdir()) / f"sneakpeak_preview_{sess.handle.pid}.wav"
    preview.unlink(missing_ok=True)
    _command_sync(sess, CM_SELECT_ALL, settle=0.3)
    _key_sync(sess, VK_SPACE, settle=0.2)
    try:
        sess.wait_until(preview.exists, timeout=10)
    except Exception:
        from conftest import window_title
        from conftest import capture
        capture(sess, SHOTS / "preview_fail.png")
        print(f"\n[preview] no file: title {window_title(sess)!r} toast {_last_toast(sess)!r} "
              f"playstate {sess.eval('return reaper.GetPlayState()')} mode {_mode(sess)}")
        raise AssertionError("Standalone Play wrote no preview after the edit - the edit left "
                             "Standalone unable to play its own buffer")
    time.sleep(0.5)
    _key_sync(sess, VK_SPACE, settle=0.3)     # stop
    got = sf.read(str(preview), dtype="float64", always_2d=True)[0]
    _key_sync(sess, VK_ESCAPE, settle=0.2)    # clear the selection again
    return got


def _open_and_wait(sess, media: Path):
    clear_project(sess)
    ensure_window(sess)
    _open_standalone(sess, media)
    wait_audio_loaded(sess, media.name, timeout=60)
    time.sleep(0.5)
    assert _mode(sess) == "STANDALONE", "the fixture did not open in Standalone"


def test_standalone_reverse_undo_restores_the_buffer(sess):
    """Reverse in Standalone went down the ITEM path: a 'modifies the file on
    disk' prompt, no Standalone undo entry (Ctrl+Z did nothing) and no dirty
    flag. Control (cb48cd5): prompt appears, undo leaves the buffer reversed."""
    from conftest import dismiss_native_modal
    media = burst_fixture("sa_reverse_10s.wav", seconds=10, channels=2)
    want = sf.read(str(media), dtype="float64", always_2d=True)[0]
    _open_and_wait(sess, media)

    sess.eval(REVERSE)
    prompted = dismiss_native_modal(sess, timeout=5)
    wait_main_thread_idle(sess, timeout=60)
    rev = _preview_of_whole_buffer(sess)
    assert np.abs(rev[:4410] - want[::-1][:4410]).max() < 1e-4, "precondition: the buffer was not reversed"
    _command_sync(sess, CM_UNDO)
    wait_main_thread_idle(sess, timeout=60)
    time.sleep(0.5)

    got = _preview_of_whole_buffer(sess)
    assert not prompted, "Standalone Reverse asked about a file on disk (ITEM path)"
    assert len(got) == len(want), f"undo left {len(got)} frames, expected {len(want)}"
    assert np.abs(got - want).max() < 1e-4, "undo did not restore the pre-reverse buffer"


def test_standalone_paste_undo_restores_the_buffer(sess):
    """Paste in Standalone took the ITEM undo path (no Standalone snapshot) and
    touched a null item. Control (cb48cd5): after Ctrl+Z the buffer still
    holds the pasted second."""
    from conftest import WAVE_Y, click_client, dismiss_native_modal, drag_client
    media = burst_fixture("sa_paste_10s.wav", seconds=10, channels=2)
    want = sf.read(str(media), dtype="float64", always_2d=True)[0]
    _open_and_wait(sess, media)

    drag_client(sess, 200, WAVE_Y, 275, WAVE_Y)     # ~1 s of a 10 s file
    time.sleep(0.3)
    send_command(sess, CM_COPY)
    time.sleep(0.5)
    click_client(sess, 500, WAVE_Y)                  # cursor
    time.sleep(0.3)
    sess.eval(PASTE)
    prompted = dismiss_native_modal(sess, timeout=5)
    wait_main_thread_idle(sess, timeout=60)
    pasted = _preview_of_whole_buffer(sess)
    assert len(pasted) > len(want) + 4000, f"precondition: paste did not lengthen the buffer ({len(pasted)} vs {len(want)})"
    _command_sync(sess, CM_UNDO)
    wait_main_thread_idle(sess, timeout=60)
    time.sleep(0.5)

    got = _preview_of_whole_buffer(sess)
    assert not prompted, "Standalone Paste asked about a file on disk (ITEM path)"
    assert len(got) == len(want), f"undo left {len(got)} frames, expected {len(want)}"
    assert np.abs(got - want).max() < 1e-4, "undo did not restore the pre-paste buffer"


# --- A4.5 / A4.6: One-Shot Factory in Standalone -------------------------------
CM_ONESHOT_FACTORY = 2256


def _run_one_shot(sess, settings: dict):
    from conftest import locate_apply_button   # Dynamics stays closed: Run is the only amber blob
    # RestoreOneShotParams only reads the os_* keys when os_trim_thr exists
    # ("first run: keep the plan defaults"): always seed the whole set.
    full = {"os_trim_thr": "-60000", "os_pad": "0", **settings}
    lua = " ".join(f'reaper.SetExtState("SneakPeak", "{k}", "{v}", false)' for k, v in full.items())
    sess.eval(lua + " return true")
    SHOTS.mkdir(parents=True, exist_ok=True)
    sess.eval('reaper.DeleteExtState("SneakPeak", "last_toast", false)')
    _command_sync(sess, CM_ONESHOT_FACTORY, settle=1.0)
    try:
        sess.wait_until(lambda: locate_apply_button(sess, SHOTS / "oneshot_panel.png") is not None, timeout=10)
    except Exception:
        from conftest import window_title
        print(f"\n[oneshot] panel not found: title {window_title(sess)!r} toast {_last_toast(sess)!r} mode {_mode(sess)}")
        raise
    sess.eval('reaper.DeleteExtState("SneakPeak", "last_toast", false)')
    x, y = locate_apply_button(sess, SHOTS / "oneshot_panel.png")
    _send_sync(sess, "WM_LBUTTONDOWN", 1, x, y, settle=0.1)
    _send_sync(sess, "WM_LBUTTONUP", 0, x, y, settle=0.1)
    wait_main_thread_idle(sess, timeout=120)
    time.sleep(1.0)
    _command_sync(sess, CM_ONESHOT_FACTORY, settle=0.3)   # close the panel again


def test_one_shot_lufs_on_a_pcm_standalone_file_does_not_clip(sess):
    """LUFS-I -16 on a quiet 16-bit file needs ~+17 dB; a 0.9 click then
    lands far above full scale and the PCM writer clipped it flat. The gain
    now goes through the true-peak limiter when it would clip. Control
    (0a444ad = cb48cd5 + the open action): a run of full-scale samples."""
    # Quiet 220 Hz tone (-30 dBFS, the loudness) with one 2 ms click at 0.9
    # (the peak): LUFS-I -16 asks for about +17 dB, the click then wants 6x
    # full scale. (A burst of a second would dominate the gated loudness and
    # need no gain at all.)
    media = perf_media_dir() / "sa_lufs16_10s.wav"
    sr = 44100
    t = np.arange(10 * sr) / sr
    y = 0.03 * np.sin(2 * np.pi * 220 * t)
    y[5 * sr:5 * sr + 88] = 0.9 * np.sign(np.sin(2 * np.pi * 220 * t[5 * sr:5 * sr + 88]) + 1e-9)
    sf.write(str(media), np.stack([y, y], axis=1), sr, subtype="PCM_16")
    out = media.parent / f"{media.stem}_01.wav"
    out.unlink(missing_ok=True)
    _open_and_wait(sess, media)

    _run_one_shot(sess, {"os_slice_mode": "0", "os_trim": "0", "os_fade_in": "0", "os_fade_out": "0",
                         "os_norm_mode": "2", "os_target": "-16000", "os_pattern": "{name}_{nn}"})

    assert out.exists(), f"One-Shot wrote no file: {out} (toast {_last_toast(sess)!r})"
    got = sf.read(str(out), dtype="float64", always_2d=True)[0]
    peak = float(np.abs(got).max())
    full = np.abs(got).max(axis=1) >= 0.9999
    run = 0; longest = 0
    for f in full:
        run = run + 1 if f else 0
        longest = max(longest, run)
    print(f"\n[oneshot] LUFS-I -16 on 16-bit: peak {20*np.log10(peak):.2f} dBFS, longest full-scale run {longest}")
    assert peak <= 10 ** (-0.1 / 20) + 1e-4, f"output peaks at {20*np.log10(peak):.2f} dBFS (ceiling -0.1)"
    assert longest < 3, f"clipped: {longest} consecutive full-scale samples"


def test_one_shot_regions_mode_is_refused_in_standalone(sess):
    """REGIONS slices by the PROJECT's regions - a Standalone file is not on
    the timeline, so the mode is refused with a message instead of cutting
    the file by unrelated regions. Control (cb48cd5): files written."""
    media = burst_fixture("sa_regions_10s.wav", seconds=10, channels=2)
    for p in media.parent.glob(f"{media.stem}_0*.wav"):
        p.unlink()
    _open_and_wait(sess, media)
    sess.eval('reaper.AddProjectMarker2(0, true, 1.0, 2.0, "r1", 1, 0) '
              'reaper.AddProjectMarker2(0, true, 4.0, 6.0, "r2", 2, 0) return true')

    _run_one_shot(sess, {"os_slice_mode": "1", "os_trim": "0", "os_fade_in": "0", "os_fade_out": "0",
                         "os_norm_mode": "0", "os_pattern": "{name}_{nn}"})

    written = sorted(p.name for p in media.parent.glob(f"{media.stem}_0*.wav"))
    toast = _last_toast(sess)
    assert not written, f"REGIONS mode sliced a Standalone file by the project's regions: {written}"
    assert "REGIONS uses the project" in toast, f"refusal toast missing: {toast!r}"
