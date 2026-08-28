"""SneakPeak ReaProof harness (shared fixtures + drivers).

Runs the built extension inside ReaProof's isolated, pinned REAPER and drives it
through REAPER's own API (Lua bridge) + in-process window messages. Ground truth
is always read back a DIFFERENT way than it was set (track audio accessor,
envelope evaluation, project state) - never SneakPeak's own internal state.

Run (from the project's ReaProof copy):
  cd /Volumes/@Basic/Projekty/EditView/reaproof
  PYTHONPATH=src LC_ALL=en_US.UTF-8 LC_NUMERIC=C TZ=UTC \
    python3 -m pytest -v -s -p reaproof.runner.pytest_plugin \
    /Volumes/@Basic/Projekty/EditView/cpp/tests/reaproof --reaproof-repeat=2

Subject override: SNEAKPEAK_DYLIB=/path/to/reaper_sneakpeak.dylib (negative
controls run the same specs against an OLD binary and must go RED).
"""
from __future__ import annotations

import os
import sys
from pathlib import Path

import numpy as np
import pytest
import soundfile as sf

REAPROOF_SRC = Path(__file__).resolve().parents[3] / "reaproof" / "src"
if REAPROOF_SRC.exists() and str(REAPROOF_SRC) not in sys.path:
    sys.path.insert(0, str(REAPROOF_SRC))

from reaproof.runner.session import ReaperSession  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
DYLIB = Path(os.environ.get("SNEAKPEAK_DYLIB", REPO / "build" / "reaper_sneakpeak.dylib"))

# Context-menu command IDs (edit_view.h enum ContextMenuID, CM_UNDO = 2000).
# Verified by parsing the enum on 2026-08-28; re-derive if the enum changes.
CM_APPLY_DYNAMICS = 2058
CM_SHOW_DYNAMICS = 2053

WINDOW_TITLE = "SneakPeak"


def pytest_collection_modifyitems(config, items):
    if not DYLIB.exists():
        skip = pytest.mark.skip(reason=f"subject not built: {DYLIB}")
        for it in items:
            it.add_marker(skip)


@pytest.fixture(scope="module")
def sess():
    with ReaperSession("sneakpeak", extensions=[DYLIB]) as s:
        if not bool(s.eval('return reaper.APIExists("JS_Window_Find")')):
            pytest.skip("js_ReaScriptAPI not available in isolated profile")
        yield s


# --------------------------------------------------------------------------
# Drivers
# --------------------------------------------------------------------------
def toggle_window(s):
    s.eval('reaper.Main_OnCommand(reaper.NamedCommandLookup("_SneakPeak_Toggle"), 0)')


def window_handle_lua() -> str:
    """Lua expression yielding our dialog HWND (nil if absent)."""
    return f'reaper.JS_Window_Find("{WINDOW_TITLE}", false)'


def window_visible(s) -> bool:
    return bool(s.eval(f"local h = {window_handle_lua()} "
                       "if h and reaper.JS_Window_IsVisible(h) then return true end "
                       "return false"))


def ensure_window(s, timeout: float = 10.0):
    if not window_visible(s):
        toggle_window(s)
    s.wait_until(lambda: window_visible(s), timeout=timeout)


def send_command(s, cmd_id: int):
    """WM_COMMAND to our dialog - exactly what the context menu would send."""
    ok = s.eval(f"local h = {window_handle_lua()} if not h then return false end "
                f'reaper.JS_WindowMessage_Post(h, "WM_COMMAND", {int(cmd_id)}, 0, 0, 0) '
                "return true")
    if not ok:
        raise RuntimeError("SneakPeak window not found")


def wait_loaded(s, name_substring: str, timeout: float = 20.0):
    """SneakPeak retitles its window "SneakPeak: <source name>" once the item is
    loaded - an observable readiness signal that needs no internal state."""
    s.wait_until(lambda: name_substring in str(s.eval(
        f"local h = {window_handle_lua()} if not h then return '' end "
        "return reaper.JS_Window_GetTitle(h)")), timeout=timeout)


