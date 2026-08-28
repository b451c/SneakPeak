#!/usr/bin/env python3
"""SneakPeak Linux VM smoke (v2.5 increment 9). Lua-only: drives the user's REAPER through
the ReaProof bridge (Scripts/__startup.lua); no js_ReaScriptAPI on aarch64 - window titles
come from SWS BR_Win32, native modals are dismissed with Xlib focus + XTEST (xfocus.py).
Flow: launch -> ext loaded -> new project + 20-min item -> toggle window -> select item (poll)
-> Reverse via _SneakPeak_Reverse -> file + track-accessor ground truth -> save + quit.
Heartbeat gaps are measured on REAPER's clock. Usage: sp_smoke.py [reaper args...]"""
import json, os, signal, subprocess, sys, threading, time
from pathlib import Path
sys.path.insert(0, str(Path.home() / "reaproof" / "src"))
from reaproof.control.bridge_client import BridgeClient, BridgeHang, BridgeTimeout

RES = Path.home() / ".config" / "REAPER"
RUN = RES / "_reaproof"
WAV = Path.home() / "sp_smoke" / "long20.wav"
RPP = Path.home() / "sp_smoke" / "smoke.RPP"
OUT = Path.home() / "sp_smoke" / "smoke_result.json"
DBGLOG = Path("/tmp/sneakpeak_debug.log")
LAUNCH_ARGS = sys.argv[1:]
R = {"steps": [], "stalls": {}, "ok": True}
TITLES = Path.home().joinpath("probe_titles.lua").read_text()
ACC = Path.home().joinpath("acc.lua").read_text()

def alive():
    return subprocess.run(["pgrep", "-x", "reaper"], capture_output=True).returncode == 0
def pid():
    r = subprocess.run(["pgrep", "-x", "reaper"], capture_output=True, text=True).stdout.split()
    return int(r[0]) if r else None
def step(name, ok, **kw):
    R["steps"].append({"name": name, "ok": bool(ok), **kw})
    if not ok: R["ok"] = False
    print(("PASS " if ok else "FAIL ") + name + (" " + json.dumps(kw) if kw else ""), flush=True)
def xwin(sub):
    out = subprocess.run(["python3", str(Path.home() / "xwin.py")], capture_output=True, text=True).stdout
    return [l.strip() for l in out.splitlines() if sub in l and "cls=('REAPER'" in l]
def xfocus(sub, key):
    r = subprocess.run(["python3", str(Path.home() / "xfocus.py"), sub, key], capture_output=True, text=True)
    return (r.stdout + r.stderr).strip().replace("\n", " | ")
def dbg_tail(pos):
    try: s = DBGLOG.read_text()
    except OSError: return "", pos
    return "".join(l + "\n" for l in s[pos:].splitlines() if "poll tick" not in l), len(s)

class HB(threading.Thread):
    """Samples heartbeat.json; max gap between consecutive ticks on REAPER's clock."""
    def __init__(self):
        super().__init__(daemon=True); self.stop = False; self.last = None
        self.max_gap = 0.0; self.last_wall = time.monotonic(); self.lock = threading.Lock()
    def run(self):
        while not self.stop:
            try: t = float(json.loads((RUN / "heartbeat.json").read_text())["t"])
            except Exception: t = None
            with self.lock:
                if t is not None:
                    if self.last is not None and t > self.last:
                        self.max_gap = max(self.max_gap, t - self.last); self.last_wall = time.monotonic()
                    self.last = t
            time.sleep(0.005)
    def phase(self, name):
        with self.lock: g = self.max_gap; self.max_gap = 0.0
        R["stalls"][name] = round(g, 3); print(f"  stall[{name}] = {g:.3f} s", flush=True); return g
    def silent_for(self):
        with self.lock: return time.monotonic() - self.last_wall

def file_profile():
    import soundfile as sf, numpy as np
    with sf.SoundFile(str(WAV)) as f:
        sr = f.samplerate; n = f.frames
        f.seek(int(0.2 * sr)); head = float(np.abs(f.read(sr)).mean())
        f.seek(n - int(1.5 * sr)); tail = float(np.abs(f.read(sr)).mean())
        return {"sr": sr, "subtype": f.subtype, "frames": n, "head": round(head, 4), "tail": round(tail, 4)}

