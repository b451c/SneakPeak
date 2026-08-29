"""Standalone safety (v2.5.0 audit, increment A4).

Files enter Standalone through the new scriptable action
`_SneakPeak_OpenStandalone` (path in ExtState SneakPeak/open_path) - the
drag-drop route is not scriptable. A4.1 - a >2-channel file was silently
loaded as stereo and Save wrote it back with channels 3+ gone. A4.2 - a
file over the 1 GB buffer cap was allocated anyway (or overflowed the frame
math). A4.4 - Paste in Standalone took no undo snapshot and touched a null
item. A4.5 - One-Shot LUFS-I on a PCM Standalone file clipped instead of
limiting. A4.6 - One-Shot REGIONS mode in Standalone sliced by a project the
file is not in. Control (cb48cd5): loaded as stereo / loads or crashes /
undo does nothing / clipped run / regions run.
"""
from __future__ import annotations

import time
from pathlib import Path

import numpy as np
import soundfile as sf

from conftest import (burst_fixture, clear_project, ensure_window, mode_from_capture,
                      perf_media_dir, rss_mb, send_command, wait_audio_loaded,
                      wait_main_thread_idle, write_long_wav)
from conftest import command_sync as _command_sync, key_sync as _key_sync, send_sync as _send_sync

CM_UNDO, CM_PASTE, CM_COPY, CM_SELECT_ALL = 2000, 2003, 2002, 2007   # edit_view.h ContextMenuID
SHOTS = Path("/tmp/sneakpeak-reaproof-shots/standalone_guards")

OPEN = ('reaper.defer(function() reaper.Main_OnCommand('
        'reaper.NamedCommandLookup("_SneakPeak_OpenStandalone"), 0) end) return true')


def _last_toast(sess) -> str:
    return str(sess.eval('return reaper.GetExtState("SneakPeak", "last_toast")'))


def _open_standalone(sess, path: Path):
    sess.eval('reaper.DeleteExtState("SneakPeak", "last_toast", false)')
    sess.eval(f'reaper.SetExtState("SneakPeak", "open_path", "{path.as_posix()}", false) return true')
    sess.eval(OPEN)
    time.sleep(1.0)


def _mode(sess) -> str:
    SHOTS.mkdir(parents=True, exist_ok=True)
    return mode_from_capture(sess, SHOTS / "mode.png")


def test_standalone_refuses_a_multichannel_file(sess):
    clear_project(sess)
    ensure_window(sess)
    media = burst_fixture("sa_6ch_10s.wav", seconds=10, channels=6)
    _open_standalone(sess, media)
    time.sleep(1.5)
    mode = _mode(sess)
    toast = _last_toast(sess)
    assert mode != "STANDALONE", f"a 6-channel file was loaded into Standalone (mode {mode})"
    assert "mono and stereo" in toast and "6 channels" in toast, f"refusal toast missing: {toast!r}"
    assert sf.info(str(media)).channels == 6


def test_standalone_refuses_a_file_over_the_buffer_cap(sess):
    """65 min mono 16-bit at 44.1k = 340 MB on disk, 1.37 GB of doubles."""
    media = write_long_wav(perf_media_dir() / "sa_long65min_mono.wav", minutes=65, channels=1)
    clear_project(sess)
    ensure_window(sess)
    rss0 = rss_mb(sess)
    _open_standalone(sess, media)
    time.sleep(3.0)
    wait_main_thread_idle(sess, timeout=60)
    mode = _mode(sess)
    delta = rss_mb(sess) - rss0
    toast = _last_toast(sess)
    print(f"\n[standalone] over-cap: mode {mode}, rss delta {delta:.0f} MB, toast {toast!r}")
    assert mode != "STANDALONE", "an over-cap file was loaded into Standalone"
    assert delta < 50, f"the over-cap file allocated its buffer anyway (+{delta:.0f} MB)"
    assert "min max" in toast, f"cap toast missing: {toast!r}"


# --- A4.4: Standalone edits keep their own undo ------------------------------
VK_SPACE, VK_ESCAPE = 0x20, 0x1B
REVERSE = ('reaper.defer(function() reaper.Main_OnCommand('
           'reaper.NamedCommandLookup("_SneakPeak_Reverse"), 0) end) return true')
