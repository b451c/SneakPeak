"""Lazy working buffer + 1 GB cap (v2.5 increment 8g, design_lazy_buffer.md).

Items whose buffer would be downsampled (over 3.8 min at 44.1k) decode NOTHING
on select: display and minimap paint from .reapeaks, exports and Dynamics
stream. The buffer is decoded only when a sample consumer asks - a panel
(Spectral, One-Shot) opens at once and fills when the buffer lands; an item
whose buffer would exceed WaveformView::kMaxBufferBytes (1 GB) is refused
with a toast and never allocated.

Observables: the window title (the loader retitles to "Loading item audio...",
a lazy select never does), REAPER's resident set (ps), and the client pixels
of the spectral pane (near-black placeholder -> spectrogram). The companion
assertions on select / delete / SET / Reverse / Dynamics live in test_perf_1h,
test_perf_edit, test_set_and_gates and test_dynamics_stream.
"""
from __future__ import annotations

import json
import time
from pathlib import Path


from conftest import (SELECT_ITEM0, assert_no_loading, capture, clear_project,
                      client_size, ensure_window, insert_item_unselected,
                      perf_media_dir, rss_mb, send_command, wait_audio_loaded,
                      window_title, write_long_wav)

RESULTS = Path("/tmp/sneakpeak-perf-results.json")
SHOTS = Path("/tmp/sneakpeak-reaproof-shots/lazy")
CM_TOGGLE_SPECTRAL = 2028      # edit_view.h enum ContextMenuID (CM_UNDO = 2000)
CM_COPY, CM_PASTE = 2002, 2003
RSS_BUDGET_MB = 50
SR_SOURCE = 44100


def _record(name: str, m: dict):
    data = json.loads(RESULTS.read_text()) if RESULTS.exists() else {}
    data[name] = m
    RESULTS.write_text(json.dumps(data, indent=1))
    print(f"\n[lazy] {name}: {m}")


def _pane_lit_fraction(s, out: Path) -> float:
    """Fraction of non-black pixels inside the SPECTRAL PANE rect (RecalcLayout at
    UI scale 1: mode bar 20 + ruler 28 above, minimap 20 + scrollbar 14 + bottom
    panel 52 below, waveform 55% of the content, 5 px splitter; the right dB
    scale column excluded). With the pane closed those rows are the lower part
    of the waveform lane (the 0.6-amplitude fixture reaches into them); open
    without audio they are the (5,5,10) placeholder; computed = spectrogram."""
    cap = capture(s, out)
    cw, ch = client_size(s)
    titlebar = cap.height - ch
    content_top, content_bot = 48, ch - 86
    wave_h = int((content_bot - content_top) * 0.55) - 2
    pane_top, pane_bot = content_top + wave_h + 5, content_bot
    band = cap.image[titlebar + pane_top:titlebar + pane_bot, 4:cw - 46, :3].astype(int)
    lit = band.max(axis=2) > 40
    return float(lit.mean())


def test_spectral_opens_at_once_and_fills_when_the_buffer_lands(sess):
    media = write_long_wav(perf_media_dir() / "long20min_stereo.wav", minutes=20)   # 8 kHz plan = lazy
    clear_project(sess)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=30)
    assert_no_loading(sess, 1.5)
    SHOTS.mkdir(parents=True, exist_ok=True)
    before = _pane_lit_fraction(sess, SHOTS / "spectral_before.png")

    send_command(sess, CM_TOGGLE_SPECTRAL)
    t0 = time.monotonic()
    # the pane opens immediately (dark placeholder) and the loader starts
    sess.wait_until(lambda: "Loading" in window_title(sess), timeout=5)
    placeholder = _pane_lit_fraction(sess, SHOTS / "spectral_placeholder.png")
    wait_audio_loaded(sess, media.stem, timeout=90)
    sess.wait_until(lambda: _pane_lit_fraction(sess, SHOTS / "spectral_after.png") > placeholder + 0.02,
                    timeout=60)
    after = _pane_lit_fraction(sess, SHOTS / "spectral_after.png")
    m = {"lit_before": round(before, 3), "lit_placeholder": round(placeholder, 3),
         "lit_after": round(after, 3), "t_filled_s": round(time.monotonic() - t0, 2)}
    _record("lazy.spectral_20min", m)
    # measured: waveform lane 0.67, placeholder 0.05 (splitter dots + scale
    # lines), spectrogram 0.17 (a 220/331 Hz tone lights the low band only)
    assert placeholder < 0.1, f"the spectral pane should open as a dark placeholder: {m}"
    assert after > placeholder + 0.02, f"the spectrogram never painted after the buffer landed: {m}"
    send_command(sess, CM_TOGGLE_SPECTRAL)   # leave the view as we found it