def sp_title(b):
    return next((t for t in b.eval(TITLES) if t.startswith("SneakPeak") or t.startswith("* SneakPeak")), None)

def quit_reaper(b):
    """Save (silent when the project has a path) + deferred quit; prompt -> No; last resort SIGTERM."""
    try:
        b.eval(f'reaper.Main_SaveProjectEx(0, "{RPP}", 0) return true', timeout=20, hang_timeout=15)
        b.eval('local n=0 local function q() n=n+1 if n>=15 then reaper.Main_OnCommand(40004,0) else reaper.defer(q) end end reaper.defer(q) return true')
    except Exception as e: print("  quit eval:", repr(e))
    t0 = time.monotonic(); how = "graceful"
    while alive() and time.monotonic() - t0 < 20:
        time.sleep(1)
        if xwin("REAPER Query"):
            print("  quit prompt:", xfocus("REAPER Query", "n")); how = "prompt-n"; time.sleep(2)
            if alive() and xwin("REAPER Query"): xfocus("REAPER Query", "Escape"); break
    if alive():
        p = pid(); os.kill(p, signal.SIGTERM); how = "sigterm"
        t1 = time.monotonic()
        while alive() and time.monotonic() - t1 < 10: time.sleep(0.5)
    return how, round(time.monotonic() - t0, 1)

