"""Peak Normalize on long items (finding F17).

Items over the 10M-frame cap hold a downsampled working buffer that flattens
transients (F13: a full-scale click reads far below its true level), so
DoNormalize measured a too-low peak and set D_VOL too hot. Since the fix the
peak streams from the source at full rate (no buffer needed). The fixture is a
quiet 5-minute bed with ONE full-scale single-sample click - the downsampling
is guaranteed to miss it, so the 213eaa9 control normalizes way above target
-> RED; the fix lands D_VOL at 0.989 / 0.9.
"""
import time
from pathlib import Path

import numpy as np
import soundfile as sf

from conftest import (SELECT_ITEM0, clear_project, ensure_window,
                      insert_item_unselected, perf_media_dir, wait_audio_loaded)

SR = 44100


def spike_fixture() -> Path:
    """5-minute stereo bed at 0.03 with a single-sample 0.9 spike at 30 s.
    Read-only (Normalize is non-destructive), so it is cached once."""
    path = perf_media_dir() / "long5min_spike24.wav"
    if not path.exists():
        with sf.SoundFile(str(path), "w", samplerate=SR, channels=2, subtype="PCM_24") as f:
            for start in range(0, 300 * SR, SR * 10):
                t = (np.arange(SR * 10) + start) / SR
                y = 0.03 * np.sin(2 * np.pi * 220 * t)
                if start <= 30 * SR < start + SR * 10:
                    y[30 * SR - start] = 0.9
                f.write(np.repeat(y[:, None], 2, axis=1).astype(np.float64))
    return path


def test_peak_normalize_on_a_long_item_measures_the_true_peak(sess):
    clear_project(sess)
    media = spike_fixture()
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=30)   # lazy: title only, no buffer

    sess.eval('reaper.Main_OnCommand(reaper.NamedCommandLookup("_SneakPeak_Normalize"), 0) return true',
              hang_timeout=60)   # streamed peak scan runs synchronously
    time.sleep(0.5)
    vol = float(sess.eval('local it = reaper.GetTrackMediaItem(reaper.GetTrack(0,0), 0) '
                          'return reaper.GetMediaItemInfo_Value(it, "D_VOL")'))
    want = 0.989 / 0.9
    print(f"\n[normalize] D_VOL = {vol:.4f} (want {want:.4f} = -0.1 dB over the 0.9 spike)")
    assert abs(vol - want) < 0.02, (
        f"peak normalize missed the true peak: D_VOL {vol:.4f}, expected {want:.4f} "
        f"(a downsampled-buffer scan sets it far higher)")
