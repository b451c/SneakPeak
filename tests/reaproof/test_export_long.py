"""Exports from long items must be written at the source rate (finding F11).

Items over the 10M-frame cap (~3.8 min stereo at 44.1k) are held in a
DOWNSAMPLED working buffer; every exporter used to write that buffer, so a
5-minute item came out as a 22050 Hz file. Ground truth: the written file
itself (soundfile) against the source's bytes for the exported window.
The 3-minute case (full-rate buffer) is GREEN on the old build too: it proves
the stream reproduces what the loader built.
"""
from __future__ import annotations

import sys as _sys

import pytest as _pytest

import numpy as np
import soundfile as sf

from conftest import (SELECT_ITEM0, burst_fixture, clear_project, ensure_window,
                      insert_item_unselected, perf_media_dir, send_command,
                      wait_audio_loaded)

CM_EDIT_COPY_STANDALONE = 2258   # edit_view.h enum ContextMenuID (parsed 2026-08-28)
SR = 44100


def _source_window(path, start_frame: int, end_frame: int) -> np.ndarray:
    """24-bit PCM as exact doubles (int24 / 2^23), frames x channels."""
    with sf.SoundFile(str(path)) as f:
        assert f.subtype == "PCM_24", f.subtype
        f.seek(start_frame)
        raw = f.read(end_frame - start_frame, dtype="int32", always_2d=True)
    return raw.astype(np.float64) / 2147483648.0


def _remove_old_copies(media):
    for p in media.parent.glob(f"{media.stem}_edit*.wav"):
        p.unlink()


def _edit_copy(sess, media, *, load_timeout=90):
    """Select the item, run Edit Copy in Standalone, wait for the new tab."""
    clear_project(sess)
    _remove_old_copies(media)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=load_timeout)
    send_command(sess, CM_EDIT_COPY_STANDALONE)
    out = media.parent / f"{media.stem}_edit.wav"
    wait_audio_loaded(sess, out.name, timeout=load_timeout)
    assert out.exists(), f"no edit copy next to the source: {out}"
    return out


def _assert_equals_source(out, media, frames: int, channels: int, atol=2e-7):
    info = sf.info(str(out))
    assert info.samplerate == SR, f"edit copy written at {info.samplerate} Hz"
    assert info.subtype == "FLOAT", info.subtype
    assert info.channels == channels, info.channels
    assert abs(info.frames - frames) <= 1, f"{info.frames} frames, expected {frames}"
    got = sf.read(str(out), dtype="float64", always_2d=True)[0]
    n = min(len(got), frames)
    want = _source_window(media, 0, n)
    diff = float(np.max(np.abs(got[:n] - want)))
    print(f"\n[export] {out.name}: {info.samplerate} Hz, {info.frames} frames, max |diff| vs source = {diff:.3g}")
    assert diff <= atol, f"edit copy differs from the source window: max |diff| {diff}"


def test_edit_copy_of_a_five_minute_item_keeps_the_source_rate(sess):
    media = burst_fixture("long5min_burst24.wav", seconds=300, channels=2)
    out = _edit_copy(sess, media)
    _assert_equals_source(out, media, 300 * SR, 2)


def test_edit_copy_of_a_three_minute_item_equals_the_source(sess):
    # Full-rate buffer (under the 10M-frame cap): the stream must reproduce it.
    media = burst_fixture("long3min_burst24.wav", seconds=180, channels=2)
    out = _edit_copy(sess, media)
    _assert_equals_source(out, media, 180 * SR, 2)


def _stereo_lr_fixture():
    """Stereo with DIFFERENT channels (the burst fixtures duplicate L into R)."""
    path = perf_media_dir() / "stereo_lr_24.wav"
    if not path.exists():
        t = np.arange(20 * SR) / SR
        left = 0.5 * np.sin(2 * np.pi * 220 * t)
        right = 0.25 * np.sin(2 * np.pi * 331 * t)
        sf.write(str(path), np.stack([left, right], axis=1).astype(np.float32), SR, subtype="PCM_24")
    return path


