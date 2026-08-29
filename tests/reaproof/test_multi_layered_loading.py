"""Layered Multi-item modes must paint a still-loading layer at full width.

v2.5 promises the Multi-item view paints from REAPER's peak files at once
(the layers decode in the background). MIX does; the Layered modes computed
the visible column range of a layer from `audioFrameCount`, which is 0 until
the loader installs the samples - so a loading layer drew ONE column while
its .reapeaks peaks (ComputeLayerPeaksFromSDK) were all there.
Oracle: two long items (20 + 26 min, both from 0 s) opened as Multi-item,
Layered (per Item) while the title still reads "Loading item audio...": the
columns carrying a layer hue must cover the waveform width. Then, loaded,
the same. Control (8b9dbf9): 1-2 hue columns while loading.
"""
from __future__ import annotations

import time
from pathlib import Path

import numpy as np

from conftest import (capture, clear_project, client_size, command_sync, ensure_window, insert_item,
                      insert_item_unselected, mode_from_capture, perf_media_dir, wait_audio_loaded,
                      wait_main_thread_idle, window_title, write_long_wav)

SHOTS = Path("/tmp/sneakpeak-reaproof-shots/multi_loading")
CM_MULTI_MODE_LAYERED = 2038         # edit_view.h ContextMenuID (evaluate the enum, never count)
GREEN, CYAN = (0, 200, 80), (0, 200, 200)   # kLayerColors[0] / [1]
DB_SCALE_WIDTH = 42


def _hue_cols(img: np.ndarray, rgb: tuple[int, int, int], y0: int, y1: int, x1: int) -> np.ndarray:
    """Capture columns (< x1) holding >= 2 pixels of the hue inside rows y0..y1."""
    band = img[y0:y1, :x1].astype(float)
    sat = band.max(axis=2) - band.min(axis=2)
    norms = band / np.maximum(np.linalg.norm(band, axis=2, keepdims=True), 1.0)
    ref = np.array(rgb, float)
    ref /= np.linalg.norm(ref)
    hit = ((norms @ ref) > 0.995) & (sat > 40)
    return np.nonzero(hit.sum(axis=0) >= 2)[0]


def _layer_columns(sess, out: Path) -> tuple[int, int, str]:
    """(columns with any layer hue, waveform width, title at capture time)."""
    title = window_title(sess)
    cap = capture(sess, out)
    cw, ch = client_size(sess)
    scale = cap.image.shape[1] / float(cw)
    titlebar = int(round(cap.height - ch * scale))
    y0, y1 = titlebar + int(50 * scale), titlebar + int(340 * scale)   # the waveform lane (800x400)
    x1 = int((cw - DB_SCALE_WIDTH) * scale)
    cols = np.union1d(_hue_cols(cap.image, GREEN, y0, y1, x1), _hue_cols(cap.image, CYAN, y0, y1, x1))
    return int(len(cols)), int(x1), title


def test_layered_paints_a_loading_layer_at_full_width(sess):
    a = write_long_wav(perf_media_dir() / "long26min_stereo.wav", minutes=26)
    b = write_long_wav(perf_media_dir() / "long20min_stereo.wav", minutes=20)
    clear_project(sess)
    insert_item_unselected(sess, a)
    insert_item(sess, b, position=0.0)          # a new track 0; `a` is on track 1
    sess.eval("reaper.SelectAllMediaItems(0, false) reaper.UpdateArrange() return true")
    ensure_window(sess)
    sess.eval("reaper.SelectAllMediaItems(0, true) reaper.UpdateArrange() return true")
    sess.wait_until(lambda: mode_from_capture(sess, SHOTS / "multi.png") == "MULTI", timeout=30)
    command_sync(sess, CM_MULTI_MODE_LAYERED, settle=0.3)
    n_loading, w, title = _layer_columns(sess, SHOTS / "layered_loading.png")
    print(f"\n[loading] title {title!r}: {n_loading} of {w} columns carry a layer hue")
    assert "Loading" in title, (f"the layers had finished loading before the capture ({title!r}) - "
                                f"the fixtures are too short to observe the loading paint")
    assert n_loading >= 0.9 * w, (f"a loading layer paints {n_loading} of {w} columns in Layered mode - "
                                  f"its .reapeaks peaks should cover the item")
    wait_audio_loaded(sess, "long", timeout=600)   # 46 min of stereo decode eagerly: > 180 s on the emulated Windows VM
    wait_main_thread_idle(sess, timeout=240)       # ...and its Multi-item paint keeps that VM's main thread busy for minutes
    n_loaded, w2, title2 = _layer_columns(sess, SHOTS / "layered_loaded.png")
    print(f"[loading] loaded ({title2!r}): {n_loaded} of {w2} columns")
    assert n_loaded >= 0.9 * w2, f"the loaded layers paint {n_loaded} of {w2} columns"
