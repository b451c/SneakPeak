"""Hard Limiter live preview is optional on long audio (v2.5, user s19 end:
"the live calculation should be optional").

Audio over 5 minutes opens the HARD LIMITER panel WITHOUT computing: no pass,
no gain-reduction band, readouts "-"; a PREVIEW pill in the panel header
starts the first pass (progress on the pill) and the choice is remembered
(ExtState lim_preview). A long timeline item is not even decoded until the
pill is pressed. Shorter audio previews at once, as before, with no pill.

Oracles: the red gain-reduction band along the top of the waveform (kGrRed
hue - the control paints it after its automatic pass, so the "no band" checks
are RED there), the window title ("Loading item audio..." must never appear
on its own - brief on a fast disk, so the band is the hard oracle), the
ExtState mirror SneakPeak/lim_preview_state ("closed" | "off" |
"on pending" | "on ready" | "auto pending" | "auto ready") + lim_preview_passes
(pass counter), and the persisted lim_preview.
Driver: the panel is positioned from the pixel-located Apply button and the
premium layout math (ComputeLimiterLayout @ 480x266, scale 1.0).
"""
from __future__ import annotations

import time
from pathlib import Path

import numpy as np

from conftest import (SELECT_ITEM0, assert_no_loading, capture, clear_project, click_client,
                      client_size, ensure_window, insert_item_unselected, key_sync,
                      locate_apply_button, perf_media_dir, send_command, wait_audio_loaded,
                      wait_loaded, write_long_wav)

CM_APPLY_LIMITER = 2176          # edit_view.h ContextMenuID (compiled enum)
VK_ESCAPE = 0x1B
SHOTS = Path("/tmp/sneakpeak-reaproof-shots/limiter_preview")
GR_RED = (229, 72, 77)           # ui_theme.h kGrRed - gain reduction only

# --- premium panel geometry (ui_render.cpp ComputeLimiterLayout @ 480x266) ---
PANEL_W, PANEL_H, PAD, HEADER_H, FOOTER_H = 480.0, 266.0, 16.0, 44.0, 44.0
APPLY_CENTER = (PANEL_W - PAD - 50.0, PANEL_H - FOOTER_H + 9.0 + 13.0)   # L.apply = {w-pad-100, footTop+9, 100, 26}
CLOSE_CENTER = (PANEL_W - PAD - 9.0, HEADER_H * 0.5)                     # L.closeBtn = {w-pad-18, hMid-9, 18, 18}
PREVIEW_CENTER = (PANEL_W - PAD - 18.0 - 10.0 - 46.0, HEADER_H * 0.5)    # L.previewPill = {closeBtn.x-10-92, hMid-10, 92, 20}

OPEN = ('reaper.defer(function() reaper.Main_OnCommand('
        'reaper.NamedCommandLookup("_SneakPeak_OpenStandalone"), 0) end) return true')


def _open_standalone(sess, path: Path):
    sess.eval(f'reaper.SetExtState("SneakPeak", "open_path", "{path.as_posix()}", false) return true')
    sess.eval(OPEN)
    time.sleep(1.0)


def _state(sess) -> str:
    return str(sess.eval('return reaper.GetExtState("SneakPeak", "lim_preview_state")'))


def _passes(sess) -> int:
    return int(sess.eval('return reaper.GetExtState("SneakPeak", "lim_preview_passes")') or 0)


def _remembered(sess) -> str:
    return str(sess.eval('return reaper.GetExtState("SneakPeak", "lim_preview")'))


def _forget(sess):
    sess.eval('reaper.DeleteExtState("SneakPeak", "lim_preview", true) return true')


def _panel_origin(sess, tag: str) -> tuple[int, int]:
    SHOTS.mkdir(parents=True, exist_ok=True)
    try:
        sess.wait_until(lambda: locate_apply_button(sess, SHOTS / f"{tag}_probe.png") is not None, timeout=10)
    except Exception:
        raise AssertionError("the Hard Limiter panel did not open (no amber Apply on screen)")
    ax, ay = locate_apply_button(sess, SHOTS / f"{tag}_probe.png")
    return int(round(ax - APPLY_CENTER[0])), int(round(ay - APPLY_CENTER[1]))


def _band_px(sess, out: Path, panel_top: int) -> int:
    """kGrRed pixels in the capture ABOVE the panel: the gain-reduction band
    along the top of the waveform (the panel's own GR strip and MAX GR readout
    sit below panel_top, so they never count). Hue-direction match over the
    black waveform ground, like the lane oracles."""
    cap = capture(sess, out)
    cw, ch = client_size(sess)
    scale = cap.image.shape[1] / float(cw)
    titlebar = int(round(cap.height - ch * scale))
    img = cap.image[:titlebar + int(panel_top * scale), :, :3].astype(float)
    sat = img.max(axis=2) - img.min(axis=2)
    norms = img / np.maximum(np.linalg.norm(img, axis=2, keepdims=True), 1.0)
    ref = np.array(GR_RED, float)
    ref /= np.linalg.norm(ref)
    return int((((norms @ ref) > 0.995) & (sat > 40)).sum())


def _push(sess):
    sess.eval('reaper.SetExtState("SneakPeak", "lim_gain", "6000", true) return true')   # +6 dB into -1 dBTP


def _cleanup(sess):
    sess.eval('reaper.SetExtState("SneakPeak", "lim_gain", "0", true) return true')
    _forget(sess)
    if _state(sess) not in ("", "closed"):
        key_sync(sess, VK_ESCAPE, settle=0.5)


