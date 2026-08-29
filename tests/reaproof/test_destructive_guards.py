"""Destructive-edit guards (v2.5.0 audit, increment A1).

A1.1 - the working buffer must never reach the disk. The ITEM buffer is
capped at 2 channels (and folded to 1 by the take's mono channel modes), yet
the Hard Limiter's ITEM apply wrote that BUFFER over the source file whenever
rate/offset/playrate/length matched: a 6-channel file came back stereo
(channels 3-6 gone, P0) and a stereo file with I_CHANMODE 2 came back mono
(P1). The whole-file path first refused those cases (2328139); since v2.5 F3
the limiter edits the file's own channels in place, so the specs check the
channel count survives AND the limit landed. Ground truth: REAPER's own
channel count for the source after the apply, the file's bytes (hash), the
file's samples, and the toast read back through SneakPeak/last_toast.
"""
from __future__ import annotations

import hashlib
import time
from pathlib import Path

import numpy as np
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


def _limited_peak_db(path: Path, t0: float, t1: float) -> float:
    with sf.SoundFile(str(path)) as f:
        f.seek(int(t0 * f.samplerate))
        y = f.read(int((t1 - t0) * f.samplerate), dtype="float64", always_2d=True)
    return float(20 * np.log10(max(np.abs(y).max(), 1e-9)))


def _apply_limiter_pushed(sess, tag: str) -> bool:
    """Apply with +6 dB of input gain (lim_* ExtState = milli-dB, restored on
    the panel open) so the burst has to be pulled down to the -1 dBTP ceiling."""
    sess.eval('reaper.SetExtState("SneakPeak", "lim_gain", "6000", true) return true')
    try:
        return _apply_limiter(sess, tag)
    finally:
        sess.eval('reaper.SetExtState("SneakPeak", "lim_gain", "0", true) return true')


def test_six_channel_item_limiter_keeps_all_six_channels_on_disk(sess):
    """v2.5 F3: the limiter edits the item's window of the FILE in place
    (WavInplace::Limit), so the 2-channel working buffer never reaches the
    disk: the 6-channel source stays 6-channel and every channel is limited.
    Control (2328139, whole-file write path): refused with a 'channels'
    toast; cb48cd5: rewritten as stereo."""
    clear_project(sess)
    media = burst_fixture("guard_6ch_30s.wav", seconds=30, channels=6)
    _load(sess, media)
    before = _source_info(sess)
    assert before["nch"] == 6, before
    sha0 = _sha(media)

    confirmed = _apply_limiter_pushed(sess, "six")

    toast = _last_toast(sess)
    assert confirmed, f"the limiter did not reach its confirmation (toast {toast!r})"
    assert _sha(media) != sha0, f"the file was not rewritten (toast {toast!r})"
    after = _source_info(sess)
    on_disk = sf.info(str(media)).channels
    assert on_disk == 6 and after["nch"] == 6, \
        f"the 6-channel source lost channels: file {on_disk} ch, REAPER reports {after['nch']}"
    pk = _limited_peak_db(media, 0.4, 1.6)
    assert pk <= -0.95, f"the burst still peaks at {pk:.2f} dBFS across the six channels (ceiling -1 dBTP)"
    assert toast.startswith("Limited"), f"result toast missing: {toast!r}"


def test_mono_channel_mode_item_limiter_keeps_the_stereo_file(sess):
    """Stereo source, take I_CHANMODE 2 (mono downmix): the buffer is one
    channel, the file has two - the in-place limit works on the file's two
    channels and the file stays stereo (control 2328139: refused; cb48cd5:
    folded to mono)."""
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

    confirmed = _apply_limiter_pushed(sess, "mono")

    toast = _last_toast(sess)
    assert confirmed, f"the limiter did not reach its confirmation (toast {toast!r})"
    assert _sha(media) != sha0, f"the file was not rewritten (toast {toast!r})"
    after = _source_info(sess)
    on_disk = sf.info(str(media)).channels
    assert on_disk == 2 and after["nch"] == 2, \
        f"the stereo source was folded: file {on_disk} ch, REAPER reports {after['nch']}"
    pk = _limited_peak_db(media, 0.4, 1.6)
    assert pk <= -0.95, f"the burst still peaks at {pk:.2f} dBFS (ceiling -1 dBTP)"


# --- A1.2: SECTION / reversed sources -----------------------------------------
REVERSE = ('reaper.defer(function() reaper.Main_OnCommand('
           'reaper.NamedCommandLookup("_SneakPeak_Reverse"), 0) end) return true')