PASTE = ('reaper.defer(function() reaper.JS_WindowMessage_Send(SP_WINDOW(), "WM_COMMAND", '
         f'{CM_PASTE}, 0, 0, 0) end) return true')


def _preview_of_whole_buffer(sess) -> np.ndarray:
    """Standalone Play writes the selection (or the tail from the cursor) as a
    temp WAV before playing it - select all first, so the file IS the buffer."""
    import tempfile
    preview = Path(tempfile.gettempdir()) / f"sneakpeak_preview_{sess.handle.pid}.wav"
    preview.unlink(missing_ok=True)
    _command_sync(sess, CM_SELECT_ALL, settle=0.3)
    _key_sync(sess, VK_SPACE, settle=0.2)
    try:
        sess.wait_until(preview.exists, timeout=10)
    except Exception:
        from conftest import window_title
        from conftest import capture
        capture(sess, SHOTS / "preview_fail.png")
        print(f"\n[preview] no file: title {window_title(sess)!r} toast {_last_toast(sess)!r} "
              f"playstate {sess.eval('return reaper.GetPlayState()')} mode {_mode(sess)}")
        raise AssertionError("Standalone Play wrote no preview after the edit - the edit left "
                             "Standalone unable to play its own buffer")
    time.sleep(0.5)
    _key_sync(sess, VK_SPACE, settle=0.3)     # stop
    got = sf.read(str(preview), dtype="float64", always_2d=True)[0]
    _key_sync(sess, VK_ESCAPE, settle=0.2)    # clear the selection again
    return got


def _open_and_wait(sess, media: Path):
    clear_project(sess)
    ensure_window(sess)
    _open_standalone(sess, media)
    wait_audio_loaded(sess, media.name, timeout=60)
    time.sleep(0.5)
    assert _mode(sess) == "STANDALONE", "the fixture did not open in Standalone"


def test_standalone_reverse_undo_restores_the_buffer(sess):
    """Reverse in Standalone went down the ITEM path: a 'modifies the file on
    disk' prompt, no Standalone undo entry (Ctrl+Z did nothing) and no dirty
    flag. Control (cb48cd5): prompt appears, undo leaves the buffer reversed."""
    from conftest import dismiss_native_modal
    media = burst_fixture("sa_reverse_10s.wav", seconds=10, channels=2)
    want = sf.read(str(media), dtype="float64", always_2d=True)[0]
    _open_and_wait(sess, media)

    sess.eval(REVERSE)
    prompted = dismiss_native_modal(sess, timeout=5)
    wait_main_thread_idle(sess, timeout=60)
    rev = _preview_of_whole_buffer(sess)
    assert np.abs(rev[:4410] - want[::-1][:4410]).max() < 1e-4, "precondition: the buffer was not reversed"
    _command_sync(sess, CM_UNDO)
    wait_main_thread_idle(sess, timeout=60)
    time.sleep(0.5)

    got = _preview_of_whole_buffer(sess)
    assert not prompted, "Standalone Reverse asked about a file on disk (ITEM path)"
    assert len(got) == len(want), f"undo left {len(got)} frames, expected {len(want)}"
    assert np.abs(got - want).max() < 1e-4, "undo did not restore the pre-reverse buffer"


def test_standalone_paste_undo_restores_the_buffer(sess):
    """Paste in Standalone took the ITEM undo path (no Standalone snapshot) and
    touched a null item. Control (cb48cd5): after Ctrl+Z the buffer still
    holds the pasted second."""
    from conftest import WAVE_Y, click_client, dismiss_native_modal, drag_client
    media = burst_fixture("sa_paste_10s.wav", seconds=10, channels=2)
    want = sf.read(str(media), dtype="float64", always_2d=True)[0]
    _open_and_wait(sess, media)

    drag_client(sess, 200, WAVE_Y, 275, WAVE_Y)     # ~1 s of a 10 s file
    time.sleep(0.3)
    send_command(sess, CM_COPY)
    time.sleep(0.5)
    click_client(sess, 500, WAVE_Y)                  # cursor
    time.sleep(0.3)
    sess.eval(PASTE)
    prompted = dismiss_native_modal(sess, timeout=5)
    wait_main_thread_idle(sess, timeout=60)
    pasted = _preview_of_whole_buffer(sess)
    assert len(pasted) > len(want) + 4000, f"precondition: paste did not lengthen the buffer ({len(pasted)} vs {len(want)})"
    _command_sync(sess, CM_UNDO)
    wait_main_thread_idle(sess, timeout=60)
    time.sleep(0.5)

    got = _preview_of_whole_buffer(sess)
    assert not prompted, "Standalone Paste asked about a file on disk (ITEM path)"
    assert len(got) == len(want), f"undo left {len(got)} frames, expected {len(want)}"
    assert np.abs(got - want).max() < 1e-4, "undo did not restore the pre-paste buffer"