def test_edit_copy_folds_the_take_channel_mode(sess):
    """I_CHANMODE 2 (mono mix) on the take: the copy is mono and equals (L+R)/2 -
    pins the fold policy (the accessor is assumed to deliver the raw channels)."""
    media = _stereo_lr_fixture()
    clear_project(sess)
    _remove_old_copies(media)
    insert_item_unselected(sess, media)
    sess.eval("""
      local it = reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), 0)
      reaper.SetMediaItemTakeInfo_Value(reaper.GetActiveTake(it), "I_CHANMODE", 2)
      reaper.UpdateArrange() return true""")
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=60)
    send_command(sess, CM_EDIT_COPY_STANDALONE)
    out = media.parent / f"{media.stem}_edit.wav"
    wait_audio_loaded(sess, out.name, timeout=60)
    info = sf.info(str(out))
    assert info.channels == 1, f"channel mode not folded: {info.channels} channels"
    assert info.samplerate == SR
    got = sf.read(str(out), dtype="float64", always_2d=True)[0][:, 0]
    src = _source_window(media, 0, len(got))
    want = (src[:, 0] + src[:, 1]) * 0.5
    diff = float(np.max(np.abs(got - want)))
    print(f"\n[export] chanmode fold: max |diff| vs (L+R)/2 = {diff:.3g}")
    assert diff <= 2e-7, diff


# --------------------------------------------------------------------------
# Drag export (ITEM mode) + accessor offset/playrate rendering
# --------------------------------------------------------------------------
import time
from pathlib import Path

from conftest import WAVE_Y, drag_client, wait_main_thread_idle

def _export_dir(sess) -> Path:
    # AudioEngine::ExportWavPath priority 1: the project's recording path,
    # which GetProjectPathEx answers for an UNSAVED project too (audit A2.3 -
    # the export used to fall through to $TMPDIR, purged under a saved
    # project's feet). Control (cb48cd5): files under TMPDIR, none here.
    return Path(str(sess.eval("return reaper.GetProjectPathEx(0)")))


def _os_drag_client(sess, x0: int, y0: int, x1: int, y1: int, *, steps: int = 24):
    """OS-level (CGEvent) drag in CLIENT coordinates; y < 0 = the title bar."""
    from reaproof.observe.input import WindowGesture, window_bounds_macos
    from conftest import WINDOW_TITLE, client_size
    g = WindowGesture(sess, WINDOW_TITLE)
    _, _, w, h = window_bounds_macos(sess.handle.pid, WINDOW_TITLE)
    cw, ch = client_size(sess)
    bar = h - ch                      # window frame above the client area
    g.drag((x0 / w, (bar + y0) / h), (x1 / w, (bar + y1) / h), steps=steps)


def _reaper_time_selection(sess):
    s, e = sess.eval("local s, e = reaper.GetSet_LoopTimeRange2(0, false, false, 0, 0, false) return {s, e}")
    return float(s), float(e)


def _best_shift(got: np.ndarray, media, start_frame: int, atol: float, shifts=range(-3, 4)):
    """Source alignment of the export within +-3 frames (the accessor rounds a
    fractional selection start to a frame); returns (shift, max_diff)."""
    best = None
    for sh in shifts:
        a = start_frame + sh
        if a < 0:
            continue
        want = _source_window(media, a, a + len(got))
        d = float(np.max(np.abs(got - want)))
        if best is None or d < best[1]:
            best = (sh, d)
        if d <= atol:
            return sh, d
    return best


@_pytest.mark.skipif(_sys.platform != "darwin",
                     reason="the OS drag session (OLE on Windows) needs a real mouse; covered by the VM-side real-mouse drag check")
