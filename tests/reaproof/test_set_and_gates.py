"""SET (working set) view + the "still loading" gates - both rewired in
phase 2a without a spec of their own.

SET: two items on one track -> "SneakPeak: Toggle track view" -> the view
concatenates the working set (mode bar reads SET), draws from .reapeaks
at once and installs the samples in the background.

Gates: a destructive ITEM op fired while the audio is still loading must be
a no-op (toast, never a synchronous fallback decode); the same op after the
load must do its job. Ground truth for Reverse = the track audio accessor:
the burst that started the file must end it.
"""
from __future__ import annotations

import json
import time
from pathlib import Path

from conftest import (burst_fixture, clear_project, CM_TRACK_VIEW, db,
                      dismiss_native_modal, ensure_window,
                      insert_item_unselected, measure_after,
                      mode_from_capture, perf_media_dir, track_rms_windows,
                      wait_audio_loaded, wait_main_thread_idle, write_long_wav)

RESULTS = Path("/tmp/sneakpeak-perf-results.json")
SHOTS = Path("/tmp/sneakpeak-reaproof-shots/set")
STALL_BUDGET = 0.25


def _record(name: str, m: dict):
    data = json.loads(RESULTS.read_text()) if RESULTS.exists() else {}
    data[name] = m
    RESULTS.write_text(json.dumps(data, indent=1))
    print(f"\n[perf] {name}: {m}")


def test_working_set_view_loads_in_background(sess):
    media = write_long_wav(perf_media_dir() / "long20min_stereo.wav", minutes=20)
    clear_project(sess)
    insert_item_unselected(sess, media)
    # second item on the SAME track, right after the first
    sess.eval("""
      local tr = reaper.GetTrack(0, 0)
      local it = reaper.AddMediaItemToTrack(tr)
      local src = reaper.GetMediaItemTake_Source(reaper.GetActiveTake(reaper.GetTrackMediaItem(tr, 0)))
      local tk = reaper.AddTakeToMediaItem(it)
      reaper.SetMediaItemTake_Source(tk, src)
      reaper.SetMediaItemInfo_Value(it, "D_POSITION", 1230.0)
      reaper.SetMediaItemInfo_Value(it, "D_LENGTH", 600.0)
      reaper.SelectAllMediaItems(0, true)
      reaper.UpdateArrange()
      return true""")
    ensure_window(sess)
    m = measure_after(sess, f"local h = SP_WINDOW() "
                            f'reaper.JS_WindowMessage_Post(h, "WM_COMMAND", {CM_TRACK_VIEW}, 0, 0, 0) return true',
                      loaded_marker=media.stem, max_wait=120, quiet=1.0)
    _record("set.enter", m)
    mode = mode_from_capture(sess, SHOTS / "set.png")
    assert mode == "SET", f"expected SET view, got {mode}"
    assert m["max_stall"] <= STALL_BUDGET, f"entering the working set froze REAPER: {m}"
    assert m["seen_loading"] and m["t_loaded"] is not None, f"background load not observed: {m}"


def test_reverse_is_gated_while_loading_then_works(sess):
    clear_project(sess)
    # 20 minutes, quiet, one loud burst in the first two seconds; 24-bit so the
    # write-back format is observable (v2.4.0 re-encoded items as 16-bit)
    media = burst_fixture("long20min_burst24.wav", seconds=20 * 60, channels=1)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    total = 20 * 60.0
    w_head, w_tail = (0.6, 1.4), (total - 1.4, total - 0.6)
    head0, tail0 = track_rms_windows(sess, [w_head, w_tail])
    assert db(head0) > db(tail0) + 20, "fixture: the burst must be at the head"

    # Fire the destructive op INSIDE REAPER the moment the loader retitles the
    # window (a bridge round trip is ~0.5 s - too coarse to hit a 1-2 s load).
    fire_lua = """
      local it = reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), 0)
      reaper.SetMediaItemSelected(it, true) reaper.UpdateArrange()
      SP_GATE_FIRED = nil
      local t0 = reaper.time_precise()
      local function poll()
        local h = SP_WINDOW()
        local title = h and reaper.JS_Window_GetTitle(h) or ""
        if title:find("Loading") then
          reaper.Main_OnCommand(reaper.NamedCommandLookup("_SneakPeak_Reverse"), 0)
          SP_GATE_FIRED = title
          return
        end
        if reaper.time_precise() - t0 < 20 then reaper.defer(poll) else SP_GATE_FIRED = false end
      end
      reaper.defer(poll)
      return true"""
    sess.eval(fire_lua)
    sess.wait_until(lambda: sess.eval("return SP_GATE_FIRED ~= nil", hang_timeout=120), timeout=30)
    fired = sess.eval("return SP_GATE_FIRED", hang_timeout=120)
    assert fired, "the background load never showed a Loading title (driver)"
    wait_audio_loaded(sess, media.stem, timeout=90)
    time.sleep(2.0)
    head1, tail1 = track_rms_windows(sess, [w_head, w_tail])
    assert db(head1) > db(tail1) + 20, (
        f"Reverse during load must be a no-op: head {db(head1):.1f} dB, tail {db(tail1):.1f} dB")

    sess.eval('reaper.defer(function() reaper.Main_OnCommand(reaper.NamedCommandLookup("_SneakPeak_Reverse"), 0) end) return true')
    assert dismiss_native_modal(sess), "the destructive confirmation never appeared"
    wait_main_thread_idle(sess, timeout=240)   # the 20-min rewrite blocks REAPER for a while
    sess.wait_until(lambda: (lambda h, t: db(t) > db(h) + 20)(*track_rms_windows(sess, [w_head, w_tail])),
                    timeout=60)
    head2, tail2 = track_rms_windows(sess, [w_head, w_tail])
    assert db(tail2) > db(head2) + 20, "Reverse after the load must reverse the item"
    # the file is written back in its own format (24-bit stays 24-bit)
    bits = int(sess.eval("local it = reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), 0) "
                         "local src = reaper.GetMediaItemTake_Source(reaper.GetActiveTake(it)) "
                         "return reaper.CF_GetMediaSourceBitDepth(src)", hang_timeout=120))
    assert bits == 24, f"destructive write changed the bit depth: {bits}"


