"""SneakPeak ReaProof harness (shared fixtures + drivers).

Runs the built extension inside ReaProof's isolated, pinned REAPER and drives it
through REAPER's own API (Lua bridge) + in-process window messages. Ground truth
is always read back a DIFFERENT way than it was set (track audio accessor,
envelope evaluation, project state) - never SneakPeak's own internal state.

Run (from the project's ReaProof copy, a sibling of the cpp/ repo):
  cd ../reaproof
  PYTHONPATH=src LC_ALL=en_US.UTF-8 LC_NUMERIC=C TZ=UTC \
    python3 -m pytest -v -s -p reaproof.runner.pytest_plugin \
    ../cpp/tests/reaproof --reaproof-repeat=2

Subject override: SNEAKPEAK_DYLIB=/path/to/reaper_sneakpeak.dylib (negative
controls run the same specs against an OLD binary and must go RED).
"""
from __future__ import annotations

import os
import shutil
import sys
import time
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
CM_TRACK_VIEW = 2041          # working set (SET view) from the selected items

WINDOW_TITLE = "SneakPeak"


def pytest_collection_modifyitems(config, items):
    if not DYLIB.exists():
        skip = pytest.mark.skip(reason=f"subject not built: {DYLIB}")
        for it in items:
            it.add_marker(skip)


def clear_mac_reopen_prompt():
    """macOS: after a REAPER crash (or the harness SIGKILL on a timeout) every
    launch of the bundle id shows 'unexpectedly quit while reopening windows -
    Reopen?' and the bridge never comes up; a timed-out launch is killed, which
    re-arms the prompt - a loop (seen 2026-08-29 after an A6 crash). REAPER
    manages its own windows, so NSApp state restoration is expendable: tell it
    to ignore the persisted state (user default, harmless for the real REAPER;
    undo with `defaults delete com.cockos.reaper ApplePersistenceIgnoreState`)."""
    if sys.platform == "darwin":
        import subprocess
        subprocess.run(["defaults", "write", "com.cockos.reaper", "ApplePersistenceIgnoreState", "-bool", "YES"],
                       capture_output=True)


def release_stuck_modifiers():
    """macOS: a key event posted WITH a modifier flag but without the modifier
    key's own down/up leaves that modifier held in the CG session state for
    good (measured 2026-08-29: Cmd stuck after test_input's Cmd+Y; OnKeyDown
    then reads GetAsyncKeyState(VK_CONTROL) as held and Delete became
    Silence). Post a key-up for every modifier the session still reports."""
    if sys.platform != "darwin":
        return
    import Quartz
    state = Quartz.kCGEventSourceStateCombinedSessionState
    for keycode in (55, 56, 58, 59):      # Cmd, Shift, Option, Control
        if Quartz.CGEventSourceKeyState(state, keycode):
            ev = Quartz.CGEventCreateKeyboardEvent(None, keycode, False)
            Quartz.CGEventSetFlags(ev, 0)
            Quartz.CGEventPost(Quartz.kCGHIDEventTap, ev)
            time.sleep(0.05)


@pytest.fixture(scope="module")
def sess():
    clear_mac_reopen_prompt()
    release_stuck_modifiers()
    with ReaperSession("sneakpeak", extensions=[DYLIB]) as s:
        if not bool(s.eval('return reaper.APIExists("JS_Window_Find")')):
            pytest.skip("js_ReaScriptAPI not available in isolated profile")
        s.eval(SP_WINDOW_LUA)
        yield s


# --------------------------------------------------------------------------
# Drivers
# --------------------------------------------------------------------------
def toggle_window(s):
    s.eval('reaper.Main_OnCommand(reaper.NamedCommandLookup("_SneakPeak_Toggle"), 0)')


