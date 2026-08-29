"""Destructive ITEM edits on long items must not degrade the file.

SneakPeak keeps long items in a DOWNSAMPLED working buffer (10M-frame cap:
anything over ~3.8 min at 44.1k stereo). A destructive op that writes that
buffer back would silently re-encode the source at the reduced rate.
Ground truth: REAPER's own view of the source after the edit (sample rate,
bit depth, length) plus the reversal itself via the track audio accessor.
"""
from __future__ import annotations

import time

from conftest import (SELECT_ITEM0, burst_fixture, clear_project, db, dismiss_native_modal,
                      ensure_window, insert_item_unselected, send_command, track_rms_windows,
                      wait_audio_loaded, wait_main_thread_idle, window_title)

MINUTES = 5
CM_UNDO = 2000        # edit_view.h enum ContextMenuID (verified 2026-08-28)
CM_DC_REMOVE = 2015

REVERSE = ('reaper.defer(function() reaper.Main_OnCommand('
           'reaper.NamedCommandLookup("_SneakPeak_Reverse"), 0) end) return true')
# A menu command fired SYNCHRONOUSLY from inside the defer loop: the native
# confirmation then blocks that loop (heartbeat stalls) exactly like the action
# above. A JS_WindowMessage_Post would be handled by the window's own message
# pump, where the modal's nested run loop keeps REAPER's timers ticking and
# dismiss_native_modal never sees the stall.
DC_REMOVE = ('reaper.defer(function() reaper.JS_WindowMessage_Send(SP_WINDOW(), "WM_COMMAND", '
             f'{CM_DC_REMOVE}, 0, 0, 0) end) return true')


def _source_info(sess) -> dict:
    return sess.eval("""
      local it = reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), 0)
      local src = reaper.GetMediaItemTake_Source(reaper.GetActiveTake(it))
      local len = reaper.GetMediaSourceLength(src)
      return { sr = reaper.GetMediaSourceSampleRate(src), bits = reaper.CF_GetMediaSourceBitDepth(src),
               len = len, nch = reaper.GetMediaSourceNumChannels(src) }""", hang_timeout=120)


def _track_means(sess, windows) -> list[float]:
    wins = ", ".join(f"{{{a}, {b}}}" for a, b in windows)
    return [float(x) for x in sess.eval(f"""
      local acc = reaper.CreateTrackAudioAccessor(reaper.GetTrack(0, 0))
      local out = {{}}
      for _, w in ipairs({{ {wins} }}) do
        local n = math.floor((w[2] - w[1]) * 44100) local buf = reaper.new_array(n) buf.clear()
        reaper.GetAudioAccessorSamples(acc, 44100, 1, w[1], n, buf)
        local a = 0 for i = 1, n do a = a + buf[i] end out[#out + 1] = a / n
      end
      reaper.DestroyAudioAccessor(acc) return out""", hang_timeout=120)]


def _load_five_minute(sess, name="long5min_burst24.wav", **fixture_kw):
    clear_project(sess)
    media = burst_fixture(name, seconds=MINUTES * 60, channels=2, **fixture_kw)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=60)
    return media


def _run_destructive(sess, fire, timeout=240):
    fire()
    assert dismiss_native_modal(sess), "the destructive confirmation never appeared"
    # F5: the write runs on a worker, so the main thread is idle at once - wait
    # for the job's title ("... N% (Esc cancels)", "... saving the pre-edit
    # copy") to clear; on Windows the item is offline until it does.
    sess.wait_until(lambda: "..." not in window_title(sess), timeout=timeout)
    wait_main_thread_idle(sess, timeout=timeout)
    time.sleep(1.0)


def _assert_format_kept(before, after):
    assert after["sr"] == before["sr"], f"destructive write changed the sample rate: {before} -> {after}"
    assert after["bits"] == before["bits"], f"destructive write changed the bit depth: {before} -> {after}"
    assert abs(after["len"] - before["len"]) < 0.01, f"length changed: {before} -> {after}"


def test_reverse_on_a_five_minute_item_keeps_rate_depth_and_length(sess):
    _load_five_minute(sess)
    before = _source_info(sess)
    assert before["sr"] == 44100 and before["bits"] == 24, before
    total = MINUTES * 60.0
    w_head, w_tail = (0.6, 1.4), (total - 1.4, total - 0.6)

    _run_destructive(sess, lambda: sess.eval(REVERSE))

    after = _source_info(sess)
    _assert_format_kept(before, after)
    head, tail = track_rms_windows(sess, [w_head, w_tail])
    assert db(tail) > db(head) + 20, f"the item was not reversed: head {db(head):.1f} tail {db(tail):.1f}"