# --- A4.5 / A4.6: One-Shot Factory in Standalone -------------------------------
CM_ONESHOT_FACTORY = 2256


def _run_one_shot(sess, settings: dict):
    from conftest import locate_apply_button   # Dynamics stays closed: Run is the only amber blob
    # RestoreOneShotParams only reads the os_* keys when os_trim_thr exists
    # ("first run: keep the plan defaults"): always seed the whole set.
    full = {"os_trim_thr": "-60000", "os_pad": "0", **settings}
    lua = " ".join(f'reaper.SetExtState("SneakPeak", "{k}", "{v}", false)' for k, v in full.items())
    sess.eval(lua + " return true")
    SHOTS.mkdir(parents=True, exist_ok=True)
    sess.eval('reaper.DeleteExtState("SneakPeak", "last_toast", false)')
    _command_sync(sess, CM_ONESHOT_FACTORY, settle=1.0)
    try:
        sess.wait_until(lambda: locate_apply_button(sess, SHOTS / "oneshot_panel.png") is not None, timeout=10)
    except Exception:
        from conftest import window_title
        print(f"\n[oneshot] panel not found: title {window_title(sess)!r} toast {_last_toast(sess)!r} mode {_mode(sess)}")
        raise
    sess.eval('reaper.DeleteExtState("SneakPeak", "last_toast", false)')
    x, y = locate_apply_button(sess, SHOTS / "oneshot_panel.png")
    _send_sync(sess, "WM_LBUTTONDOWN", 1, x, y, settle=0.1)
    _send_sync(sess, "WM_LBUTTONUP", 0, x, y, settle=0.1)
    wait_main_thread_idle(sess, timeout=120)
    time.sleep(1.0)
    _close_one_shot_panel(sess)


def _close_one_shot_panel(sess):
    """CM_ONESHOT_FACTORY only SHOWS the panel (no toggle), so the old "close"
    left it open for every later test (its Run button is the largest amber
    blob - the next spec's Apply click ran the Factory instead). Click the
    panel's close cross: a fixed offset from Run in the 800x400 layout."""
    from conftest import locate_apply_button
    xy = locate_apply_button(sess, SHOTS / "oneshot_close.png")
    if xy is None:
        return
    _send_sync(sess, "WM_LBUTTONDOWN", 1, xy[0] + 40, xy[1] - 235, settle=0.1)
    _send_sync(sess, "WM_LBUTTONUP", 0, xy[0] + 40, xy[1] - 235, settle=0.3)
    assert locate_apply_button(sess, SHOTS / "oneshot_close.png") is None, "the One-Shot panel did not close"