def test_drag_export_of_a_five_minute_selection_keeps_the_source_rate(sess):
    media = burst_fixture("long5min_burst24.wav", seconds=300, channels=2)
    clear_project(sess)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=90)

    # Selection = a drag on the waveform (~25%..62% of the item); SneakPeak
    # mirrors it into REAPER's time selection, which gives us the exact seconds.
    drag_client(sess, 200, WAVE_Y, 500, WAVE_Y)
    time.sleep(0.5)
    t0, t1 = _reaper_time_selection(sess)
    assert t1 - t0 > 60.0, f"selection too short: {t0}..{t1}"

    before = set(_export_dir(sess).glob("sneakpeak_sel_*.wav"))
    # Drag from inside the selection out of the waveform lane (no modifier =
    # export the moment the pointer leaves the lane, then the OS drag session).
    # REAL mouse events (CGEvent): the OS drag session that SneakPeak starts
    # from WM_MOUSEMOVE needs a real mouse-up to end - through the in-process
    # JS_WindowMessage path it runs inside the Lua chunk and hangs the bridge.
    # The drop lands on the window's own title bar (a no-op target).
    wall0 = time.monotonic()
    _os_drag_client(sess, 350, WAVE_Y, 350, -12, steps=24)
    new = []
    def appeared():
        new[:] = sorted(set(_export_dir(sess).glob("sneakpeak_sel_*.wav")) - before)
        return bool(new)
    sess.wait_until(appeared, timeout=30)
    wall = time.monotonic() - wall0
    wait_main_thread_idle(sess, timeout=30)   # the drag session has ended
    out = new[-1]
    info = sf.info(str(out))
    frames = int(round((t1 - t0) * SR))
    print(f"\n[export] drag: {out.name} {info.samplerate} Hz {info.subtype} {info.frames} frames "
          f"(selection {t0:.4f}-{t1:.4f} s = {frames}), {wall:.2f} s gesture-to-file")
    assert info.samplerate == SR, f"drag export written at {info.samplerate} Hz"
    assert info.subtype == "PCM_24", f"source format not kept: {info.subtype}"
    assert info.channels == 2
    assert abs(info.frames - frames) <= 1, f"{info.frames} frames, expected {frames}"
    got = sf.read(str(out), dtype="int32", always_2d=True)[0].astype(np.float64) / 2147483648.0
    shift, diff = _best_shift(got, media, int(round(t0 * SR)), atol=1.5 / 8388608.0)
    print(f"[export] drag: aligned at shift {shift}, max |diff| vs source = {diff:.3g}")
    assert diff <= 1.5 / 8388608.0, f"export differs from the source window (shift {shift}): {diff}"


def _trimmed_item(sess, media, *, offset_s: float, length_s: float, playrate: float = 1.0):
    """One item showing [offset, offset + length * playrate) of the source."""
    clear_project(sess)
    _remove_old_copies(media)
    insert_item_unselected(sess, media)
    sess.eval(f"""
      local it = reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), 0)
      local tk = reaper.GetActiveTake(it)
      reaper.SetMediaItemTakeInfo_Value(tk, "B_PPITCH", 0)
      reaper.SetMediaItemTakeInfo_Value(tk, "D_PLAYRATE", {playrate})
      reaper.SetMediaItemTakeInfo_Value(tk, "D_STARTOFFS", {offset_s})
      reaper.SetMediaItemLength(it, {length_s}, false)
      reaper.UpdateArrange() return true""")
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=90)
    send_command(sess, CM_EDIT_COPY_STANDALONE)
    out = media.parent / f"{media.stem}_edit.wav"
    wait_audio_loaded(sess, out.name, timeout=90)
    return out


def test_export_of_a_trimmed_offset_item_is_its_window_of_the_source(sess):
    """D_STARTOFFS 100 s, length 60 s: the export equals source[100 s, 160 s)
    - the accessor renders the take offset (the F12 class of bug, for reads)."""
    media = burst_fixture("long5min_burst24.wav", seconds=300, channels=2)
    out = _trimmed_item(sess, media, offset_s=100.0, length_s=60.0)
    info = sf.info(str(out))
    assert info.samplerate == SR and abs(info.frames - 60 * SR) <= 1, (info.samplerate, info.frames)
    got = sf.read(str(out), dtype="float64", always_2d=True)[0]
    want = _source_window(media, 100 * SR, 100 * SR + len(got))
    diff = float(np.max(np.abs(got - want)))
    print(f"\n[export] offset item: max |diff| vs source[100 s..] = {diff:.3g}")
    assert diff <= 2e-7, diff