# Our dialog HWND (nil if absent), defined ONCE per session (Lua globals
# persist across evals). Scans EVERY window whose title contains "SneakPeak"
# and prefers the visible one with our own title shape: a dismissed native
# MessageBox ("SneakPeak - Destructive Operation") lingers as an invisible,
# title-less window that a plain JS_Window_Find returns first, hiding the live
# window for the rest of the session (finding F9).
SP_WINDOW_LUA = f"""
  SP_WINDOW = function()
    local n, list = reaper.JS_Window_ListFind("{WINDOW_TITLE}", false)
    if not n or n <= 0 then return nil end
    local best = nil
    for addr in string.gmatch(list, "[^,]+") do
      local h = reaper.JS_Window_HandleFromAddress(addr)
      local t = reaper.JS_Window_GetTitle(h) or ""
      if t:match("^%*? ?SneakPeak") and not t:find("Operation") then
        if reaper.JS_Window_IsVisible(h) then return h end
        best = best or h
      end
    end
    return best
  end
  return true"""


def window_handle_lua() -> str:
    """Lua expression yielding our dialog HWND (nil if absent) - see SP_WINDOW_LUA."""
    return "SP_WINDOW()"


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


def assert_no_loading(s, seconds: float = 2.0) -> str:
    """8g: a lazy item must never retitle to "Loading item audio..." on its own.
    Polls the window title for `seconds`; returns the last title seen."""
    import time as _t
    t_end = _t.monotonic() + seconds
    last = ""
    while _t.monotonic() < t_end:
        last = window_title(s)
        assert "Loading" not in last, f"the buffer decoded without being asked: {last!r}"
        _t.sleep(0.05)
    return last


def rss_mb(s) -> float:
    """REAPER's resident set (MB) - the memory observable for the buffer specs.
    Windows: GetProcessMemoryInfo working set (there is no ps)."""
    if sys.platform == "win32":
        import ctypes
        from ctypes import wintypes

        class PMC(ctypes.Structure):
            _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                        ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                        ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]

        h = ctypes.windll.kernel32.OpenProcess(0x0410, False, s.handle.pid)  # QUERY_INFORMATION | VM_READ
        if not h:
            raise RuntimeError(f"OpenProcess failed for pid {s.handle.pid}")
        try:
            pmc = PMC(); pmc.cb = ctypes.sizeof(PMC)
            if not ctypes.windll.psapi.GetProcessMemoryInfo(h, ctypes.byref(pmc), pmc.cb):
                raise RuntimeError("GetProcessMemoryInfo failed")
            return pmc.WorkingSetSize / (1024.0 * 1024.0)
        finally:
            ctypes.windll.kernel32.CloseHandle(h)
    import subprocess as _sp
    out = _sp.check_output(["ps", "-o", "rss=", "-p", str(s.handle.pid)])
    return int(out.strip()) / 1024.0


def wait_audio_loaded(s, name_substring: str, timeout: float = 90.0, stable: float = 1.0):
    """Loaded item AND the background loader finished (no 'Loading' in the
    title) - held for `stable` seconds: the plain name is on the title for a
    moment between the select and the loader's first progress retitle."""
    import time as _t
    since = None
    def done():
        nonlocal since
        t = str(s.eval(f"local h = {window_handle_lua()} if not h then return '' end "
                       "return reaper.JS_Window_GetTitle(h)"))
        ok = name_substring in t and "Loading" not in t
        if not ok:
            since = None
            return False
        since = since or _t.monotonic()
        return _t.monotonic() - since >= stable
    s.wait_until(done, timeout=timeout + stable)


def client_size(s) -> tuple[int, int]:
    l, t, r, b = s.eval(f"local h = {window_handle_lua()} "
                        "local _, l, t, r, b = reaper.JS_Window_GetClientRect(h) "
                        "return {l, t, r, b}")
    return abs(r - l), abs(b - t)


class _WinCapture:
    """Duck-typed stand-in for reaproof's Capture on Windows: the pixel
    locators here only read .image (HxWx3 RGB) and .height."""
    def __init__(self, image, path, title):
        self.image = image
        self.height, self.width = image.shape[:2]
        self.path = path
        self.window_title = title