def test_one_shot_lufs_on_a_pcm_standalone_file_does_not_clip(sess):
    """LUFS-I -16 on a quiet 16-bit file needs ~+17 dB; a 0.9 click then
    lands far above full scale and the PCM writer clipped it flat. The gain
    now goes through the true-peak limiter when it would clip. Control
    (0a444ad = cb48cd5 + the open action): a run of full-scale samples."""
    # Quiet 220 Hz tone (-30 dBFS, the loudness) with one 2 ms click at 0.9
    # (the peak): LUFS-I -16 asks for about +17 dB, the click then wants 6x
    # full scale. (A burst of a second would dominate the gated loudness and
    # need no gain at all.)
    media = perf_media_dir() / "sa_lufs16_10s.wav"
    sr = 44100
    t = np.arange(10 * sr) / sr
    y = 0.03 * np.sin(2 * np.pi * 220 * t)
    y[5 * sr:5 * sr + 88] = 0.9 * np.sign(np.sin(2 * np.pi * 220 * t[5 * sr:5 * sr + 88]) + 1e-9)
    sf.write(str(media), np.stack([y, y], axis=1), sr, subtype="PCM_16")
    out = media.parent / f"{media.stem}_01.wav"
    out.unlink(missing_ok=True)
    _open_and_wait(sess, media)

    _run_one_shot(sess, {"os_slice_mode": "0", "os_trim": "0", "os_fade_in": "0", "os_fade_out": "0",
                         "os_norm_mode": "2", "os_target": "-16000", "os_pattern": "{name}_{nn}"})

    assert out.exists(), f"One-Shot wrote no file: {out} (toast {_last_toast(sess)!r})"
    got = sf.read(str(out), dtype="float64", always_2d=True)[0]
    peak = float(np.abs(got).max())
    full = np.abs(got).max(axis=1) >= 0.9999
    run = 0; longest = 0
    for f in full:
        run = run + 1 if f else 0
        longest = max(longest, run)
    print(f"\n[oneshot] LUFS-I -16 on 16-bit: peak {20*np.log10(peak):.2f} dBFS, longest full-scale run {longest}")
    assert peak <= 10 ** (-0.1 / 20) + 1e-4, f"output peaks at {20*np.log10(peak):.2f} dBFS (ceiling -0.1)"
    assert longest < 3, f"clipped: {longest} consecutive full-scale samples"


def test_one_shot_regions_mode_is_refused_in_standalone(sess):
    """REGIONS slices by the PROJECT's regions - a Standalone file is not on
    the timeline, so the mode is refused with a message instead of cutting
    the file by unrelated regions. Control (cb48cd5): files written."""
    media = burst_fixture("sa_regions_10s.wav", seconds=10, channels=2)
    for p in media.parent.glob(f"{media.stem}_0*.wav"):
        p.unlink()
    _open_and_wait(sess, media)
    sess.eval('reaper.AddProjectMarker2(0, true, 1.0, 2.0, "r1", 1, 0) '
              'reaper.AddProjectMarker2(0, true, 4.0, 6.0, "r2", 2, 0) return true')

    _run_one_shot(sess, {"os_slice_mode": "1", "os_trim": "0", "os_fade_in": "0", "os_fade_out": "0",
                         "os_norm_mode": "0", "os_pattern": "{name}_{nn}"})

    written = sorted(p.name for p in media.parent.glob(f"{media.stem}_0*.wav"))
    toast = _last_toast(sess)
    assert not written, f"REGIONS mode sliced a Standalone file by the project's regions: {written}"
    assert "REGIONS uses the project" in toast, f"refusal toast missing: {toast!r}"


# --- A7.4: a small Standalone edit re-analyses the buffer -----------------------
CM_SILENCE = 2006


def _tone_burst_fixture(name: str, *, hz: float = 2000.0, seconds: float = 10.0, sr: int = 44100) -> Path:
    """Mono quiet tone (0.03) with a 0.9 burst at 0.5-1.5 s; a 2 kHz carrier so
    a 2 ms window holds whole cycles (stable RMS). Fresh file every call."""
    path = perf_media_dir() / name
    t = np.arange(int(seconds * sr)) / sr
    y = 0.03 * np.sin(2 * np.pi * hz * t)
    burst = (t >= 0.5) & (t < 1.5)
    y[burst] = 0.9 * np.sin(2 * np.pi * hz * t[burst])
    sf.write(str(path), y.astype("float32"), sr, subtype="FLOAT")   # MONO: the hash samples every 4096th
    return path                                                        # interleaved sample = every 4096th frame