def main():
    assert WAV.exists(), "WAV missing"
    if alive(): print("REAPER already running - abort"); sys.exit(2)
    subprocess.run(["rm", "-rf", str(RUN)])
    R["file_before"] = file_profile(); print("file before", R["file_before"])
    assert R["file_before"]["head"] > 0.2, "regenerate long20.wav (tone must sit at the head)"
    out = subprocess.run(["bash", str(Path.home() / "mp_launch_linux.sh")] + LAUNCH_ARGS, capture_output=True, text=True).stdout
    step("launch", "REAPER-STARTED" in out, out=out.strip().splitlines()[-1] if out else "")
    b = BridgeClient(RUN, is_alive=alive)
    t0 = time.monotonic(); env = b.wait_ready(120); R["bridge_env"] = env
    step("bridge_ready", True, secs=round(time.monotonic() - t0, 1), has_js=env.get("has_js_api"), has_sws=env.get("has_sws"))
    hb = HB(); hb.start(); time.sleep(1.0); hb.phase("idle")
    ids = b.eval('return {toggle=reaper.NamedCommandLookup("_SneakPeak_Toggle"), rev=reaper.NamedCommandLookup("_SneakPeak_Reverse"), load=reaper.NamedCommandLookup("_SneakPeak_LoadSelectedItem"), br=reaper.APIExists("BR_Win32_GetForegroundWindow"), ver=reaper.GetAppVersion()}')
    R["ids"] = ids
    step("extension_loaded", ids["toggle"] and ids["rev"] and ids["br"], **ids)
    R["windows_at_start"] = xwin("")
    length = b.eval(f'''
reaper.Main_OnCommand(40023, 0)
reaper.InsertTrackAtIndex(0, false)
local tr = reaper.GetTrack(0, 0)
reaper.SetOnlyTrackSelected(tr)
reaper.SetEditCurPos(0, false, false)
reaper.InsertMedia("{WAV}", 0)
local it = reaper.GetTrackMediaItem(tr, 0)
reaper.GetSetMediaItemTakeInfo_String(reaper.GetActiveTake(it), "P_NAME", "long20", true)
reaper.SetMediaItemSelected(it, false)
reaper.UpdateArrange()
return reaper.GetMediaItemInfo_Value(it, "D_LENGTH")
''', timeout=120, hang_timeout=60)
    step("item_inserted", length and length > 1190, length=length)
    time.sleep(2.0); hb.phase("insert")
    _, dpos = dbg_tail(0)
    st0 = b.eval(f'return reaper.GetToggleCommandState({ids["toggle"]})')
    b.eval(f'reaper.Main_OnCommand({ids["toggle"]}, 0) return true', hang_timeout=30); time.sleep(1.5)
    st1 = b.eval(f'return reaper.GetToggleCommandState({ids["toggle"]})')
    if st1 == 0 and st0 == 1:
        b.eval(f'reaper.Main_OnCommand({ids["toggle"]}, 0) return true', hang_timeout=30); time.sleep(1.5)
        st1 = b.eval(f'return reaper.GetToggleCommandState({ids["toggle"]})')
    g = hb.phase("toggle"); t = sp_title(b)
    step("window_open", st1 == 1 and t is not None and alive(), state_before=st0, state_after=st1, title=t, stall=round(g, 3))
    d, dpos = dbg_tail(dpos); print("  DBG toggle:\n" + d, end="")
    b.eval('local it = reaper.GetTrackMediaItem(reaper.GetTrack(0,0), 0) reaper.SetMediaItemSelected(it, true) reaper.UpdateArrange() return true')
    time.sleep(6.0)
    g = hb.phase("select_20min"); t = sp_title(b)
    d, dpos = dbg_tail(dpos); print("  DBG select:\n" + d, end="")
    step("select_long_item_poll", alive() and g <= 0.25 and t == "SneakPeak: long20", stall=round(g, 3), title=t)
    if t != "SneakPeak: long20":
        b.eval(f'reaper.Main_OnCommand({ids["load"]}, 0) return true', hang_timeout=30); time.sleep(2)
        t = sp_title(b); step("select_long_item_action", t == "SneakPeak: long20", title=t)
        d, dpos = dbg_tail(dpos); print("  DBG action load:\n" + d, end="")
    # Reverse: the native confirm blocks the defer loop on Linux (nested SWELL DialogBoxParam)
    b.eval(f'reaper.defer(function() reaper.Main_OnCommand({ids["rev"]}, 0) end) return true')
    time.sleep(1.5)
    modal = xwin("Destructive"); blocked = hb.silent_for() > 1.0
    print("  modal:", modal, "| defer blocked:", blocked, flush=True)
    step("reverse_confirm_shown", bool(modal) and blocked)
    print("  dismiss:", xfocus("Destructive", "Return"), flush=True)
    t_yes = time.monotonic(); resumed = None
    while time.monotonic() - t_yes < 240 and alive():
        s = hb.silent_for(); time.sleep(0.25)
        if hb.silent_for() < s: resumed = round(time.monotonic() - t_yes, 2); break
    g = hb.phase("reverse_write"); t = sp_title(b) if alive() else None
    step("reverse_returned", resumed is not None and alive(), resumed_after_s=resumed, title=t, stall=round(g, 3))
    d, dpos = dbg_tail(dpos); print("  DBG reverse:\n" + d, end="")
    time.sleep(1.0)
    R["file_after"] = file_profile(); print("file after", R["file_after"])
    fb, fa = R["file_before"], R["file_after"]
    step("file_reversed_in_place", fa["tail"] > 0.2 and fa["head"] < 0.08 and fa["sr"] == fb["sr"] and fa["frames"] == fb["frames"] and fa["subtype"] == fb["subtype"], **fa)
    acc = b.eval(ACC, timeout=60, hang_timeout=30); R["accessor_after"] = acc
    step("track_accessor_hears_reverse", acc["tail"] > 0.2 and acc["head"] < 0.08, **acc)
    hb.phase("verify")
    how, secs = quit_reaper(b); hb.stop = True
    step("quit", not alive(), how=how, secs=secs)

try:
    main()
except Exception as e:
    R["ok"] = False; R["exception"] = repr(e); print("EXCEPTION", repr(e), flush=True)
finally:
    R["reaper_alive_at_end"] = alive()
    OUT.parent.mkdir(exist_ok=True); OUT.write_text(json.dumps(R, indent=1))
    print("RESULT", "OK" if R["ok"] else "FAIL", "->", OUT, flush=True)