def test_export_of_a_rate_changed_item_follows_the_playrate(sess):
    """Playrate 1.3 over 60 s of item time covers 78 s of source: the export is
    60 s long and its per-second RMS tracks source[100 + 1.3 t] within 1 dB
    (REAPER's resampler decides the samples, so the check is energy, not bytes)."""
    media = burst_fixture("long5min_burst24.wav", seconds=300, channels=2)
    out = _trimmed_item(sess, media, offset_s=100.0, length_s=60.0, playrate=1.3)
    info = sf.info(str(out))
    assert info.samplerate == SR and abs(info.frames - 60 * SR) <= 1, (info.samplerate, info.frames)
    got = sf.read(str(out), dtype="float64", always_2d=True)[0][:, 0]
    src = _source_window(media, 100 * SR, 100 * SR + int(78.5 * SR))[:, 0]
    worst = 0.0
    for sec in range(0, 60):
        g = got[sec * SR:(sec + 1) * SR]
        a = int(round(sec * 1.3 * SR)); b = int(round((sec + 1) * 1.3 * SR))
        w = src[a:b]
        rg, rw = float(np.sqrt(np.mean(g * g))), float(np.sqrt(np.mean(w * w)))
        worst = max(worst, abs(20 * np.log10(max(rg, 1e-9) / max(rw, 1e-9))))
    print(f"\n[export] playrate 1.3: worst per-second RMS deviation {worst:.2f} dB")
    assert worst <= 1.0, worst


# --------------------------------------------------------------------------
# One-Shot Factory slices
# --------------------------------------------------------------------------
from conftest import click_client, locate_apply_button

CM_ONESHOT_FACTORY = 2256
SHOTS = Path("/tmp/sneakpeak-reaproof-shots/export_long")
REGIONS = [(10.0, 11.0), (20.0, 21.5)]


def test_one_shot_slices_of_a_long_item_keep_the_source_rate(sess):
    media = _silent_burst_wav("long5min_silentbursts.wav", 300)
    clear_project(sess)
    for p in media.parent.glob(f"{media.stem}_0*.wav"):
        p.unlink()
    insert_item_unselected(sess, media)
    # The panel restores these session defaults when it opens: slice by
    # regions, trim/fades/normalize off, so each file is exactly its region.
    regions = " ".join(f'reaper.AddProjectMarker2(0, true, {a}, {b}, "r{i}", {i + 1}, 0)'
                       for i, (a, b) in enumerate(REGIONS))
    sess.eval(f"""
      local function set(k, v) reaper.SetExtState("SneakPeak", k, v, false) end
      set("os_trim_thr", "-60000") set("os_pad", "0") set("os_fade_in", "0")
      set("os_fade_out", "0") set("os_target", "-1000") set("os_norm_mode", "0")
      set("os_trim", "0") set("os_slice_mode", "1") set("os_pattern", "{{name}}_{{nn}}")
      {regions}
      return true""")
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=90)

    SHOTS.mkdir(parents=True, exist_ok=True)
    send_command(sess, CM_ONESHOT_FACTORY)
    # 8g: opening the panel starts the lazy buffer load (the slice list is in
    # buffer frames); Run before it lands is a toast, so wait for the title.
    wait_audio_loaded(sess, media.stem, timeout=90)
    # The panel's amber Run button is the only amber blob with Dynamics closed.
    sess.wait_until(lambda: locate_apply_button(sess, SHOTS / "oneshot_panel.png") is not None,
                    timeout=10)
    click_client(sess, *locate_apply_button(sess, SHOTS / "oneshot_panel.png"))
    outs = [media.parent / f"{media.stem}_{i + 1:02d}.wav" for i in range(len(REGIONS))]
    sess.wait_until(lambda: all(o.exists() for o in outs), timeout=30)
    wait_main_thread_idle(sess, timeout=30)

    for out, (r0, r1) in zip(outs, REGIONS):
        info = sf.info(str(out))
        frames = int(round((r1 - r0) * SR))
        print(f"\n[export] one-shot {out.name}: {info.samplerate} Hz {info.subtype} {info.frames} frames "
              f"(region {r0}-{r1} s = {frames})")
        assert info.samplerate == SR, f"{out.name} written at {info.samplerate} Hz"
        assert info.subtype == "FLOAT", info.subtype
        assert abs(info.frames - frames) <= 2, (info.frames, frames)
        got = sf.read(str(out), dtype="float64", always_2d=True)[0]
        shift, diff = _best_shift(got, media, int(round(r0 * SR)), atol=2e-7)
        print(f"[export] one-shot {out.name}: aligned at shift {shift}, max |diff| = {diff:.3g}")
        assert diff <= 2e-7, f"{out.name} differs from the source region (shift {shift}): {diff}"