def _capture_window_windows(pid: int, out_path: Path, *, settle: float = 0.4,
                            retries: int = 20):
    """PrintWindow(PW_RENDERFULLCONTENT) of our top-level - SneakPeak renders
    with plain GDI, which PrintWindow captures faithfully; includes the frame,
    like screencapture on macOS (the titlebar offset math stays identical)."""
    import ctypes
    import time as _t
    from ctypes import wintypes
    import numpy as _np
    from PIL import Image as _Image
    u32, g32 = ctypes.windll.user32, ctypes.windll.gdi32
    hwnd = None
    deadline = _t.monotonic() + retries * 0.25
    while _t.monotonic() < deadline and not hwnd:
        found = []

        @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        def cb(h, _):
            wpid = wintypes.DWORD()
            u32.GetWindowThreadProcessId(h, ctypes.byref(wpid))
            if wpid.value == pid and u32.IsWindowVisible(h):
                buf = ctypes.create_unicode_buffer(512)
                u32.GetWindowTextW(h, buf, 512)
                if buf.value.lstrip("* ").startswith(WINDOW_TITLE) and "Operation" not in buf.value:
                    found.append((h, buf.value))
                    return False
            return True

        u32.EnumWindows(cb, 0)
        if found:
            hwnd, title = found[0]
            break
        _t.sleep(0.25)
    if not hwnd:
        raise RuntimeError(f"window not found (pid={pid}, title~='{WINDOW_TITLE}')")
    _t.sleep(settle)
    r = wintypes.RECT()
    u32.GetWindowRect(hwnd, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    hdcw = u32.GetWindowDC(hwnd)
    mem = g32.CreateCompatibleDC(hdcw)
    bmp = g32.CreateCompatibleBitmap(hdcw, w, h)
    old = g32.SelectObject(mem, bmp)
    try:
        u32.PrintWindow(hwnd, mem, 2)                 # PW_RENDERFULLCONTENT
        class BMIH(ctypes.Structure):
            _fields_ = [("biSize", wintypes.DWORD), ("biWidth", ctypes.c_long),
                        ("biHeight", ctypes.c_long), ("biPlanes", wintypes.WORD),
                        ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                        ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", ctypes.c_long),
                        ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", wintypes.DWORD),
                        ("biClrImportant", wintypes.DWORD)]
        bmi = BMIH(ctypes.sizeof(BMIH), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)  # top-down BGRA
        buf = (ctypes.c_ubyte * (w * h * 4))()
        g32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bmi), 0)
    finally:
        g32.SelectObject(mem, old)
        g32.DeleteObject(bmp)
        g32.DeleteDC(mem)
        u32.ReleaseDC(hwnd, hdcw)
    img = _np.frombuffer(buf, dtype=_np.uint8).reshape(h, w, 4)[..., [2, 1, 0]].copy()
    if img.std() < 1.0:
        raise RuntimeError("captured an all-black frame - PrintWindow returned nothing")
    _Image.fromarray(img).save(str(out_path))
    return _WinCapture(img, Path(out_path), title)


