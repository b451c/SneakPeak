"""Temp-file paths (finding F20, forum #105 - Windows).

Every temporary WAV SneakPeak writes (Standalone preview, paste clip, undo
snapshot, One-Shot LUFS measurement, export fallback) used a "/tmp" fallback
that does not exist on Windows, so the write failed silently - "no playback in
Standalone" was the visible symptom. The observable effect: pressing Play in
Standalone must produce the preview file in the platform temp
directory (tempfile.gettempdir() == %TEMP% on Windows, $TMPDIR elsewhere - the same
lookup AudioEngine::TempDir() performs). Control: the 306fe07 build on Windows
writes nothing (C:\\tmp is absent) -> RED.
"""
import tempfile
import time
from pathlib import Path

import soundfile as sf

from conftest import (SELECT_ITEM0, burst_fixture, clear_project, ensure_window,
                      insert_item_unselected, press_key, send_command, wait_audio_loaded)

VK_SPACE = 0x20   # Standalone play/stop is bound to Space in our own key handler

CM_EDIT_COPY_STANDALONE = 2258   # edit_view.h enum ContextMenuID (parsed 2026-08-28)


def test_standalone_play_writes_the_preview_into_the_system_temp_dir(sess):
    clear_project(sess)
    media = burst_fixture("short_burst24.wav", seconds=10, channels=2)
    for p in media.parent.glob(f"{media.stem}_edit*.wav"):
        p.unlink()
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=60)
    send_command(sess, CM_EDIT_COPY_STANDALONE)
    out = media.parent / f"{media.stem}_edit.wav"
    wait_audio_loaded(sess, out.name, timeout=60)   # the Standalone tab is up

    preview = Path(tempfile.gettempdir()) / f"sneakpeak_preview_{sess.handle.pid}.wav"
    preview.unlink(missing_ok=True)
    press_key(sess, VK_SPACE)   # Standalone play - writes the preview WAV, then plays it
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline and not preview.exists():
        time.sleep(0.1)
    assert preview.exists(), f"Standalone Play wrote no preview file at {preview}"
    info = sf.info(str(preview))
    print(f"\n[temp] preview {preview}: {info.samplerate} Hz, {info.channels} ch, {info.frames} frames")
    assert info.samplerate == 44100 and info.channels == 2
    assert abs(info.frames - 10 * 44100) <= 1
    press_key(sess, VK_SPACE)   # stop