def client_size(s) -> tuple[int, int]:
    l, t, r, b = s.eval(f"local h = {window_handle_lua()} "
                        "local _, l, t, r, b = reaper.JS_Window_GetClientRect(h) "
                        "return {l, t, r, b}")
    return abs(r - l), abs(b - t)


def capture(s, out: Path):
    from reaproof.observe.visual.capture import capture_window_macos
    return capture_window_macos(s.handle.pid, WINDOW_TITLE, out)


def locate_apply_button(s, out: Path) -> tuple[int, int] | None:
    """Pixel-locate the Dynamics panel's amber Apply button (kAmber fill,
    84x24 - by far the largest amber blob) in a window capture; returns its
    centre in CLIENT coordinates or None when no panel is showing."""
    from scipy import ndimage
    cap = capture(s, out)
    img = cap.image.astype(int)
    r, g, b = img[..., 0], img[..., 1], img[..., 2]
    mask = (r > 170) & (g > 110) & (g < 200) & (b < 90)
    labels, n = ndimage.label(mask)
    if n == 0:
        return None
    sizes = ndimage.sum(mask, labels, range(1, n + 1))
    best = int(np.argmax(sizes)) + 1
    if sizes[best - 1] < 800:            # a filled 84x24 button is ~2000 px
        return None
    ys, xs = np.nonzero(labels == best)
    cw, ch = client_size(s)
    titlebar = cap.height - ch            # capture includes the window frame
    return int(xs.mean()), int(ys.mean()) - titlebar


def click_client(s, x: int, y: int):
    from reaproof.observe.input import bridge_click
    bridge_click(s, WINDOW_TITLE, x, y)


def apply_dynamics(s, shots_dir: Path, *, tries: int = 4, timeout: float = 8.0):
    """Open the Dynamics panel (context-menu command) and press its Apply
    button - the real user path - until the take envelope carries a curve."""
    shots_dir.mkdir(parents=True, exist_ok=True)
    send_command(s, CM_APPLY_DYNAMICS)
    for i in range(tries):
        try:
            s.wait_until(lambda: locate_apply_button(s, shots_dir / f"panel{i}.png") is not None,
                         timeout=timeout)
        except Exception:
            send_command(s, CM_APPLY_DYNAMICS)
            continue
        xy = locate_apply_button(s, shots_dir / f"panel{i}.png")
        click_client(s, *xy)
        try:
            s.wait_until(lambda: len(take_envelope_points(s)) > 4, timeout=timeout)
            capture(s, shots_dir / "applied.png")
            return
        except Exception:
            continue
    raise AssertionError("Apply Dynamics never produced an envelope curve")


def clear_project(s):
    """Empty the live project WITHOUT File>New (which raises a save-changes
    modal on a dirty project and would hang the bridge)."""
    s.eval("""
      reaper.Main_OnCommand(1016, 0)
      reaper.SelectAllMediaItems(0, false)
      for i = reaper.CountTracks(0) - 1, 0, -1 do
        reaper.DeleteTrack(reaper.GetTrack(0, i))
      end
      reaper.UpdateArrange()
      return true
    """)
    s.wait_until(lambda: int(s.eval("return reaper.CountTracks(0)")) == 0, timeout=10)