def capture(s, out: Path):
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    if sys.platform == "win32":
        return _capture_window_windows(s.handle.pid, out)
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
    """A click on OUR window through SP_WINDOW() (Sends) - see drag_client."""
    click_sync(s, x, y, settle=0.2)


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
      -- markers/regions too: a --reaproof-repeat pass must not inherit the
      -- previous pass's regions (One-Shot REGIONS slices every region it finds)
      for i = reaper.CountProjectMarkers(0) - 1, 0, -1 do
        reaper.DeleteProjectMarkerByIndex(0, i)
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
    trip, so it can be polled at kHz rates without touching REAPER.
    Windows: EnumWindows over the process's visible top-levels, matched by the
    "SneakPeak" prefix (also "* SneakPeak" when dirty) - same contract."""
    if sys.platform == "win32":
        import ctypes
        from ctypes import wintypes
        u32 = ctypes.windll.user32
        found = []

        @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        def cb(hwnd, _):
            wpid = wintypes.DWORD()
            u32.GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
            if wpid.value == pid and u32.IsWindowVisible(hwnd):
                buf = ctypes.create_unicode_buffer(512)
                u32.GetWindowTextW(hwnd, buf, 512)
                t = buf.value
                if t.lstrip("* ").startswith(WINDOW_TITLE) and "Operation" not in t:
                    found.append(t)
                    return False
            return True

        u32.EnumWindows(cb, 0)
        return found[0] if found else ""
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
    seen_loading = False
    for w, title in titles:
        tr = to_reaper(w)
        if tr is None or tr < action_t:
            continue
        if t_first is None and first_marker in title:
            t_first = tr - action_t
        if "Loading" in title:
            seen_loading = True
            t_loaded = None            # a plain title BEFORE the loader retitled was premature
        elif t_loaded is None and loaded_marker in title:
            t_loaded = tr - action_t
    # the loader only retitles once it has ticked; a plain-name title with no
    # "Loading" ever seen means the view loaded synchronously (short file)
    return {"max_stall": round(max_stall, 3),
            "t_first": None if t_first is None else round(t_first, 3),
            "t_loaded": None if t_loaded is None else round(t_loaded, 3),
            "seen_loading": seen_loading,
            "ticks": len(samples)}


def wait_main_thread_idle(s, timeout: float = 120.0, quiet: float = 0.5):
    """Block (without touching the bridge) until REAPER's defer loop ticks
    again and has done so regularly for `quiet` s - e.g. after a long
    synchronous destructive write."""
    import time as _t
    t0 = _t.monotonic()
    last = _heartbeat_t(s)
    last_change = _t.monotonic()
    first_change = None
    while _t.monotonic() - t0 < timeout:
        hb = _heartbeat_t(s)
        if hb is not None and hb != last:
            last = hb
            now = _t.monotonic()
            if first_change is None or now - last_change > 0.2:
                first_change = now      # (re)start the quiet window after any gap
            last_change = now
            if now - first_change >= quiet:
                return
        _t.sleep(0.01)
    raise TimeoutError("REAPER main thread did not come back")


def dismiss_native_modal(s, *, timeout: float = 15.0):
    """SneakPeak's destructive-op confirmation is a native MessageBox (an app-
    modal NSAlert on macOS): REAPER's defer loop - and with it the bridge -
    stops until it is answered. Wait for the heartbeat to stall, bring the
    isolated REAPER to the front and press Return (= the default 'Yes').
    Windows: the dialog is a plain top-level window in our own window station,
    so find it by caption and post WM_COMMAND/IDYES - no focus, no key events."""
    import subprocess
    import time as _t
    if sys.platform == "win32":
        import ctypes
        u32 = ctypes.windll.user32
        t0 = _t.monotonic()
        buf = ctypes.create_unicode_buffer(256)
        while _t.monotonic() - t0 < timeout:
            # every SneakPeak MessageBox (#32770): the destructive confirm, the
            # "Overwrite original file?" question, the error boxes. Our own
            # dialog window has no IDYES/IDOK control, so it never matches.
            h = u32.FindWindowExW(None, None, "#32770", None)
            while h:
                u32.GetWindowTextW(h, buf, 256)
                if buf.value.startswith("SneakPeak"):
                    if u32.GetDlgItem(h, 6):                        # a Yes/No question -> Yes
                        u32.PostMessageW(h, 0x0111, 6, 0)           # WM_COMMAND, IDYES
                        return True
                    if u32.GetDlgItem(h, 2) or u32.GetDlgItem(h, 1):  # an OK box (its button is IDCANCEL on Win11)
                        u32.PostMessageW(h, 0x0010, 0, 0)           # WM_CLOSE
                        return True
                h = u32.FindWindowExW(None, h, "#32770", None)
            _t.sleep(0.02)
        return False
    import Quartz
    t0 = _t.monotonic()
    last = _heartbeat_t(s)
    last_change = _t.monotonic()
    while _t.monotonic() - t0 < timeout:
        hb = _heartbeat_t(s)
        if hb is not None and hb != last:
            last, last_change = hb, _t.monotonic()
        if _t.monotonic() - last_change > 0.4:      # modal up: no ticks for 400 ms
            subprocess.run(["osascript", "-e",
                            'tell application "System Events" to set frontmost of '
                            f'(first process whose unix id is {s.handle.pid}) to true'],
                           capture_output=True)
            _t.sleep(0.3)
            for down in (True, False):
                Quartz.CGEventPost(Quartz.kCGHIDEventTap,
                                   Quartz.CGEventCreateKeyboardEvent(None, 36, down))  # kVK_Return
                _t.sleep(0.05)
            return True
        _t.sleep(0.02)
    return False


def perf_media_dir() -> Path:
    d = Path("/tmp/sneakpeak-perf-media")
    d.mkdir(parents=True, exist_ok=True)
    return d


def burst_fixture(name: str, *, seconds: float, channels: int, sr: int = 44100,
                  dc: float = 0.0) -> Path:
    """Fresh working copy of a quiet 220 Hz tone with one loud burst at 0.5-1.5 s
    (plus a constant offset `dc`), 24-bit PCM. Destructive specs rewrite the file
    they edit, so the pristine original lives under pristine/ and every call
    hands out a new copy. The cache is keyed by `name` alone: a different
    parameter set needs a different name."""
    pristine = perf_media_dir() / "pristine" / name
    if not pristine.exists():
        pristine.parent.mkdir(exist_ok=True)
        with sf.SoundFile(str(pristine), "w", samplerate=sr, channels=channels, subtype="PCM_24") as f:
            for start in range(0, int(seconds * sr), sr * 10):
                t = (np.arange(sr * 10) + start) / sr
                y = 0.03 * np.sin(2 * np.pi * 220 * t)
                burst = (t >= 0.5) & (t < 1.5)
                y[burst] = 0.9 * np.sin(2 * np.pi * 220 * t[burst])
                y += dc
                f.write(np.repeat(y[:, None], channels, axis=1).astype(np.float32))
    if sys.platform == "win32":
        # Windows: REAPER's decoder pool keeps the previous test's copy open and
        # copyfile over it dies with a sharing violation (PermissionError), so
        # every call hands out a uniquely named copy instead. The stem still
        # CONTAINS the base name, so title oracles and _edit globs keep working.
        import itertools
        for i in itertools.count():
            work = perf_media_dir() / (f"{Path(name).stem}_w{i}{Path(name).suffix}" if i else name)
            try:
                shutil.copyfile(pristine, work)
            except PermissionError:
                continue
            # specs derive the pristine path as pristine/<media.name> - mirror
            # the cached original under the unique name too (never held open)
            mirror = pristine.parent / work.name
            if not mirror.exists():
                shutil.copyfile(pristine, mirror)
            return work
    work = perf_media_dir() / name
    shutil.copyfile(pristine, work)
    return work


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
    import shutil
    import subprocess
    if shutil.which("ffmpeg") is None:
        pytest.skip("ffmpeg not installed - the AAC fixture cannot be encoded here")
    tmp = path.with_suffix(".src.wav")
    write_long_wav(tmp, minutes=minutes, sr=sr, channels=2)
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", str(tmp),
                    "-c:a", "aac", "-b:a", "128k", str(path)], check=True)
    tmp.unlink(missing_ok=True)
    return path


def hide_window(s):
    if window_visible(s):
        toggle_window(s)
        s.wait_until(lambda: not window_visible(s), timeout=10)


def insert_item_unselected(s, media: Path, *, position: float = 0.0):
    """Insert without SneakPeak ever seeing the (transiently selected) item:
    with the window open its selection poll would load it in the gap between
    InsertMedia and the deselect - and the retained buffer (phase 2c) would
    then make the next select instant, hiding the load under test."""
    was_visible = window_visible(s)
    if was_visible:
        hide_window(s)
    info = insert_item(s, media, position=position)
    s.eval("reaper.SelectAllMediaItems(0, false) reaper.UpdateArrange() return true")
    if was_visible:
        ensure_window(s)
    return info


SELECT_ITEM0 = ("local it = reaper.GetTrackMediaItem(reaper.GetTrack(0, 0), 0) "
                "reaper.SetMediaItemSelected(it, true) reaper.UpdateArrange() return true")
DESELECT_ALL = "reaper.SelectAllMediaItems(0, false) reaper.UpdateArrange() return true"


# --------------------------------------------------------------------------
# View-mode observable (mode-bar accent pixels, colours from theme.cpp) + input
# --------------------------------------------------------------------------
MODE_ACCENTS = {
    "ITEM": (80, 160, 230), "SET": (80, 200, 100), "TIMELINE": (180, 140, 255),
    "MULTI": (225, 105, 200), "STANDALONE": (230, 160, 50), "MASTER": (200, 80, 80),
}


def mode_from_capture(s, out: Path) -> str:
    """Classify the current view mode from the mode-bar label colour in a
    window capture (the label is drawn in the mode's accent; hue survives
    the capture's colour profile, brightness does not -> cosine match)."""
    cap = capture(s, out)
    cw, ch = client_size(s)
    titlebar = cap.height - ch
    # The mode chip only (x < 60 at scale 1.0): the file tabs to its right carry
    # their own accents (the active "N ITEMS" tab is blue whatever the mode).
    band = cap.image[titlebar:titlebar + 26, 0:60].astype(float)
    px = band.reshape(-1, 3)
    sat = px.max(axis=1) - px.min(axis=1)
    px = px[(sat > 60) & (px.max(axis=1) > 90)]                      # coloured pixels only
    if len(px) == 0:
        return "NONE"
    votes: dict[str, int] = {}
    norms = px / np.linalg.norm(px, axis=1, keepdims=True)
    for name, rgb in MODE_ACCENTS.items():
        ref = np.array(rgb, float); ref /= np.linalg.norm(ref)
        votes[name] = int(((norms @ ref) > 0.995).sum())
    best = max(votes, key=votes.get)
    return best if votes[best] > 0 else "NONE"


def drag_client(s, x0: int, y0: int, x1: int, y1: int, steps: int = 30):
    """In-process drag (Sends) on OUR window. Not bridge_drag: it resolves the
    window with JS_Window_Find("SneakPeak"), the FIRST match, and once a
    "SneakPeak"-titled MessageBox (the Standalone overwrite prompt) has been
    up its hidden leftover window lists first - every later drag then went
    nowhere (found 2026-08-29; the F9 lore, one function further)."""
    moves = "\n".join(f'reaper.JS_WindowMessage_Send(h, "WM_MOUSEMOVE", 1, 0, '
                      f'{int(x0 + (x1 - x0) * i / steps)}, {int(y0 + (y1 - y0) * i / steps)})'
                      for i in range(1, steps + 1))
    ok = s.eval(f"""
      local h = {window_handle_lua()}
      if not h then return false end
      reaper.JS_WindowMessage_Send(h, "WM_LBUTTONDOWN", 1, 0, {int(x0)}, {int(y0)})
      {moves}
      reaper.JS_WindowMessage_Send(h, "WM_LBUTTONUP", 0, 0, {int(x1)}, {int(y1)})
      return true""")
    if not ok:
        raise RuntimeError("drag_client: SneakPeak window not found")


def send_sync(s, msg: str, wparam: int, lo: int = 0, hi: int = 0, settle: float = 0.5):
    """JS_WindowMessage_Send from inside the defer loop. Once a Send has been
    used on our window (bridge_drag / bridge_click do), POSTED messages
    (send_command / press_key) are no longer delivered to it on macOS
    (measured 2026-08-29), so every later command, key and click goes this
    way; a modal it raises blocks the defer loop, which is exactly the stall
    dismiss_native_modal keys on."""
    s.eval(f'reaper.defer(function() reaper.JS_WindowMessage_Send({window_handle_lua()}, "{msg}", '
           f'{wparam}, 0, {lo}, {hi}) end) return true')
    time.sleep(settle)


def command_sync(s, cmd: int, settle: float = 0.5):
    send_sync(s, "WM_COMMAND", cmd, settle=settle)


def key_sync(s, vk: int, settle: float = 0.5):
    send_sync(s, "WM_KEYDOWN", vk, settle=settle)


def click_sync(s, x: int, y: int, settle: float = 0.5):
    send_sync(s, "WM_LBUTTONDOWN", 1, x, y, settle=0.05)
    send_sync(s, "WM_LBUTTONUP", 0, x, y, settle=settle)


def press_key(s, vk: int):
    ok = s.eval(f"local h = {window_handle_lua()} if not h then return false end "
                f'reaper.JS_WindowMessage_Post(h, "WM_KEYDOWN", {int(vk)}, 0, 0, 0) '
                f'reaper.JS_WindowMessage_Post(h, "WM_KEYUP", {int(vk)}, 0, 0, 0) return true')
    if not ok:
        raise RuntimeError("SneakPeak window not found")


VK_DELETE = 0x2E
WAVE_Y = 200          # a client row inside the waveform lane (800x400 window)


def track_item_count(s, track_idx: int = 0) -> int:
    return int(s.eval(f"local tr = reaper.GetTrack(0, {track_idx}) "
                      "return tr and reaper.CountTrackMediaItems(tr) or -1", hang_timeout=120))
