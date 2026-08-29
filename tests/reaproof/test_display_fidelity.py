"""What the waveform SHOWS on a long item must be the truth (v2.5 increment 8d).

Items over the 10M-frame cap are decoded into a downsampled working buffer
(8 kHz for 20 minutes of 44.1k stereo). Peaks computed from that buffer are
wrong: a one-sample click at 0.9 reads 0.16 through the 8 kHz accessor and
a 12 kHz tone vanishes (measured 2026-08-28). REAPER's own .reapeaks - what
the arrange view draws - keep them. The display of such items must therefore
come from .reapeaks even after the buffer is installed.

Observable: a screen capture at zoom-to-fit. Each click column must span
most of the lane height; through the 8 kHz buffer it spans ~16 %.
"""
from __future__ import annotations

import time

import numpy as np

from conftest import (SELECT_ITEM0, capture, clear_project, client_size, ensure_window,
                      insert_item_unselected, perf_media_dir, wait_audio_loaded,
                      window_title)

SHOT = perf_media_dir() / "shots" / "fidelity_fit.png"
CLICK_EVERY_S = 10
MINUTES = 20


def _click_fixture():
    """20 min of near-silence (0.02 sine bed) with a bipolar click (+0.9 then
    -0.9, one sample each) every 10 s so a click column spans the whole lane;
    24-bit stereo. Never edited, so no pristine copy is needed."""
    import soundfile as sf
    media = perf_media_dir() / "long20min_bipolar_clicks.wav"
    if media.exists():
        return media
    sr = 44100
    with sf.SoundFile(str(media), "w", samplerate=sr, channels=2, subtype="PCM_24") as f:
        for start in range(0, MINUTES * 60 * sr, sr * 10):
            t = (np.arange(sr * 10) + start) / sr
            y = 0.02 * np.sin(2 * np.pi * 220 * t)
            y[int(0.5 * sr)] = 0.9                       # one bipolar click per 10 s chunk
            y[int(0.5 * sr) + 1] = -0.9
            f.write(np.stack([y, y], axis=1).astype(np.float32))
    return media


def _waveform_area(img: np.ndarray, x0: int, x1: int) -> tuple[int, int]:
    """Rows of a waveform lane = the longest run of mostly-dark rows between x0
    and x1 (ruler, toolbar and meter are grey). Click columns take ~18 % of a
    row and the dB grid lines are single non-dark rows, so the threshold is
    loose and gaps of a few rows are bridged."""
    dark = (img[:, x0:x1, :].max(axis=2) < 40).mean(axis=1) > 0.6
    filled = dark.copy()
    for y in range(len(dark)):
        if not dark[y] and dark[max(0, y - 4):y].any() and dark[y + 1:y + 5].any():
            filled[y] = True
    best, run_start, best_len = (0, 0), None, 0
    for y, d in enumerate(list(filled) + [False]):
        if d and run_start is None:
            run_start = y
        elif not d and run_start is not None:
            if y - run_start > best_len:
                best, best_len = (run_start, y), y - run_start
            run_start = None
    return best


def test_click_peaks_survive_on_a_long_item_at_zoom_to_fit(sess):
    media = _click_fixture()
    clear_project(sess)
    insert_item_unselected(sess, media)
    # A fresh file has no .reapeaks and the SDK display path waits for them:
    # build them synchronously so the capture never races the peak builder.
    sess.eval("""
      local src = reaper.GetMediaItemTake_Source(reaper.GetActiveTake(reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), 0)))
      if reaper.PCM_Source_BuildPeaks(src, 0) ~= 0 then
        while reaper.PCM_Source_BuildPeaks(src, 1) ~= 0 do end
        reaper.PCM_Source_BuildPeaks(src, 2)
      end
      return true""", hang_timeout=300)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=120)   # buffer installed = the old path switched
    sess.eval('reaper.Main_OnCommand(reaper.NamedCommandLookup("_SneakPeak_ZoomFit"), 0) return true')
    time.sleep(1.0)                                    # repaint settles

    SHOT.parent.mkdir(parents=True, exist_ok=True)
    print(f"\n[fidelity] title at capture: {window_title(sess)!r}")
    cap = capture(sess, SHOT)
    cw, ch = client_size(sess)
    img = cap.image[cap.height - ch:, :cw, :]          # client area only
    x0, x1 = 40, cw - 80                               # clear of the dB scale on either side
    y0, y1 = _waveform_area(img, x0, x1)
    area_h = y1 - y0
    assert area_h > 100, f"waveform area not found in the capture ({y0}, {y1})"

    lane = img[y0:y1, x0:x1, :]
    extent = (lane.max(axis=2) >= 40).sum(axis=0)      # non-black pixels per column
    baseline = float(np.median(extent))                # center lines, grid, 0.02 bed
    tall = int((extent > baseline + 0.4 * area_h).sum())   # SDK: ~90 % of the lane; 8 kHz buffer: ~16 %
    expected = MINUTES * 60 // CLICK_EVERY_S           # 120 clicks, ~6 px apart at fit
    print(f"\n[fidelity] area {area_h}px, baseline {baseline:.0f}px, tall columns {tall}/{expected}")
    assert tall >= expected // 2, (
        f"only {tall} of {expected} click columns reach the lane height - the display "
        f"is drawn from the downsampled buffer (see {SHOT})")