# --- A2.4: exports bake the item volume (contract lock) ------------------------
def _rms(x: np.ndarray) -> float:
    return float(np.sqrt(np.mean(np.square(x.astype(np.float64)))))


@_pytest.mark.skipif(_sys.platform != "darwin",
                     reason="the OS drag session needs a real mouse (see the drag export spec above)")
def test_drag_export_bakes_the_item_volume(sess):
    """Locked decision (2026-08-28 s7): what you hear is what you export. An
    item at D_VOL -6 dB drags out 6 dB below its source. This spec documents
    the contract - it is GREEN on the cb48cd5 control by design (no RED
    needed: the behaviour is being locked, not fixed). The file is looked for
    in the project path AND in $TMPDIR so the destination change (A2.3) does
    not leak into this contract."""
    media = burst_fixture("vol_30s.wav", seconds=30, channels=2)
    clear_project(sess)
    insert_item_unselected(sess, media)
    sess.eval("""
      local it = reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), 0)
      reaper.SetMediaItemInfo_Value(it, "D_VOL", 0.5)
      reaper.UpdateItemInProject(it)""")
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=60)

    drag_client(sess, 200, WAVE_Y, 500, WAVE_Y)
    time.sleep(0.5)
    t0, t1 = _reaper_time_selection(sess)
    assert t1 - t0 > 5.0, f"selection too short: {t0}..{t1}"
    import os
    dirs = {_export_dir(sess), Path(os.environ.get("TMPDIR", "/tmp"))}
    def exports():
        return {p for d in dirs for p in d.glob("sneakpeak_sel_*.wav")}   # ITEM mode: no source path in the name
    before = exports()
    _os_drag_client(sess, 350, WAVE_Y, 350, -12, steps=24)
    new = []
    def appeared():
        new[:] = sorted(exports() - before)
        return bool(new)
    sess.wait_until(appeared, timeout=30)
    wait_main_thread_idle(sess, timeout=30)

    got = sf.read(str(new[-1]), dtype="float64", always_2d=True)[0]
    want = _source_window(media, int(round(t0 * SR)), int(round(t0 * SR)) + len(got))
    ratio = _rms(got) / _rms(want)
    print(f"\n[export] D_VOL 0.5: export/source RMS ratio = {ratio:.4f}")
    assert abs(ratio - 0.5) < 0.01, f"item volume not baked (ratio {ratio:.4f}, want 0.5)"


def _silent_burst_wav(name: str, seconds: float, every: float = 10.0) -> Path:
    """Digital silence (-90 dB noise floor) with a 1 s 220 Hz burst at 0.9 from
    0.5 s of every `every`-second block, 24-bit stereo. Fresh copy per call."""
    from conftest import perf_media_dir
    pristine = perf_media_dir() / "pristine" / name
    if not pristine.exists():
        pristine.parent.mkdir(exist_ok=True)
        rng = np.random.default_rng(7)
        with sf.SoundFile(str(pristine), "w", samplerate=SR, channels=2, subtype="PCM_24") as f:
            for start in range(0, int(seconds * SR), SR * 10):
                t = (np.arange(SR * 10) + start) / SR
                y = rng.standard_normal(SR * 10) * 3e-5
                burst = ((t % every) >= 0.5) & ((t % every) < 1.5)
                y[burst] = 0.9 * np.sin(2 * np.pi * 220 * t[burst])
                f.write(np.stack([y, y], axis=1).astype(np.float32))
    work = perf_media_dir() / name
    import shutil
    shutil.copyfile(pristine, work)
    return work


