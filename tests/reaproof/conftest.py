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


# --------------------------------------------------------------------------
# Performance probes (main-thread stall via the bridge heartbeat)
# --------------------------------------------------------------------------
def _heartbeat_t(s) -> float | None:
    import json
    try:
        return float(json.loads(s.bridge.heartbeat.read_text(encoding="utf-8",
                                                             errors="replace")).get("t"))
    except (OSError, ValueError, TypeError):
        return None


def window_title(s) -> str:
    return str(s.eval(f"local h = {window_handle_lua()} if not h then return '' end "
                      "return reaper.JS_Window_GetTitle(h)", hang_timeout=120))


def _cg_window_title(pid: int) -> str:
    """Our window's title straight from the window server - no bridge round
    trip, so it can be polled at kHz rates without touching REAPER."""
    from reaproof.observe.visual.capture import _find_window_macos
    _, name, _ = _find_window_macos(pid, WINDOW_TITLE)
    return name or ""


def measure_after(s, action_lua: str, *, loaded_marker: str, first_marker: str = "SneakPeak: ",
                  max_wait: float = 120.0, quiet: float = 1.5) -> dict:
    """Run `action_lua` and measure how REAPER's main thread behaves afterwards.

    A probe THREAD samples two bridge-free observables every few ms:
      - heartbeat.json (tick, t): written by the bridge's defer loop at the
        top of every REAPER main-loop tick, `t` = reaper.time_precise()
      - the SneakPeak window title via CGWindowList
    The action itself returns reaper.time_precise() so everything is placed
    on REAPER's own clock (a bridge round trip costs ~0.5 s and would
    otherwise pollute the numbers). Returns:
      max_stall  longest gap between consecutive heartbeat ticks after the
                 action (s) = longest main-thread freeze
      t_first    action -> title left idle ("SneakPeak: ..." incl. Loading)
      t_loaded   action -> title shows the source name with no "Loading"
    """
    import threading
    import time as _t
    import json as _json

    hb_path = s.bridge.heartbeat
    pid = s.handle.pid
    samples: list[tuple[float, int, float]] = []     # (wall, tick, t)
    titles: list[tuple[float, str]] = []              # (wall, title) on change
    stop = threading.Event()

    def probe():
        last_tick = None
        last_title = None
        n = 0
        while not stop.is_set():
            try:
                d = _json.loads(hb_path.read_text(encoding="utf-8", errors="replace"))
                tick, t = int(d["tick"]), float(d["t"])
                if tick != last_tick:
                    samples.append((_t.monotonic(), tick, t))
                    last_tick = tick
            except (OSError, ValueError, KeyError, TypeError):
                pass
            n += 1
            if n % 4 == 0:                            # ~every 12 ms
                title = _cg_window_title(pid)
                if title != last_title:
                    titles.append((_t.monotonic(), title))
                    last_title = title
            _t.sleep(0.003)

    th = threading.Thread(target=probe, daemon=True)
    th.start()
    _t.sleep(0.2)                                     # settle: a few idle ticks
    wall0 = _t.monotonic()
    action_t = float(s.eval(action_lua.replace("return true", "return reaper.time_precise()"),
                            hang_timeout=120))
    # wait for the loaded state + a quiet tail, on wall clock
    t_end = None
    while _t.monotonic() - wall0 < max_wait:
        if titles and loaded_marker in titles[-1][1] and "Loading" not in titles[-1][1]:
            if t_end is None:
                t_end = _t.monotonic()
            elif _t.monotonic() - t_end > quiet:
                break
        _t.sleep(0.02)
    stop.set()
    th.join(timeout=2)

    # wall -> REAPER clock via the nearest heartbeat before the event
    def to_reaper(wall):
        best = None
        for w, _, t in samples:
            if w <= wall:
                best = t + (wall - w)
            else:
                break
        return best

    max_stall = 0.0
    for (w0, k0, t0), (w1, k1, t1) in zip(samples, samples[1:]):
        if t1 <= action_t or k1 <= k0:
            continue
        gap = (t1 - t0) / (k1 - k0)                   # per missed-sample fairness
        if gap > max_stall:
            max_stall = gap

    t_first = t_loaded = None
    for w, title in titles:
        tr = to_reaper(w)
        if tr is None or tr < action_t:
            continue
        if t_first is None and first_marker in title:
            t_first = tr - action_t
        if t_loaded is None and loaded_marker in title and "Loading" not in title:
            t_loaded = tr - action_t
    return {"max_stall": round(max_stall, 3),
            "t_first": None if t_first is None else round(t_first, 3),
            "t_loaded": None if t_loaded is None else round(t_loaded, 3),
            "ticks": len(samples)}


def perf_media_dir() -> Path:
    d = Path("/tmp/sneakpeak-perf-media")
    d.mkdir(parents=True, exist_ok=True)
    return d


def write_long_wav(path: Path, *, minutes: float, sr: int = 44100, channels: int = 2):
    """Music-like long file (sines + slow AM so peaks vary), 16-bit PCM."""
    if path.exists():
        return path
    n = int(minutes * 60 * sr)
    chunk = sr * 10
    with sf.SoundFile(str(path), "w", samplerate=sr, channels=channels, subtype="PCM_16") as f:
        for start in range(0, n, chunk):
            m = min(chunk, n - start)
            t = (np.arange(m) + start) / sr
            am = 0.5 + 0.5 * np.sin(2 * np.pi * 0.1 * t)
            y = 0.6 * am * (np.sin(2 * np.pi * 220 * t) + 0.5 * np.sin(2 * np.pi * 331 * t))
            block = np.stack([y] * channels, axis=1).astype(np.float32)
            f.write(block)
    return path


def write_long_aac(path: Path, *, minutes: float, sr: int = 48000):
    """17-minute AAC (m4a) like the forum #103 item: compressed AND at a
    samplerate different from the 44.1k project -> decode + resample on load."""
    if path.exists():
        return path
    import subprocess
    tmp = path.with_suffix(".src.wav")
    write_long_wav(tmp, minutes=minutes, sr=sr, channels=2)
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", str(tmp),
                    "-c:a", "aac", "-b:a", "128k", str(path)], check=True)
    tmp.unlink(missing_ok=True)
    return path


def insert_item_unselected(s, media: Path, *, position: float = 0.0):
    info = insert_item(s, media, position=position)
    s.eval("reaper.SelectAllMediaItems(0, false) reaper.UpdateArrange() return true")
    return info


SELECT_ITEM0 = ("local it = reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), 0) "
                "reaper.SetMediaItemSelected(it, true) reaper.UpdateArrange() return true")
DESELECT_ALL = "reaper.SelectAllMediaItems(0, false) reaper.UpdateArrange() return true"