# ---------------------------------------------------------------------------
# A9.6: the minimap folds the take channel mode like the waveform does
# ---------------------------------------------------------------------------
MINIMAP_ACTIVE = (90, 130, 160)      # minimap_view.cpp activeColor (in-view columns)
MINIMAP_BG = (12, 14, 18)            # minimap_view.cpp bgBrush (the band itself)
LR_BURST = (150.0, 160.0)            # s: loud on L (0.9), half on R (0.4)
CM_MINIMAP = 2030                    # edit_view.h enum ContextMenuID (CM_UNDO = 2000)


def _lr_split_fixture():
    """5 min of a 0.02 bed on both channels with a 10 s burst where L is 0.9 and
    R is 0.4: a channel fold that ignores I_CHANMODE shows the same (averaged)
    column height for 'mono left' and 'mono right'; the honest fold shows L
    more than twice as tall as R. Stereo 44.1k x 5 min = 13.2 M
    frames > the 10 M cap, so the item stays lazy and both the waveform and the
    minimap draw from .reapeaks. Never edited, no pristine copy needed."""
    import soundfile as sf
    media = perf_media_dir() / "long5min_lr_split.wav"
    if media.exists():
        return media
    sr = 44100
    with sf.SoundFile(str(media), "w", samplerate=sr, channels=2, subtype="PCM_24") as f:
        for start in range(0, 5 * 60 * sr, sr * 10):
            t = (np.arange(sr * 10) + start) / sr
            bed = 0.02 * np.sin(2 * np.pi * 220 * t)
            burst = (t >= LR_BURST[0]) & (t < LR_BURST[1])
            left, right = bed.copy(), bed.copy()
            left[burst] = 0.9 * np.sin(2 * np.pi * 220 * t[burst])
            right[burst] = 0.4 * np.sin(2 * np.pi * 220 * t[burst])
            f.write(np.stack([left, right], axis=1).astype(np.float32))
    return media


def _minimap_burst_height(sess, out) -> tuple[int, int]:
    """(burst column height, band height) in the minimap. The band = the rows
    carrying the minimap background; -1 when there is no band (minimap hidden);
    0 while the band shows no steel-blue columns yet. Burst columns = the x
    range of LR_BURST at zoom-to-fit."""
    cap = capture(sess, out)
    cw, ch = client_size(sess)
    img = cap.image[cap.height - ch:, :cw, :].astype(int)
    bg_rows = np.nonzero(np.all(img == np.array(MINIMAP_BG), axis=2).sum(axis=1) > cw * 0.2)[0]
    if len(bg_rows) == 0:
        return -1, 0
    y0, y1 = int(bg_rows.min()), int(bg_rows.max()) + 1
    active = np.all(img[y0:y1] == np.array(MINIMAP_ACTIVE), axis=2)
    x0 = int(LR_BURST[0] / 300.0 * cw) + 2
    x1 = int(LR_BURST[1] / 300.0 * cw) - 2
    return int(active[:, x0:x1].sum(axis=0).max()), y1 - y0


def _set_chanmode(sess, mode: int):
    sess.eval(f"""
      local it = reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), 0)
      reaper.Undo_BeginBlock()
      reaper.SetMediaItemTakeInfo_Value(reaper.GetActiveTake(it), "I_CHANMODE", {mode})
      reaper.UpdateItemInProject(it)
      reaper.Undo_EndBlock("chanmode", -1)
      return true""")


def test_minimap_matches_the_waveform_channel_mode(sess):
    media = _lr_split_fixture()
    clear_project(sess)
    insert_item_unselected(sess, media)
    sess.eval("""
      local src = reaper.GetMediaItemTake_Source(reaper.GetActiveTake(reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), 0)))
      if reaper.PCM_Source_BuildPeaks(src, 0) ~= 0 then
        while reaper.PCM_Source_BuildPeaks(src, 1) ~= 0 do end
        reaper.PCM_Source_BuildPeaks(src, 2)
      end
      return true""", hang_timeout=300)
    ensure_window(sess)
    from conftest import DESELECT_ALL, send_command
    heights = {}
    for mode in (3, 4):                                # mono left, mono right
        _set_chanmode(sess, mode)
        sess.eval(DESELECT_ALL)
        time.sleep(0.4)
        sess.eval(SELECT_ITEM0)                        # a fresh load with the mode applied
        wait_audio_loaded(sess, media.stem, timeout=120)
        sess.eval('reaper.Main_OnCommand(reaper.NamedCommandLookup("_SneakPeak_ZoomFit"), 0) return true')
        shot = perf_media_dir() / "shots" / f"minimap_chanmode{mode}.png"
        h, band = -1, 0
        for _ in range(30):                            # the reload repaints the band, then its columns
            time.sleep(0.5)
            h, band = _minimap_burst_height(sess, shot)
            if h > 0:
                break
            if h < 0 and _ == 6:                       # still no band: the minimap is hidden here
                send_command(sess, CM_MINIMAP)
        assert h > 0, f"no minimap columns in the capture (h={h}, band={band})"
        heights[mode] = (h, band)
        print(f"\n[fidelity] minimap burst column {h}px of a {band}px band with I_CHANMODE={mode}")
    (h_l, band), (h_r, _) = heights[3], heights[4]
    assert h_l >= band * 0.35, f"the L-only burst (0.9) should fill the minimap band: {heights}"
    assert h_r * 1.8 <= h_l, (
        f"the minimap ignores the channel mode: mono-left {h_l}px vs mono-right {h_r}px "
        f"(R is 0.4, L is 0.9 - the columns must differ by more than 1.8x)")