def test_undo_after_reverse_on_a_five_minute_item_restores_the_original(sess):
    """The single-level destructive undo must put the ORIGINAL bytes back - not a
    write of the (downsampled) working buffer."""
    _load_five_minute(sess)
    before = _source_info(sess)
    total = MINUTES * 60.0
    w_head, w_tail = (0.6, 1.4), (total - 1.4, total - 0.6)
    _run_destructive(sess, lambda: sess.eval(REVERSE))
    head, tail = track_rms_windows(sess, [w_head, w_tail])
    assert db(tail) > db(head) + 20, "precondition: the reverse itself did not land"

    send_command(sess, CM_UNDO)
    wait_main_thread_idle(sess, timeout=240)
    wait_audio_loaded(sess, "long5min_burst24", timeout=120)
    time.sleep(1.0)

    after = _source_info(sess)
    _assert_format_kept(before, after)
    head, tail = track_rms_windows(sess, [w_head, w_tail])
    assert db(head) > db(tail) + 20, f"undo did not restore the original: head {db(head):.1f} tail {db(tail):.1f}"


def test_dc_remove_on_a_five_minute_item_streams_at_the_file_rate(sess):
    _load_five_minute(sess, name="long5min_burst24_dc.wav", dc=0.1)   # pristine cache is keyed by name
    before = _source_info(sess)
    total = MINUTES * 60.0
    windows = [(2.0, 3.0), (total - 3.0, total - 2.0)]
    m0 = _track_means(sess, windows)
    assert all(m > 0.08 for m in m0), f"fixture: expected a +0.1 offset, got {m0}"

    _run_destructive(sess, lambda: sess.eval(DC_REMOVE))

    after = _source_info(sess)
    _assert_format_kept(before, after)
    m1 = _track_means(sess, windows)
    assert all(abs(m) < 0.005 for m in m1), f"DC offset survived: {m0} -> {m1}"


def test_reverse_on_a_short_item_is_audible_in_reaper(sess):
    """F7 isolated from the length question: after a destructive Reverse the
    NEW audio must be what REAPER serves for the take (take + track accessor),
    not the pre-edit file pinned by an open decoder."""
    clear_project(sess)
    media = burst_fixture("short_burst24.wav", seconds=10, channels=2)
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


def test_reverse_on_a_trimmed_item_edits_only_its_window_of_the_file(sess):
    """F12: an item that shows the first 5 s of a 10 s file. The reverse must
    turn exactly those 5 s around inside the file and leave the rest (and the
    file's length) alone - the buffer path wrote the window back as the whole
    file, truncating the source."""
    import numpy as np, soundfile as sf
    clear_project(sess)
    media = burst_fixture("short_burst24.wav", seconds=10, channels=2)
    pristine, sr = sf.read(str(media.parent / "pristine" / media.name), dtype="float64")
    insert_item_unselected(sess, media)
    sess.eval('local it = reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), 0) '
              'reaper.SetMediaItemInfo_Value(it, "D_LENGTH", 5.0) reaper.UpdateArrange() return true')
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=30)

    _run_destructive(sess, lambda: sess.eval(REVERSE), timeout=60)

    edited, sr2 = sf.read(str(media), dtype="float64")
    assert sr2 == sr and edited.shape == pristine.shape, f"file shape changed: {pristine.shape} -> {edited.shape}"
    n = 5 * sr
    assert np.allclose(edited[n:], pristine[n:], atol=1e-6), "audio outside the item's window was modified"
    assert np.allclose(edited[:n], pristine[:n][::-1], atol=2e-6), "the item's window was not reversed in the file"
    # and REAPER hears it: the burst (0.5-1.5 s in the file) now sits at 3.5-4.5 s
    head, tail = track_rms_windows(sess, [(0.6, 1.4), (3.6, 4.4)])
    assert db(tail) > db(head) + 20, f"REAPER does not serve the reversed window: head {db(head):.1f} tail {db(tail):.1f}"


# --------------------------------------------------------------------------
# F5: the edit runs in the background - REAPER stays responsive, the title
# shows progress, Esc cancels and restores the pre-edit copy, the edited item
# is pinned until the write ends (control 621d560: synchronous, RED on 1-2).
# --------------------------------------------------------------------------
MINUTES20 = 20
STALL_BUDGET = 0.25       # s - anything longer reads as a hang
VK_ESCAPE = 0x1B


def _load_twenty_minute(sess, name):
    """A fresh 24-bit working copy per call (it is edited); the burst at
    0.5-1.5 s is the reversal oracle, pristine/<name> the byte oracle."""
    clear_project(sess)
    media = burst_fixture(name, seconds=MINUTES20 * 60, channels=2)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=60)
    return media