# --- A4.6: One-Shot SILENCE slicing on a long item ----------------------------
def test_one_shot_silence_slices_of_a_long_item_land_on_the_burst_edges(sess):
    """Slice-by-silence scans the WORKING buffer, which on a long item is the
    reduced-rate one (33 kHz on 5 minutes, 8 kHz on 20): this measures how far the slice
    edges land from the true burst edges (bursts at 0.5-1.5 s of every 10 s
    block, -20 dB threshold, no trim/pad). Reported in ms; the assertion is
    the 1 ms bar from the audit plan."""
    media = _silent_burst_wav("long5min_silentbursts.wav", 300)
    clear_project(sess)
    for p in media.parent.glob(f"{media.stem}_0*.wav"):
        p.unlink()
    for p in media.parent.glob(f"{media.stem}_1*.wav"):
        p.unlink()
    insert_item_unselected(sess, media)
    sess.eval("""
      local function set(k, v) reaper.SetExtState("SneakPeak", k, v, false) end
      set("os_trim_thr", "-60000") set("os_pad", "0") set("os_fade_in", "0")
      set("os_fade_out", "0") set("os_target", "-1000") set("os_norm_mode", "0")
      set("os_trim", "0") set("os_slice_mode", "2") set("os_pattern", "{name}_{nnn}")
      return true""")
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=120)
    SHOTS.mkdir(parents=True, exist_ok=True)
    send_command(sess, CM_ONESHOT_FACTORY)
    wait_audio_loaded(sess, media.stem, timeout=180)   # lazy buffer lands
    sess.wait_until(lambda: locate_apply_button(sess, SHOTS / "oneshot_silence.png") is not None,
                    timeout=10)
    click_client(sess, *locate_apply_button(sess, SHOTS / "oneshot_silence.png"))
    n_bursts = 30
    outs = [media.parent / f"{media.stem}_{i + 1:03d}.wav" for i in range(n_bursts)]
    try:
        sess.wait_until(lambda: outs[-1].exists(), timeout=120)
    except Exception:
        print(f"\n[oneshot] only {len(list(media.parent.glob(media.stem + '_[0-9]*.wav')))} files; toast "
              f"{sess.eval('return reaper.GetExtState(\"SneakPeak\", \"last_toast\")')!r}")
        raise
    wait_main_thread_idle(sess, timeout=60)

    written = sorted(media.parent.glob(f"{media.stem}_[0-9][0-9][0-9].wav"))
    assert len(written) == n_bursts, f"{len(written)} slices, expected {n_bursts}"
    offsets_ms = []
    for i, out in enumerate(written[:12] + written[-3:]):
        got = sf.read(str(out), dtype="float64", always_2d=True)[0]
        k = int(out.stem.rsplit("_", 1)[1]) - 1
        start = int(round((k * 10.0 + 0.5) * SR))
        shift, diff = _best_shift(got[:2048], media, start, atol=2e-6, shifts=range(-200, 201))
        offsets_ms.append(shift * 1000.0 / SR)
    worst = max(abs(o) for o in offsets_ms)
    print(f"\n[oneshot] silence edges vs bursts: offsets ms {['%.3f' % o for o in offsets_ms]} worst {worst:.3f}")
    assert worst < 1.0, f"slice edges up to {worst:.3f} ms off the burst edges"


def test_one_shot_silence_slices_of_a_short_item_sanity(sess):
    """Same settings on a 30 s full-rate item: 3 bursts -> 3 slices. A control
    for the long-item measurement above (buffer path vs reduced-rate buffer)."""
    media = _silent_burst_wav("short30s_silentbursts.wav", 30)
    clear_project(sess)
    for p in media.parent.glob(f"{media.stem}_[0-9]*.wav"):
        p.unlink()
    insert_item_unselected(sess, media)
    sess.eval("""
      local function set(k, v) reaper.SetExtState("SneakPeak", k, v, false) end
      set("os_trim_thr", "-60000") set("os_pad", "0") set("os_fade_in", "0")
      set("os_fade_out", "0") set("os_target", "-1000") set("os_norm_mode", "0")
      set("os_trim", "0") set("os_slice_mode", "2") set("os_pattern", "{name}_{nnn}")
      return true""")
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_audio_loaded(sess, media.stem, timeout=60)
    SHOTS.mkdir(parents=True, exist_ok=True)
    send_command(sess, CM_ONESHOT_FACTORY)
    sess.wait_until(lambda: locate_apply_button(sess, SHOTS / "oneshot_silence_short.png") is not None,
                    timeout=10)
    click_client(sess, *locate_apply_button(sess, SHOTS / "oneshot_silence_short.png"))
    time.sleep(3.0)
    wait_main_thread_idle(sess, timeout=60)
    written = sorted(p.name for p in media.parent.glob(f"{media.stem}_[0-9]*.wav"))
    toast = sess.eval('return reaper.GetExtState("SneakPeak", "last_toast")')
    print(f"\n[oneshot] short item silence: {written} toast {toast!r}")
    assert len(written) == 3, f"expected 3 burst slices, got {written} ({toast!r})"