TOGGLE_TAKE_REVERSE = 41051   # Item properties: Toggle take reverse (wraps the source in a SECTION)


def test_reverse_on_a_reversed_take_leaves_the_parent_file_alone(sess):
    """A take reversed in REAPER (or a section of a file) plays through a
    SECTION source whose parent is the WAV. SneakPeak resolved the path to the
    parent and edited the parent's region as if the take played it directly -
    on a reversed take that reversed the wrong audio in the file. Destructive
    edits on such takes are refused before the prompt; the parent's bytes and
    the reversed playback stay as they were. Control (cb48cd5): parent file
    hash differs after the prompt."""
    from conftest import db, track_rms_windows
    clear_project(sess)
    media = burst_fixture("guard_reversed_30s.wav", seconds=30, channels=2)
    insert_item_unselected(sess, media)
    sess.eval(SELECT_ITEM0)
    sess.eval(f"reaper.Main_OnCommand({TOGGLE_TAKE_REVERSE}, 0)")
    sess.eval("reaper.Main_OnCommand(40289, 0)")   # unselect all items
    parent = sess.eval("""
      local it = reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), 0)
      local src = reaper.GetMediaItemTake_Source(reaper.GetActiveTake(it))
      return reaper.GetMediaSourceParent(src) ~= nil""")
    assert parent, "precondition: the toggled take does not play through a SECTION source"
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=60)
    time.sleep(0.5)
    # reversed: the 0.5-1.5 s burst of the last 10 s block now sits at 28.5-29.5 s
    w_head, w_tail = (0.6, 1.4), (28.6, 29.4)
    head, tail = track_rms_windows(sess, [w_head, w_tail])
    assert db(tail) > db(head) + 20, f"precondition: take not reversed (head {db(head):.1f} tail {db(tail):.1f})"
    sha0 = _sha(media)
    sess.eval('reaper.DeleteExtState("SneakPeak", "last_toast", false)')
    SHOTS.mkdir(parents=True, exist_ok=True)

    sess.eval(REVERSE)
    time.sleep(0.4)
    capture(sess, SHOTS / "reversed_1_pressed.png")
    confirmed = dismiss_native_modal(sess, timeout=6)
    wait_main_thread_idle(sess, timeout=120)
    time.sleep(1.0)

    assert _sha(media) == sha0, "the parent file's bytes changed"
    assert not confirmed, "the destructive prompt appeared before the source check"
    head, tail = track_rms_windows(sess, [w_head, w_tail])
    assert db(tail) > db(head) + 20, f"playback no longer reversed (head {db(head):.1f} tail {db(tail):.1f})"
    toast = _last_toast(sess)
    assert "section or reversed" in toast, f"refusal toast missing: {toast!r}"


# --- A1.3: the pre-edit snapshot must exist before the file is touched ---------
def test_snapshot_failure_cancels_the_edit(sess):
    """UndoSave copied the source into the temp dir and, when that copy failed
    (temp dir missing, disk full), logged it and let the edit go ahead with no
    way back. With the temp dir pointed at a directory that does not exist
    the edit must be cancelled after the prompt: file bytes unchanged, a
    message naming the copy failure. Own REAPER session (the env is read at
    launch). Control (cb48cd5): file reversed, no undo."""
    import os
    import sys
    from conftest import DYLIB, SP_WINDOW_LUA, db, track_rms_windows
    from reaproof.runner.session import ReaperSession
    if sys.platform == "win32":
        names, bad = ("TMP", "TEMP"), r"C:\nonexistent\sneakpeak"
    else:
        names, bad = ("TMPDIR",), "/nonexistent/sneakpeak"
    # Windows: SP_WINDOW() sees every process's windows, so the module session's
    # visible SneakPeak window would answer window_visible() for the fresh
    # session below and hide_window() would wait forever. Hide it first.
    from conftest import hide_window
    hide_window(sess)
    saved = {n: os.environ.get(n) for n in names}
    for n in names:
        os.environ[n] = bad
    try:
        s = ReaperSession("sneakpeak-badtmp", extensions=[DYLIB]).start()
    finally:
        for n, v in saved.items():
            if v is None:
                os.environ.pop(n, None)
            else:
                os.environ[n] = v
    try:
        s.eval(SP_WINDOW_LUA)
        clear_project(s)
        media = burst_fixture("guard_badtmp_30s.wav", seconds=30, channels=2)
        insert_item_unselected(s, media)
        ensure_window(s)
        s.eval(SELECT_ITEM0)
        wait_audio_loaded(s, media.stem, timeout=60)
        time.sleep(0.5)
        sha0 = _sha(media)
        w_head, w_tail = (0.6, 1.4), (28.6, 29.4)
        head, tail = track_rms_windows(s, [w_head, w_tail])
        assert db(head) > db(tail) + 20, "precondition: burst not at the head"
        s.eval('reaper.DeleteExtState("SneakPeak", "last_toast", false)')
        SHOTS.mkdir(parents=True, exist_ok=True)

        s.eval(REVERSE)
        assert dismiss_native_modal(s, timeout=8), "the Reverse confirmation never appeared"
        time.sleep(0.4)
        capture(s, SHOTS / "badtmp_1_after_prompt.png")
        wait_main_thread_idle(s, timeout=120)
        time.sleep(1.0)

        assert _sha(media) == sha0, "the file was edited without a pre-edit copy"
        head, tail = track_rms_windows(s, [w_head, w_tail])
        assert db(head) > db(tail) + 20, f"playback changed (head {db(head):.1f} tail {db(tail):.1f})"
        toast = _last_toast(s)
        assert "pre-edit copy" in toast and "cancelled" in toast, f"cancel toast missing: {toast!r}"
    finally:
        s.stop()