def test_item_over_the_buffer_cap_is_refused_without_allocating(sess):
    # 2.5 h stereo at an 8 kHz SOURCE rate: PlanRead's floor keeps the read rate
    # at 8 kHz (not "downsampled"), yet the buffer would be 1.15 GB of doubles.
    media = write_long_wav(perf_media_dir() / "long150min_8k_stereo.wav", minutes=150, sr=8000)
    clear_project(sess)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    rss0 = rss_mb(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=60)
    assert_no_loading(sess, 3.0)
    SHOTS.mkdir(parents=True, exist_ok=True)
    before = _pane_lit_fraction(sess, SHOTS / "cap_before.png")

    send_command(sess, CM_TOGGLE_SPECTRAL)   # a sample consumer asks -> refused with a toast
    last = assert_no_loading(sess, 3.0)
    after = _pane_lit_fraction(sess, SHOTS / "cap_after.png")
    m = {"rss_delta_mb": round(rss_mb(sess) - rss0, 1), "title": last,
         "lit_before": round(before, 3), "lit_after": round(after, 3)}
    _record("lazy.cap_150min", m)
    assert m["rss_delta_mb"] < RSS_BUDGET_MB, f"an over-cap item allocated its buffer: {m}"
    # the pane rows still show the waveform lane (a placeholder would be all dark)
    assert before > 0.1 and after > 0.1, f"the spectral pane opened on an over-cap item: {m}"


def _bottom_panel_lit_fraction(s, out: Path) -> float:
    """Fraction of non-dark pixels in the bottom panel (52 px at UI scale 1):
    the level meter bars live there; stopped, they decay to nothing."""
    cap = capture(s, out)
    cw, ch = client_size(s)
    titlebar = cap.height - ch
    band = cap.image[titlebar + ch - 50:titlebar + ch - 2, 2:cw - 2, :3].astype(int)
    return float((band.max(axis=2) > 60).mean())


def _paste_source_rates(s) -> list[int]:
    return list(s.eval("""
      local out = {}
      local tr = reaper.GetTrack(0, 0)
      for i = 0, reaper.CountTrackMediaItems(tr) - 1 do
        local tk = reaper.GetActiveTake(reaper.GetTrackMediaItem(tr, i))
        local src = reaper.GetMediaItemTake_Source(tk)
        local path = reaper.GetMediaSourceFileName(src, "")
        if path:find("sneakpeak_paste") then out[#out + 1] = reaper.GetMediaSourceSampleRate(src) end
      end
      return out""", hang_timeout=120) or [])


def test_copy_paste_on_a_lazy_item_streams_at_the_source_rate(sess):
    """Copy used to lift the selection out of the working buffer - 8 kHz on a
    20-minute item - and Paste then inserted that 8 kHz temp WAV into the
    project. Copy now streams the selection at the source rate and needs no
    buffer at all: ground truth = the pasted item's source sample rate."""
    from conftest import WAVE_Y, click_client, drag_client, track_item_count
    media = write_long_wav(perf_media_dir() / "long20min_stereo.wav", minutes=20)
    clear_project(sess)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=30)
    assert_no_loading(sess, 1.5)
    rss0 = rss_mb(sess)

    drag_client(sess, 200, WAVE_Y, 220, WAVE_Y)      # ~30 s of the item
    time.sleep(0.3)
    send_command(sess, CM_COPY)
    assert_no_loading(sess, 2.0)                      # streamed, not decoded
    click_client(sess, 700, WAVE_Y)                   # cursor deep in the item
    time.sleep(0.3)
    n0 = track_item_count(sess)
    send_command(sess, CM_PASTE)
    sess.wait_until(lambda: track_item_count(sess) > n0, timeout=30)
    sess.wait_until(lambda: len(_paste_source_rates(sess)) == 1, timeout=10)
    rates = _paste_source_rates(sess)
    m = {"paste_rates": rates, "rss_delta_mb": round(rss_mb(sess) - rss0, 1)}
    _record("lazy.copy_paste_20min", m)
    assert rates == [SR_SOURCE], f"the pasted audio is not at the source rate: {m}"
    assert m["rss_delta_mb"] < RSS_BUDGET_MB + 40, f"Copy decoded a working buffer: {m}"   # +clipboard


def test_level_meter_follows_playback_on_a_lazy_item(sess):
    """With no working buffer the item meter used to sit flat; it now reads
    its window through the view's live take accessor. Observable: the bottom
    panel's meter pixels light up while REAPER plays the item, with no
    Loading title at any point."""
    media = write_long_wav(perf_media_dir() / "long20min_stereo.wav", minutes=20)
    clear_project(sess)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=30)
    assert_no_loading(sess, 1.5)
    SHOTS.mkdir(parents=True, exist_ok=True)
    stopped = _bottom_panel_lit_fraction(sess, SHOTS / "meter_stopped.png")
    sess.eval("reaper.SetEditCurPos(30.0, false, false) reaper.Main_OnCommand(1007, 0) return true")
    try:
        time.sleep(2.0)
        playing = _bottom_panel_lit_fraction(sess, SHOTS / "meter_playing.png")
        last = assert_no_loading(sess, 1.0)
    finally:
        sess.eval("reaper.Main_OnCommand(1016, 0) return true")
    m = {"lit_stopped": round(stopped, 3), "lit_playing": round(playing, 3), "title": last}
    _record("lazy.meter_20min", m)
    assert playing > stopped + 0.01, f"the meter stayed flat on a lazy item: {m}"