def test_working_set_cannot_lock_the_view_after_its_items_die(sess):
    """F1: SET view, then the project is cleared (set items + track gone), then a
    NEW item is selected - it must load normally, every time. Before the fix the
    stale 'active' set compared raw pointers; whenever the new item reused a
    freed address LoadSelectedItem returned 'stay locked' and nothing loaded.
    Address reuse is up to the allocator, so the cycle repeats several times."""
    media = write_long_wav(perf_media_dir() / "long20min_stereo.wav", minutes=20)
    short = perf_media_dir() / "short_tone.wav"
    if not short.exists():
        import numpy as np, soundfile as sf
        t = np.arange(44100 * 8) / 44100
        sf.write(str(short), (0.5 * np.sin(2 * np.pi * 330 * t)).astype(np.float32), 44100)
    for cycle in range(5):
        clear_project(sess)
        insert_item_unselected(sess, media)
        sess.eval("""
          local tr = reaper.GetTrack(0, 0)
          for i = 1, 3 do
            local it = reaper.AddMediaItemToTrack(tr)
            local src = reaper.GetMediaItemTake_Source(reaper.GetActiveTake(reaper.GetTrackMediaItem(tr, 0)))
            local tk = reaper.AddTakeToMediaItem(it)
            reaper.SetMediaItemTake_Source(tk, src)
            reaper.SetMediaItemInfo_Value(it, "D_POSITION", 1230.0 + 700.0 * (i - 1))
            reaper.SetMediaItemInfo_Value(it, "D_LENGTH", 600.0)
          end
          reaper.SelectAllMediaItems(0, true)
          reaper.UpdateArrange()
          return true""")
        ensure_window(sess)
        sess.eval(f"local h = SP_WINDOW() "
                  f'reaper.JS_WindowMessage_Post(h, "WM_COMMAND", {CM_TRACK_VIEW}, 0, 0, 0) return true')
        sess.wait_until(lambda: mode_from_capture(sess, SHOTS / f"f1_set{cycle}.png") == "SET", timeout=20)

        clear_project(sess)               # the set's items and track are gone
        # brand-new items on a brand-new track - freed MediaItem blocks get reused
        insert_item_unselected(sess, short)
        sess.eval("""
          local tr = reaper.GetTrack(0, 0)
          for i = 1, 3 do
            local it = reaper.AddMediaItemToTrack(tr)
            local src = reaper.GetMediaItemTake_Source(reaper.GetActiveTake(reaper.GetTrackMediaItem(tr, 0)))
            local tk = reaper.AddTakeToMediaItem(it)
            reaper.SetMediaItemTake_Source(tk, src)
            reaper.GetSetMediaItemTakeInfo_String(tk, "P_NAME", "short_tone.wav", true)
            reaper.SetMediaItemInfo_Value(it, "D_POSITION", 10.0 * i)
            reaper.SetMediaItemInfo_Value(it, "D_LENGTH", 8.0)
          end
          reaper.SelectAllMediaItems(0, false)
          reaper.UpdateArrange()
          return true""")
        ensure_window(sess)
        for idx in range(4):
            sess.eval(f"reaper.SelectAllMediaItems(0, false) "
                      f"local it = reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), {idx}) "
                      "reaper.SetMediaItemSelected(it, true) reaper.UpdateArrange() return true")
            try:
                wait_audio_loaded(sess, "short_tone", timeout=8)
            except Exception:
                raise AssertionError(f"cycle {cycle}, item {idx}: the new item never loaded - "
                                     f"the dead working set locked the view")
            mode = mode_from_capture(sess, SHOTS / f"f1_after{cycle}_{idx}.png")
            assert mode == "ITEM", f"cycle {cycle}, item {idx}: expected ITEM view, got {mode}"
