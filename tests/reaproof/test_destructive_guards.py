"""Destructive-edit guards (v2.5.0 audit, increment A1).

A1.1 - whole-file writes must refuse a channel-count mismatch. The ITEM
working buffer is capped at 2 channels (and folded to 1 by the take's mono
channel modes), yet the Hard Limiter's ITEM apply wrote that BUFFER over the
source file whenever rate/offset/playrate/length matched: a 6-channel file
came back stereo (channels 3-6 gone, P0) and a stereo file with I_CHANMODE 2
came back mono (P1). Ground truth: REAPER's own channel count for the source
after the apply, the file's bytes (hash), and the refusal toast read back
through the SneakPeak/last_toast ExtState probe. Control (cb48cd5): nch 2 / 1.
"""
from __future__ import annotations

import hashlib
import time
from pathlib import Path

import soundfile as sf

from conftest import (SELECT_ITEM0, burst_fixture, capture, clear_project,
                      dismiss_native_modal, ensure_window, insert_item_unselected,
                      locate_apply_button, send_command, wait_audio_loaded,
                      wait_main_thread_idle)

CM_APPLY_LIMITER = 2176   # edit_view.h enum ContextMenuID (compiled 2026-08-28)
SHOTS = Path("/tmp/sneakpeak-reaproof-shots/destructive_guards")


def _source_info(sess) -> dict:
    return sess.eval("""
      local it = reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), 0)
      local src = reaper.GetMediaItemTake_Source(reaper.GetActiveTake(it))
      return { sr = reaper.GetMediaSourceSampleRate(src), nch = reaper.GetMediaSourceNumChannels(src),
               len = reaper.GetMediaSourceLength(src) }""", hang_timeout=120)


def _sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _last_toast(sess) -> str:
    return str(sess.eval('return reaper.GetExtState("SneakPeak", "last_toast")'))


def _load(sess, media: Path):
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=60)
    time.sleep(0.5)


def _apply_limiter(sess, tag: str) -> bool:
    """Open the HARD LIMITER panel (context-menu command) and press its Apply
    button - the real user path. Returns whether the destructive confirmation
    appeared (it is answered Yes, so an unguarded build really writes)."""
    SHOTS.mkdir(parents=True, exist_ok=True)
    sess.eval('reaper.DeleteExtState("SneakPeak", "last_toast", false)')
    sess.eval('reaper.DeleteExtState("SneakPeakSpec", "press", false)')
    capture(sess, SHOTS / f"{tag}_0_loaded.png")
    send_command(sess, CM_APPLY_LIMITER)
    sess.wait_until(lambda: locate_apply_button(sess, SHOTS / f"{tag}_1_panel.png") is not None,
                    timeout=8)
    x, y = locate_apply_button(sess, SHOTS / f"{tag}_1_panel.png")
    print(f"\n[guards] {tag}: Apply at client ({x}, {y})")
    # The press is SENT from inside the defer loop, not posted: the native
    # confirmation then blocks that loop, which is the heartbeat stall
    # dismiss_native_modal keys on. A posted click is dispatched by the
    # window's own pump, where the modal's nested run loop keeps REAPER's
    # timers ticking and the prompt is never seen (see test_destructive_long).
    sess.eval(f"""reaper.defer(function()
        local h = SP_WINDOW()   -- never JS_Window_Find: a dismissed MessageBox lingers first (F9)
        local t0 = reaper.time_precise()
        local r = reaper.JS_WindowMessage_Send(h, "WM_LBUTTONDOWN", 1, 0, {x}, {y})
        reaper.JS_WindowMessage_Send(h, "WM_LBUTTONUP", 0, 0, {x}, {y})
        reaper.SetExtState("SneakPeakSpec", "press", string.format("win=%s ret=%s held=%.2fs",
          h and reaper.JS_Window_GetTitle(h) or "nil", tostring(r), reaper.time_precise() - t0), false)
      end) return true""")
    time.sleep(0.4)
    capture(sess, SHOTS / f"{tag}_2_pressed.png")   # a refusal toast is still solid here
    confirmed = dismiss_native_modal(sess, timeout=6)
    capture(sess, SHOTS / f"{tag}_3_after_press.png")
    wait_main_thread_idle(sess, timeout=120)
    time.sleep(1.0)
    capture(sess, SHOTS / f"{tag}_4_idle.png")
    press = sess.eval('return reaper.GetExtState("SneakPeakSpec", "press")')
    print(f"[guards] {tag}: press {press} confirmed={confirmed}")
    return confirmed


def test_six_channel_item_limiter_leaves_the_file_alone(sess):
    clear_project(sess)
    media = burst_fixture("guard_6ch_30s.wav", seconds=30, channels=6)
    _load(sess, media)
    before = _source_info(sess)
    assert before["nch"] == 6, before
    sha0 = _sha(media)

    confirmed = _apply_limiter(sess, "six")

    after = _source_info(sess)
    on_disk = sf.info(str(media)).channels
    assert on_disk == 6 and after["nch"] == 6, \
        f"the 6-channel source was rewritten: file {on_disk} ch, REAPER reports {after['nch']}"
    assert _sha(media) == sha0, "the source file's bytes changed"
    assert not confirmed, "the destructive prompt appeared before the eligibility check"
    toast = _last_toast(sess)
    assert "channels" in toast and "not changed" in toast, f"refusal toast missing: {toast!r}"


def test_mono_channel_mode_item_limiter_leaves_the_file_alone(sess):
    """Stereo source, take I_CHANMODE 2 (mono downmix): the buffer is one
    channel, the file has two - the apply must refuse, not fold the file."""
    clear_project(sess)
    media = burst_fixture("guard_stereo_chanmode_30s.wav", seconds=30, channels=2)
    insert_item_unselected(sess, media)
    sess.eval("""
      local it = reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), 0)
      reaper.SetMediaItemTakeInfo_Value(reaper.GetActiveTake(it), "I_CHANMODE", 2)
      reaper.UpdateItemInProject(it)""")
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=60)
    time.sleep(0.5)
    before = _source_info(sess)
    assert before["nch"] == 2, before
    sha0 = _sha(media)

    confirmed = _apply_limiter(sess, "mono")

    after = _source_info(sess)
    on_disk = sf.info(str(media)).channels
    assert on_disk == 2 and after["nch"] == 2, \
        f"the stereo source was rewritten: file {on_disk} ch, REAPER reports {after['nch']}"
    assert _sha(media) == sha0, "the source file's bytes changed"
    assert not confirmed, "the destructive prompt appeared before the eligibility check"
    toast = _last_toast(sess)
    assert "channels" in toast and "not changed" in toast, f"refusal toast missing: {toast!r}"
