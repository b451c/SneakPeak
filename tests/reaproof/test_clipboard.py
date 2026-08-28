"""Copy / Cut / Paste durability (v2.5.0 audit, increment A2).

A2.1 - Cut deleted even when Copy was refused (over the 1 GB buffer cap,
unreadable audio): the selection was ripple-deleted and the clipboard stale.
A2.2 - Paste wrote its clip into the OS temp folder, which gets purged, so
a saved project lost the pasted media later; two pastes within one second
also overwrote each other's file. The clip now lives in the project's
recording path under a per-session counter. Control (cb48cd5): item cut /
clip under TMPDIR.
"""
from __future__ import annotations

import time
from pathlib import Path

from conftest import (SELECT_ITEM0, WAVE_Y, burst_fixture, clear_project, click_client,
                      drag_client, ensure_window, insert_item_unselected, perf_media_dir,
                      send_command, track_item_count, wait_audio_loaded, write_long_wav)

CM_CUT, CM_COPY, CM_PASTE, CM_SELECT_ALL = 2001, 2002, 2003, 2007   # edit_view.h enum ContextMenuID


def _last_toast(sess) -> str:
    return str(sess.eval('return reaper.GetExtState("SneakPeak", "last_toast")'))


def _item0_length(sess) -> float:
    return float(sess.eval("return reaper.GetMediaItemInfo_Value("
                           "reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), 0), 'D_LENGTH')"))


def _paste_sources(sess) -> list[str]:
    return [str(p) for p in sess.eval("""
      local out = {}
      local tr = reaper.GetTrack(0, 0)
      for i = 0, reaper.CountTrackMediaItems(tr) - 1 do
        local tk = reaper.GetActiveTake(reaper.GetTrackMediaItem(tr, i))
        local src = reaper.GetMediaItemTake_Source(tk)
        local path = reaper.GetMediaSourceFileName(src, "")
        if path:find("sneakpeak_paste") then out[#out + 1] = path end
      end
      return out""")]


def test_cut_refused_over_the_buffer_cap_keeps_the_item(sess):
    media = write_long_wav(perf_media_dir() / "long26min_stereo.wav", minutes=26)   # > 1 GB of doubles
    clear_project(sess)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=60)   # lazy: title only
    n0, len0 = track_item_count(sess), _item0_length(sess)
    sess.eval('reaper.DeleteExtState("SneakPeak", "last_toast", false)')
    send_command(sess, CM_SELECT_ALL)
    time.sleep(0.3)

    send_command(sess, CM_CUT)
    time.sleep(2.0)

    assert track_item_count(sess) == n0, "Cut deleted although nothing was copied"
    assert abs(_item0_length(sess) - len0) < 0.01, "Cut shortened the item although nothing was copied"
    toast = _last_toast(sess)
    assert "too long to copy" in toast, f"refusal toast missing: {toast!r}"


def test_paste_lands_in_the_project_recording_path(sess):
    media = burst_fixture("clip_30s.wav", seconds=30, channels=2)
    clear_project(sess)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=60)
    proj_dir = Path(str(sess.eval("return reaper.GetProjectPathEx(0)")))
    assert proj_dir.is_dir(), f"REAPER's recording path does not exist: {proj_dir}"

    drag_client(sess, 200, WAVE_Y, 225, WAVE_Y)      # ~1 s of the item
    time.sleep(0.3)
    send_command(sess, CM_COPY)
    time.sleep(0.5)
    click_client(sess, 600, WAVE_Y)                   # cursor deep in the item
    time.sleep(0.3)
    n0 = track_item_count(sess)
    send_command(sess, CM_PASTE)
    send_command(sess, CM_PASTE)                      # within the same second
    sess.wait_until(lambda: track_item_count(sess) >= n0 + 2, timeout=30)
    time.sleep(0.5)

    sources = _paste_sources(sess)
    assert len(sources) == 2, f"expected two pasted items, got {sources}"
    assert len(set(sources)) == 2, f"two pastes within a second share one media file: {sources}"
    for p in sources:
        assert Path(p).parent == proj_dir, f"pasted clip outside the project path: {p} (want {proj_dir})"
        assert Path(p).exists(), f"pasted clip missing on disk: {p}"
        assert media.stem in Path(p).name, f"clip name does not carry the source name: {p}"