def insert_item(s, wav: Path, *, position: float = 0.0, playrate: float = 1.0) -> dict:
    """One track, one item from `wav` at `position`; playrate applied with the
    item length rescaled the way REAPER does (source length / rate)."""
    got = s.eval(f"""
      reaper.InsertTrackAtIndex(0, false)
      local tr = reaper.GetTrack(0, 0)
      reaper.SetOnlyTrackSelected(tr)
      reaper.SetEditCurPos({position}, false, false)
      reaper.InsertMedia("{wav.as_posix()}", 0)
      local it = reaper.GetTrackMediaItem(tr, 0)
      if not it then return nil end
      local tk = reaper.GetActiveTake(it)
      local src = reaper.GetMediaItemTake_Source(tk)
      local srclen = reaper.GetMediaSourceLength(src)
      reaper.SetMediaItemTakeInfo_Value(tk, "B_PPITCH", 0)
      reaper.SetMediaItemTakeInfo_Value(tk, "D_PLAYRATE", {playrate})
      reaper.SetMediaItemLength(it, srclen / {playrate}, false)
      reaper.SetMediaItemInfo_Value(it, "D_POSITION", {position})
      reaper.SetMediaItemSelected(it, true)
      reaper.UpdateArrange()
      return {{ pos = reaper.GetMediaItemInfo_Value(it, "D_POSITION"),
               len = reaper.GetMediaItemInfo_Value(it, "D_LENGTH"),
               rate = reaper.GetMediaItemTakeInfo_Value(tk, "D_PLAYRATE"),
               srclen = srclen }}
    """)
    assert got, "InsertMedia produced no item"
    return got


def take_envelope_points(s) -> list[tuple[float, float]]:
    """(time, value) of the take Volume envelope points on track 0 / item 0
    (time in REAPER's take-envelope timebase, value raw)."""
    return [tuple(p) for p in s.eval("""
      local it = reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), 0)
      local tk = reaper.GetActiveTake(it)
      local env = reaper.GetTakeEnvelopeByName(tk, "Volume")
      if not env then return {} end
      local out = {}
      for i = 0, reaper.CountEnvelopePoints(env) - 1 do
        local _, t, v = reaper.GetEnvelopePoint(env, i)
        out[#out + 1] = { t, v }
      end
      return out
    """)]


def track_rms_windows(s, windows: list[tuple[float, float]], sr: int = 44100) -> list[float]:
    """RMS of the TRACK output (item volume, fades, take envelope applied by
    REAPER itself) over [start, end) project-time windows. This is the ground
    truth for "what the listener hears" - independent of SneakPeak."""
    wins = ", ".join(f"{{{a}, {b}}}" for a, b in windows)
    return [float(x) for x in s.eval(f"""
      local tr = reaper.GetTrack(0, 0)
      local acc = reaper.CreateTrackAudioAccessor(tr)
      local out = {{}}
      for _, w in ipairs({{ {wins} }}) do
        local n = math.floor((w[2] - w[1]) * {sr})
        local buf = reaper.new_array(n)
        buf.clear()
        reaper.GetAudioAccessorSamples(acc, {sr}, 1, w[1], n, buf)
        local acc2 = 0.0
        for i = 1, n do local v = buf[i]; acc2 = acc2 + v * v end
        out[#out + 1] = math.sqrt(acc2 / n)
      end
      reaper.DestroyAudioAccessor(acc)
      return out
    """)]


def db(x: float) -> float:
    return 20.0 * np.log10(max(float(x), 1e-12))


# --------------------------------------------------------------------------
# Signals
# --------------------------------------------------------------------------
def write_floor_burst_wav(path: Path, *, seconds: float = 10.0, sr: int = 44100,
                          floor_amp: float = 0.03, burst_amp: float = 0.9,
                          burst: tuple[float, float] = (4.0, 5.0), freq: float = 440.0):
    """Steady tone at floor_amp with one loud burst in [burst) - a signal
    whose compression envelope has ONE unambiguous dip."""
    n = int(seconds * sr)
    t = np.arange(n) / sr
    y = np.sin(2 * np.pi * freq * t) * floor_amp
    b0, b1 = int(burst[0] * sr), int(burst[1] * sr)
    y[b0:b1] = np.sin(2 * np.pi * freq * t[b0:b1]) * burst_amp
    sf.write(str(path), y.astype(np.float32), sr)