# --- A1.4: a write that fails part-way is rolled back -------------------------
CM_SELECT_ALL, CM_GAIN_UP = 2007, 2013   # edit_view.h enum ContextMenuID (compiled 2026-08-28)
GAIN_UP = ('reaper.defer(function() reaper.JS_WindowMessage_Send(SP_WINDOW(), "WM_COMMAND", '
           f'{CM_GAIN_UP}, 0, 0, 0) end) return true')


def _claim_double_data(path: Path):
    """Patch the RIFF and data chunk sizes to twice the real sample bytes - a
    truncated WAV (interrupted copy, disk that filled up). The in-place
    editors size the edit from the header and hit EOF half-way through."""
    b = bytearray(path.read_bytes())
    pos = 12
    while pos + 8 <= len(b):
        cid, size = bytes(b[pos:pos + 4]), int.from_bytes(b[pos + 4:pos + 8], "little")
        if cid == b"data":
            real = len(b) - (pos + 8)
            b[pos + 4:pos + 8] = (real * 2).to_bytes(4, "little")
            b[4:8] = (len(b) - 8 + real).to_bytes(4, "little")
            path.write_bytes(b)
            return
        pos += 8 + size + (size & 1)
    raise AssertionError("no data chunk")


def test_partial_write_rolls_back_to_the_pre_edit_copy(sess):
    """Gain on a selection edits the file chunk by chunk; when a read fails
    half-way (the data chunk claims twice the bytes the file holds, and the
    item is extended over the claimed length) the first half is already
    rewritten. The failed write must put the pre-edit copy back: bytes
    identical to before, a message saying so. Control (cb48cd5): bytes differ
    (first half gained) behind a "Failed to write WAV file" box."""
    clear_project(sess)
    media = burst_fixture("guard_truncated_30s.wav", seconds=30, channels=2)
    _claim_double_data(media)
    insert_item_unselected(sess, media)
    sess.eval("""
      local it = reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), 0)
      reaper.SetMediaItemLength(it, 60.0, false)
      reaper.UpdateItemInProject(it)""")
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=90)
    time.sleep(0.5)
    sha0 = _sha(media)
    sess.eval('reaper.DeleteExtState("SneakPeak", "last_toast", false)')
    SHOTS.mkdir(parents=True, exist_ok=True)
    send_command(sess, CM_SELECT_ALL)
    time.sleep(0.3)

    sess.eval(GAIN_UP)
    assert dismiss_native_modal(sess, timeout=6), "the Gain confirmation never appeared (F1)"
    time.sleep(0.6)
    capture(sess, SHOTS / "truncated_1_pressed.png")
    modal = dismiss_native_modal(sess, timeout=6)
    wait_main_thread_idle(sess, timeout=120)
    time.sleep(1.0)

    assert _sha(media) == sha0, "the half-written file was not restored"
    assert not modal, "an error box appeared although the file was restored"
    toast = _last_toast(sess)
    assert "restored from the pre-edit copy" in toast, f"rollback toast missing: {toast!r}"