def test_long_standalone_opens_without_a_pass_and_the_pill_starts_it(sess):
    """6-minute stereo Standalone file, +6 dB: the panel opens with no pass and
    no band for 20 s; the PREVIEW pill runs the pass (band + readouts), a
    second press drops it again, and the choice survives close + reopen."""
    media = write_long_wav(perf_media_dir() / "limprev_6min_stereo.wav", minutes=6)
    clear_project(sess)
    ensure_window(sess)
    _open_standalone(sess, media)
    wait_audio_loaded(sess, media.stem, timeout=120)
    _forget(sess)
    _push(sess)
    try:
        passes0 = _passes(sess)
        send_command(sess, CM_APPLY_LIMITER)
        px, py = _panel_origin(sess, "long")
        time.sleep(20.0)   # the control's automatic pass lands well within this
        band = _band_px(sess, SHOTS / "long_1_open.png", py)
        state = _state(sess)
        assert band < 200, f"the gain-reduction band was painted without a PREVIEW press ({band} red px, state {state!r})"
        assert state == "off", f"a long file must open with the preview off, state {state!r}"
        assert _passes(sess) == passes0, f"a preview pass ran without a PREVIEW press ({_passes(sess) - passes0})"

        pill = (px + int(PREVIEW_CENTER[0]), py + int(PREVIEW_CENTER[1]))
        click_client(sess, *pill)
        sess.wait_until(lambda: _state(sess) == "on ready", timeout=120)
        assert _passes(sess) > passes0
        band = _band_px(sess, SHOTS / "long_2_on.png", py)
        assert band > 800, f"PREVIEW on: no gain-reduction band ({band} red px)"
        assert _remembered(sess) == "1", f"the choice was not remembered: lim_preview {_remembered(sess)!r}"

        click_client(sess, *pill)
        sess.wait_until(lambda: _state(sess) == "off", timeout=5)
        band = _band_px(sess, SHOTS / "long_3_off.png", py)
        assert band < 200, f"PREVIEW off again: the band stayed ({band} red px)"
        assert _remembered(sess) == "0"

        click_client(sess, *pill)
        sess.wait_until(lambda: _state(sess) == "on ready", timeout=120)
        click_client(sess, px + int(CLOSE_CENTER[0]), py + int(CLOSE_CENTER[1]))
        sess.wait_until(lambda: _state(sess) == "closed", timeout=5)
        passes1 = _passes(sess)
        send_command(sess, CM_APPLY_LIMITER)
        sess.wait_until(lambda: _state(sess) == "on ready" and _passes(sess) > passes1, timeout=120)
        capture(sess, SHOTS / "long_4_reopened.png")
    finally:
        _cleanup(sess)


def test_short_file_previews_at_once_with_no_pill(sess):
    """A 30 s file keeps the instant preview: the pass runs on open, the band
    appears, the mirror says "auto" (no pill)."""
    media = write_long_wav(perf_media_dir() / "limprev_30s_stereo.wav", minutes=0.5)
    clear_project(sess)
    ensure_window(sess)
    _open_standalone(sess, media)
    wait_audio_loaded(sess, media.stem, timeout=60)
    _forget(sess)
    _push(sess)
    try:
        passes0 = _passes(sess)
        send_command(sess, CM_APPLY_LIMITER)
        px, py = _panel_origin(sess, "short")
        sess.wait_until(lambda: _state(sess) == "auto ready", timeout=30)
        assert _passes(sess) > passes0
        band = _band_px(sess, SHOTS / "short_1_open.png", py)
        assert band > 800, f"short file: no gain-reduction band after the automatic pass ({band} red px)"
    finally:
        _cleanup(sess)


def test_long_item_opens_the_panel_without_decoding_until_the_pill(sess):
    """A 6-minute item (lazy: over the 10M-frame cap): the panel opens with no
    decode and no pass (no band after 20 s); the pill starts the load and
    then the pass (a pass in ITEM mode needs the landed buffer)."""
    media = write_long_wav(perf_media_dir() / "limprev_6min_item.wav", minutes=6)
    clear_project(sess)
    insert_item_unselected(sess, media)
    ensure_window(sess)
    sess.eval(SELECT_ITEM0)
    wait_loaded(sess, media.stem, timeout=60)
    assert_no_loading(sess, 2.0)   # precondition: a lazy item does not decode on its own
    _forget(sess)
    _push(sess)
    try:
        passes0 = _passes(sess)
        send_command(sess, CM_APPLY_LIMITER)
        px, py = _panel_origin(sess, "item")
        assert_no_loading(sess, 20.0)
        band = _band_px(sess, SHOTS / "item_1_open.png", py)
        state = _state(sess)
        assert band < 200, f"the lazy item was decoded and previewed without a PREVIEW press ({band} red px, state {state!r})"
        assert state == "off", f"a long item must open with the preview off, state {state!r}"
        assert _passes(sess) == passes0
        click_client(sess, px + int(PREVIEW_CENTER[0]), py + int(PREVIEW_CENTER[1]))
        wait_audio_loaded(sess, media.stem, timeout=180)
        sess.wait_until(lambda: _state(sess) == "on ready", timeout=120)
        assert _passes(sess) > passes0
        band = _band_px(sess, SHOTS / "item_2_on.png", py)
        assert band > 800, f"PREVIEW on the item: no gain-reduction band ({band} red px)"
    finally:
        _cleanup(sess)