def _apply_standalone(sess, shots: Path):
    from conftest import locate_apply_button
    sess.eval('reaper.DeleteExtState("SneakPeak", "last_toast", false)')
    if locate_apply_button(sess, shots / "dyn_panel.png") is None:
        _command_sync(sess, 2058, settle=1.0)                        # CM_APPLY_DYNAMICS: shows the panel
        sess.wait_until(lambda: locate_apply_button(sess, shots / "dyn_panel.png") is not None, timeout=15)
    x, y = locate_apply_button(sess, shots / "dyn_panel.png")
    _send_sync(sess, "WM_LBUTTONDOWN", 1, x, y, settle=0.1)
    _send_sync(sess, "WM_LBUTTONUP", 0, x, y, settle=0.1)
    sess.wait_until(lambda: _last_toast(sess).startswith("Dynamics applied"), timeout=20)
    wait_main_thread_idle(sess, timeout=60)


def test_standalone_small_edit_reanalyses(sess):
    """The engine keeps the last analysis trace behind a sparse content hash
    (every 4096th sample). A 60 ms silence cut inside the burst that lands
    between two hashed samples left the old trace in place, so the second
    Apply compressed with the curve of the UNEDITED audio: the gain right
    after the gap equalled the gain before it. Re-analysed, the compressor
    has released during the gap and re-attacks after it - the 2 ms after the
    gap come out louder than the 2 ms before. Control (b88fbd4): ratio 1.0."""
    from conftest import WAVE_Y, drag_client
    media = _tone_burst_fixture("sa_reanalyse_10s.wav")
    _open_and_wait(sess, media)
    SHOTS.mkdir(parents=True, exist_ok=True)
    _apply_standalone(sess, SHOTS)                                  # Apply 1: the burst is compressed
    drag_client(sess, 80, WAVE_Y, 84, WAVE_Y)                        # ~1.056-1.109 s, inside the burst
    time.sleep(0.4)
    _command_sync(sess, CM_SILENCE, settle=0.5)
    wait_main_thread_idle(sess, timeout=60)
    _apply_standalone(sess, SHOTS)                                  # Apply 2: must see the gap
    got = _preview_of_whole_buffer(sess)
    sr = 44100
    mono = got[:, 0]
    burst = mono[int(0.5 * sr):int(1.5 * sr)]
    zero = np.abs(burst) < 1e-9
    runs = np.flatnonzero(np.diff(np.concatenate(([0], zero.astype(int), [0]))))
    starts, ends = runs[0::2], runs[1::2]
    long = ends - starts >= 1000                     # the sine's exact zeros are single samples
    starts, ends = starts[long], ends[long]
    assert len(starts) == 1, f"expected one silent gap inside the burst, found {len(starts)}"
    g0, g1 = int(starts[0]) + int(0.5 * sr), int(ends[0]) + int(0.5 * sr)
    stride = 4096 // got.shape[1]                    # hashed FRAMES (the hash walks interleaved samples)
    hashed_inside = [k * stride for k in range(g0 // stride, g1 // stride + 2) if g0 <= k * stride < g1]
    w = int(0.002 * sr)
    before = float(np.sqrt(np.mean(mono[g0 - w:g0] ** 2)))
    after = float(np.sqrt(np.mean(mono[g1:g1 + w] ** 2)))
    ratio = after / max(before, 1e-12)
    print(f"\n[standalone] gap frames {g0}-{g1} ({(g1 - g0) / sr * 1e3:.0f} ms), hashed samples inside: {hashed_inside}; "
          f"RMS 2 ms before {before:.4f} / after {after:.4f} = x{ratio:.3f}")
    assert not hashed_inside, "precondition: the gap covers a hashed sample (the hash would catch it)"
    assert ratio > 1.10, f"the second Apply used the stale analysis (after/before = {ratio:.3f})"


# ---------------------------------------------------------------------------
# A10.4: a Standalone overwrite keeps the original WAV's metadata chunks
# ---------------------------------------------------------------------------
SAVE = ('reaper.defer(function() reaper.Main_OnCommand('
        'reaper.NamedCommandLookup("_SneakPeak_SaveStandalone"), 0) end) return true')


def _bwf_fixture() -> Path:
    """The 10 s burst fixture with bext + iXML + LIST chunks in front of the
    data chunk (a Broadcast WAV the way field recorders write it). Fresh copy
    per call - the spec overwrites it."""
    base = burst_fixture("sa_bwf_base_10s.wav", seconds=10, channels=2)
    raw = base.read_bytes()
    data_at = raw.index(b"data", 12)
    bext = b"SneakPeak BWF description".ljust(256, b"\0") + b"originator".ljust(32, b"\0") + b"\0" * (602 - 288)
    ixml = b"<BWFXML><IXML_VERSION>1.5</IXML_VERSION><PROJECT>ReaProof</PROJECT></BWFXML>"   # odd length
    info = b"INFO" + b"ISFT" + (10).to_bytes(4, "little") + b"SneakPeak\0"
    def chunk(cid: bytes, payload: bytes) -> bytes:
        return cid + len(payload).to_bytes(4, "little") + payload + (b"\0" if len(payload) & 1 else b"")
    body = raw[12:data_at] + chunk(b"bext", bext) + chunk(b"iXML", ixml) + chunk(b"LIST", info) + raw[data_at:]
    out = base.with_name("sa_bwf_10s.wav")
    out.write_bytes(b"RIFF" + (len(body) + 4).to_bytes(4, "little") + b"WAVE" + body)
    return out


def _riff_chunks(path: Path) -> dict:
    raw = path.read_bytes()
    assert raw[:4] == b"RIFF" and raw[8:12] == b"WAVE", "not a RIFF/WAVE"
    assert int.from_bytes(raw[4:8], "little") == len(raw) - 8, "RIFF size != file size - 8"
    out, pos = {}, 12
    while pos + 8 <= len(raw):
        assert pos % 2 == 0, f"chunk at odd offset {pos}"
        cid, size = raw[pos:pos + 4], int.from_bytes(raw[pos + 4:pos + 8], "little")
        out[cid] = raw[pos + 8:pos + 8 + size]
        pos += 8 + size + (size & 1)
    return out


def test_standalone_overwrite_keeps_the_bwf_metadata(sess):
    """Reverse a Broadcast WAV in Standalone and save over it: bext / iXML /
    LIST come back byte-identical, the audio is the edited one and REAPER
    still opens the file. Control (44ac91e): the chunks are gone."""
    from conftest import dismiss_native_modal
    media = _bwf_fixture()
    before = _riff_chunks(media)
    want = sf.read(str(media), dtype="float64", always_2d=True)[0]
    assert {b"bext", b"iXML", b"LIST"} <= set(before), "fixture lost its chunks"
    _open_and_wait(sess, media)
    sess.eval(REVERSE)
    dismiss_native_modal(sess, timeout=3)     # no prompt since A4.4; harmless otherwise
    wait_main_thread_idle(sess, timeout=60)
    sess.eval('reaper.DeleteExtState("SneakPeak", "last_toast", false)')
    # the Save action from a deferred script: the "Overwrite original file?"
    # MessageBox then stalls the defer loop and the dismisser answers Yes
    # (Ctrl+S through the accelerator keeps the loop pumping - invisible here)
    sess.eval(SAVE)
    prompted = dismiss_native_modal(sess, timeout=10)
    try:
        sess.wait_until(lambda: _last_toast(sess).startswith("Saved"), timeout=30)
    except Exception:
        from conftest import window_title
        from test_input import _mac_windows
        print(f"\n[bwf] no Saved toast: prompted={prompted} title={window_title(sess)!r} "
              f"toast={_last_toast(sess)!r} windows={_mac_windows(sess)}")
        raise
    wait_main_thread_idle(sess, timeout=60)

    after = _riff_chunks(media)
    for cid in (b"bext", b"iXML", b"LIST"):
        assert cid in after, f"{cid!r} chunk gone after the Standalone overwrite"
        assert after[cid] == before[cid], f"{cid!r} chunk not byte-identical after the overwrite"
    got = sf.read(str(media), dtype="float64", always_2d=True)[0]
    assert len(got) == len(want), f"{len(got)} frames after the save, expected {len(want)}"
    assert np.abs(got[:4410] - want[::-1][:4410]).max() < 1e-4, "the saved audio is not the reversed buffer"
    length = float(sess.eval(f'local src = reaper.PCM_Source_CreateFromFile("{media.as_posix()}") '
                             'if not src then return -1 end local len = reaper.GetMediaSourceLength(src) '
                             'reaper.PCM_Source_Destroy(src) return len'))
    assert abs(length - 10.0) < 0.01, f"REAPER reads the saved BWF as {length} s"