def _sha(path) -> str:
    import hashlib
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _assert_reversed(sess, total: float):
    head, tail = track_rms_windows(sess, [(0.6, 1.4), (total - 1.4, total - 0.6)])
    assert db(tail) > db(head) + 20, f"the item was not reversed: head {db(head):.1f} tail {db(tail):.1f}"


def test_reverse_on_a_twenty_minute_item_keeps_reaper_responsive(sess):
    from conftest import measure_after_modal
    media = _load_twenty_minute(sess, "long20min_burst24.wav")
    before = _source_info(sess)

    m = measure_after_modal(sess, REVERSE, idle_marker=media.stem, progress_marker="Reversing", max_wait=180)
    print(f"\n[perf] reverse 20 min: {m}")

    assert m["progress"], "the title never reported progress - the write ran on the main thread"
    assert any("%" in t for t in m["progress"]), f"no percent in the progress titles: {m['progress']}"
    assert m["max_stall"] < STALL_BUDGET, \
        f"REAPER's main thread froze for {m['max_stall']} s during the reverse (budget {STALL_BUDGET})"
    wait_main_thread_idle(sess, timeout=120)
    time.sleep(1.0)
    _assert_format_kept(before, _source_info(sess))
    _assert_reversed(sess, MINUTES20 * 60.0)


def test_escape_cancels_a_background_reverse_and_restores_the_file(sess):
    from conftest import measure_after_modal
    media = _load_twenty_minute(sess, "long20min_burst24_esc.wav")
    pristine = media.parent / "pristine" / media.name
    sha0 = _sha(pristine)
    sess.eval('reaper.DeleteExtState("SneakPeak", "last_toast", false)')

    # Esc posted from REAPER's own loop on the first percent tick (a bridge
    # round trip lands after a fast disk has finished the whole edit).
    m = measure_after_modal(sess, REVERSE, idle_marker=media.stem, progress_marker="Reversing", max_wait=180,
                            on_progress_lua=f'reaper.JS_WindowMessage_Post(h, "WM_KEYDOWN", {VK_ESCAPE}, 0, 0, 0) '
                                            f'reaper.JS_WindowMessage_Post(h, "WM_KEYUP", {VK_ESCAPE}, 0, 0, 0)')
    print(f"\n[perf] reverse 20 min + Esc: {m}")
    wait_main_thread_idle(sess, timeout=120)
    time.sleep(1.0)

    assert m["fired"], "the watcher never saw a percent in the title"
    assert _sha(media) == sha0, "Esc did not restore the pre-edit file byte for byte"
    toast = str(sess.eval('return reaper.GetExtState("SneakPeak", "last_toast")'))
    assert "cancelled" in toast, f"no cancel toast: {toast!r}"
    head, tail = track_rms_windows(sess, [(0.6, 1.4), (MINUTES20 * 60.0 - 1.4, MINUTES20 * 60.0 - 0.6)])
    assert db(head) > db(tail) + 20, f"REAPER does not serve the original: head {db(head):.1f} tail {db(tail):.1f}"


def test_selecting_another_item_during_a_background_reverse(sess):
    """The view stays on the item being rewritten (its finalize needs it), then
    follows REAPER's selection once the write has landed."""
    from conftest import measure_after_modal, window_title
    clear_project(sess)
    media = burst_fixture("long20min_burst24_sel.wav", seconds=MINUTES20 * 60, channels=2)
    other = burst_fixture("short_burst24_other.wav", seconds=10, channels=2)
    # Both items BEFORE the first select (an insert re-creates the window), and
    # `other` FIRST: every insert_item adds a new track 0, so the item under
    # edit must be the last one in to sit on track 0 (SELECT_ITEM0, the oracle).
    insert_item_unselected(sess, other)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=60)
    before = _source_info(sess)

    m = measure_after_modal(sess, REVERSE, idle_marker="SneakPeak", progress_marker="Reversing", max_wait=180,
                            on_progress_lua="reaper.SetMediaItemSelected(reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), 0), false) "
                                            "reaper.SetMediaItemSelected(reaper.GetTrackMediaItem(reaper.GetTrack(0, 1), 0), true) "
                                            "reaper.UpdateArrange()")
    print(f"\n[perf] reverse 20 min + select: {m}")
    wait_main_thread_idle(sess, timeout=120)
    time.sleep(1.0)

    assert m["fired"], "the watcher never saw a percent in the title"
    _assert_format_kept(before, _source_info(sess))
    _assert_reversed(sess, MINUTES20 * 60.0)
    assert other.stem in window_title(sess), "the view did not follow the selection after the write"
