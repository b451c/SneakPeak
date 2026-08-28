"""Exports from long items must be written at the source rate (finding F11).

Items over the 10M-frame cap (~3.8 min stereo at 44.1k) are held in a
DOWNSAMPLED working buffer; every exporter used to write that buffer, so a
5-minute item came out as a 22050 Hz file. Ground truth: the written file
itself (soundfile) against the source's bytes for the exported window.
The 3-minute case (full-rate buffer) is GREEN on the old build too: it proves
the stream reproduces what the loader built.
"""
from __future__ import annotations

import numpy as np
import soundfile as sf

from conftest import (SELECT_ITEM0, burst_fixture, clear_project, ensure_window,
                      insert_item_unselected, perf_media_dir, send_command,
                      wait_audio_loaded)

CM_EDIT_COPY_STANDALONE = 2258   # edit_view.h enum ContextMenuID (parsed 2026-08-28)
SR = 44100


def _source_window(path, start_frame: int, end_frame: int) -> np.ndarray:
    """24-bit PCM as exact doubles (int24 / 2^23), frames x channels."""
    with sf.SoundFile(str(path)) as f:
        assert f.subtype == "PCM_24", f.subtype
        f.seek(start_frame)
        raw = f.read(end_frame - start_frame, dtype="int32", always_2d=True)
    return raw.astype(np.float64) / 2147483648.0


def _remove_old_copies(media):
    for p in media.parent.glob(f"{media.stem}_edit*.wav"):
        p.unlink()


def _edit_copy(sess, media, *, load_timeout=90):
    """Select the item, run Edit Copy in Standalone, wait for the new tab."""
    clear_project(sess)
    _remove_old_copies(media)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=load_timeout)
    send_command(sess, CM_EDIT_COPY_STANDALONE)
    out = media.parent / f"{media.stem}_edit.wav"
    wait_audio_loaded(sess, out.name, timeout=load_timeout)
    assert out.exists(), f"no edit copy next to the source: {out}"
    return out


def _assert_equals_source(out, media, frames: int, channels: int, atol=2e-7):
    info = sf.info(str(out))
    assert info.samplerate == SR, f"edit copy written at {info.samplerate} Hz"
    assert info.subtype == "FLOAT", info.subtype
    assert info.channels == channels, info.channels
    assert abs(info.frames - frames) <= 1, f"{info.frames} frames, expected {frames}"
    got = sf.read(str(out), dtype="float64", always_2d=True)[0]
    n = min(len(got), frames)
    want = _source_window(media, 0, n)
    diff = float(np.max(np.abs(got[:n] - want)))
    print(f"\n[export] {out.name}: {info.samplerate} Hz, {info.frames} frames, max |diff| vs source = {diff:.3g}")
    assert diff <= atol, f"edit copy differs from the source window: max |diff| {diff}"


def test_edit_copy_of_a_five_minute_item_keeps_the_source_rate(sess):
    media = burst_fixture("long5min_burst24.wav", seconds=300, channels=2)
    out = _edit_copy(sess, media)
    _assert_equals_source(out, media, 300 * SR, 2)


def test_edit_copy_of_a_three_minute_item_equals_the_source(sess):
    # Full-rate buffer (under the 10M-frame cap): the stream must reproduce it.
    media = burst_fixture("long3min_burst24.wav", seconds=180, channels=2)
    out = _edit_copy(sess, media)
    _assert_equals_source(out, media, 180 * SR, 2)


def _stereo_lr_fixture():
    """Stereo with DIFFERENT channels (the burst fixtures duplicate L into R)."""
    path = perf_media_dir() / "stereo_lr_24.wav"
    if not path.exists():
        t = np.arange(20 * SR) / SR
        left = 0.5 * np.sin(2 * np.pi * 220 * t)
        right = 0.25 * np.sin(2 * np.pi * 331 * t)
        sf.write(str(path), np.stack([left, right], axis=1).astype(np.float32), SR, subtype="PCM_24")
    return path


def test_edit_copy_folds_the_take_channel_mode(sess):
    """I_CHANMODE 2 (mono mix) on the take: the copy is mono and equals (L+R)/2 -
    pins the fold policy (the accessor is assumed to deliver the raw channels)."""
    media = _stereo_lr_fixture()
    clear_project(sess)
    _remove_old_copies(media)
    insert_item_unselected(sess, media)
    sess.eval("""
      local it = reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), 0)
      reaper.SetMediaItemTakeInfo_Value(reaper.GetActiveTake(it), "I_CHANMODE", 2)
      reaper.UpdateArrange() return true""")
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=60)
    send_command(sess, CM_EDIT_COPY_STANDALONE)
    out = media.parent / f"{media.stem}_edit.wav"
    wait_audio_loaded(sess, out.name, timeout=60)
    info = sf.info(str(out))
    assert info.channels == 1, f"channel mode not folded: {info.channels} channels"
    assert info.samplerate == SR
    got = sf.read(str(out), dtype="float64", always_2d=True)[0][:, 0]
    src = _source_window(media, 0, len(got))
    want = (src[:, 0] + src[:, 1]) * 0.5
    diff = float(np.max(np.abs(got - want)))
    print(f"\n[export] chanmode fold: max |diff| vs (L+R)/2 = {diff:.3g}")
    assert diff <= 2e-7, diff
