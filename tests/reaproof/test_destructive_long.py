"""Destructive ITEM edits on long items must not degrade the file.

SneakPeak keeps long items in a DOWNSAMPLED working buffer (10M-frame cap:
anything over ~3.8 min at 44.1k stereo). A destructive op that writes that
buffer back would silently re-encode the source at the reduced rate.
Ground truth: REAPER's own view of the source after the edit (sample rate,
bit depth, length) plus the reversal itself via the track audio accessor.
"""
from __future__ import annotations

import time
from pathlib import Path

from conftest import (SELECT_ITEM0, clear_project, db, dismiss_native_modal, ensure_window,
                      insert_item_unselected, perf_media_dir, track_rms_windows,
                      wait_audio_loaded, wait_main_thread_idle)

MINUTES = 5


def _fixture() -> Path:
    media = perf_media_dir() / "long5min_burst24.wav"
    if not media.exists():
        import numpy as np, soundfile as sf
        sr = 44100
        with sf.SoundFile(str(media), "w", samplerate=sr, channels=2, subtype="PCM_24") as f:
            for start in range(0, MINUTES * 60 * sr, sr * 10):
                t = (np.arange(sr * 10) + start) / sr
                y = 0.03 * np.sin(2 * np.pi * 220 * t)
                burst = (t >= 0.5) & (t < 1.5)
                y[burst] = 0.9 * np.sin(2 * np.pi * 220 * t[burst])
                f.write(np.stack([y, y], axis=1).astype(np.float32))
    return media


def _source_info(sess) -> dict:
    return sess.eval("""
      local it = reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), 0)
      local src = reaper.GetMediaItemTake_Source(reaper.GetActiveTake(it))
      local len = reaper.GetMediaSourceLength(src)
      return { sr = reaper.GetMediaSourceSampleRate(src), bits = reaper.CF_GetMediaSourceBitDepth(src),
               len = len, nch = reaper.GetMediaSourceNumChannels(src) }""", hang_timeout=120)


def test_reverse_on_a_five_minute_item_keeps_rate_depth_and_length(sess):
    media = _fixture()
    clear_project(sess)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=60)
    before = _source_info(sess)
    assert before["sr"] == 44100 and before["bits"] == 24, before
    total = MINUTES * 60.0
    w_head, w_tail = (0.6, 1.4), (total - 1.4, total - 0.6)

    sess.eval('reaper.defer(function() reaper.Main_OnCommand(reaper.NamedCommandLookup("_SneakPeak_Reverse"), 0) end) return true')
    assert dismiss_native_modal(sess), "the destructive confirmation never appeared"
    wait_main_thread_idle(sess, timeout=240)
    time.sleep(1.0)

    after = _source_info(sess)
    assert after["sr"] == before["sr"], f"destructive write changed the sample rate: {before} -> {after}"
    assert after["bits"] == before["bits"], f"destructive write changed the bit depth: {before} -> {after}"
    assert abs(after["len"] - before["len"]) < 0.01, f"length changed: {before} -> {after}"
    head, tail = track_rms_windows(sess, [w_head, w_tail])
    assert db(tail) > db(head) + 20, f"the item was not reversed: head {db(head):.1f} tail {db(tail):.1f}"


def test_reverse_on_a_short_item_is_audible_in_reaper(sess):
    """F7 isolated from the length question: after a destructive Reverse the
    NEW audio must be what REAPER serves for the take (take + track accessor),
    not the pre-edit file pinned by an open decoder."""
    import numpy as np, soundfile as sf
    media = perf_media_dir() / "short_burst24.wav"
    if not media.exists():
        sr = 44100
        t = np.arange(sr * 10) / sr
        y = 0.03 * np.sin(2 * np.pi * 220 * t)
        burst = (t >= 0.5) & (t < 1.5)
        y[burst] = 0.9 * np.sin(2 * np.pi * 220 * t[burst])
        sf.write(str(media), np.stack([y, y], axis=1).astype(np.float32), sr, subtype="PCM_24")
    clear_project(sess)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=30)
    w_head, w_tail = (0.6, 1.4), (8.6, 9.4)
    head0, tail0 = track_rms_windows(sess, [w_head, w_tail])
    assert db(head0) > db(tail0) + 20

    sess.eval('reaper.defer(function() reaper.Main_OnCommand(reaper.NamedCommandLookup("_SneakPeak_Reverse"), 0) end) return true')
    assert dismiss_native_modal(sess), "the destructive confirmation never appeared"
    wait_main_thread_idle(sess, timeout=60)
    time.sleep(1.0)
    head, tail = track_rms_windows(sess, [w_head, w_tail])
    assert db(tail) > db(head) + 20, f"REAPER still serves the pre-edit audio: head {db(head):.1f} tail {db(tail):.1f}"
    take = sess.eval("""
      local acc = reaper.CreateTakeAudioAccessor(reaper.GetActiveTake(reaper.GetTrackMediaItem(reaper.GetTrack(0,0),0)))
      local out = {}
      for _, w in ipairs({ {0.6,1.4}, {8.6,9.4} }) do
        local n = math.floor((w[2]-w[1])*44100) local buf = reaper.new_array(n) buf.clear()
        reaper.GetAudioAccessorSamples(acc, 44100, 1, w[1], n, buf)
        local a = 0 for i = 1, n do a = a + buf[i]*buf[i] end out[#out+1] = math.sqrt(a/n)
      end
      reaper.DestroyAudioAccessor(acc) return out""", hang_timeout=60)
    assert db(take[1]) > db(take[0]) + 20, f"take accessor stale: {take}"
    bits = int(sess.eval("local it = reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), 0) "
                         "return reaper.CF_GetMediaSourceBitDepth(reaper.GetMediaItemTake_Source(reaper.GetActiveTake(it)))",
                         hang_timeout=60))
    assert bits == 24, f"destructive write changed the bit depth: {bits}"
