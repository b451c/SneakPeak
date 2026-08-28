#!/usr/bin/env python3
"""Linux/Wayland UI-scale probe (v2.5.0 audit A3.4, F24 suspicion).

Question: with GDK_SCALE=2 does SneakPeak's ui_scale re-seed (edit_view.cpp
QuerySystemDefaultUiScale -> SWELL_GetScaling256) stack on top of GDK's own
window scaling (= 4x)? SWELL's swell_scaling_init takes the GDK monitor scale
factor into g_swell_ui_scale and then DISABLES GDK window scaling
(gdk_x11_display_set_window_scale(disp, 1)), scaling dialog templates itself,
so one 2x is expected. Evidence: the X window geometry (xwin.py, device pixels)
vs the logical window rect from SWS BR_Win32_GetWindowRect - equal means the
client area is 1:1 pixels (SWELL scaling, GDK scaling off) and SneakPeak's own
g_uiScale is the only magnifier. Also reads the persisted ui_scale and what
SWELL reports. Usage: GDK_SCALE=2 python3 -u sp_scale_probe.py -new -nosplash
"""
import json, os, signal, subprocess, sys, time
from pathlib import Path
sys.path.insert(0, str(Path.home() / "reaproof" / "src"))
from reaproof.control.bridge_client import BridgeClient

HERE = Path(__file__).resolve().parent
RES = Path.home() / ".config" / "REAPER"
RUN = RES / "_reaproof"
OUT = Path.home() / "sp_smoke" / f"scale_probe_gdk{os.environ.get('GDK_SCALE', '1')}.json"
TITLES = (HERE / "probe_titles.lua").read_text()
R = {"gdk_scale": os.environ.get("GDK_SCALE"), "ok": True}

def alive():
    return subprocess.run(["pgrep", "-x", "reaper"], capture_output=True).returncode == 0
def pid():
    r = subprocess.run(["pgrep", "-x", "reaper"], capture_output=True, text=True).stdout.split()
    return int(r[0]) if r else None
def xwin(sub):
    out = subprocess.run(["python3", str(HERE / "xwin.py")], capture_output=True, text=True).stdout
    return [l.strip() for l in out.splitlines() if sub in l and "cls=('REAPER'" in l]

RECT = TITLES.replace("return tops", "") + '''
local w = reaper.BR_Win32_GetWindow(reaper.BR_Win32_GetMainHwnd(), 0)
local n = 0
while w and n < 40 do
  if reaper.BR_Win32_IsWindowVisible(w) and title(w):find("SneakPeak") then
    local _, l, t, r, b = reaper.BR_Win32_GetWindowRect(w)
    local client = nil
    if reaper.APIExists("BR_Win32_GetClientRect") then
      local _, cl, ct, cr, cb = reaper.BR_Win32_GetClientRect(w) client = {cl, ct, cr, cb}
    end
    return { title = title(w), win = {l, t, r, b}, client = client,
             ui_scale = reaper.GetExtState("SneakPeak", "ui_scale") }
  end
  w = reaper.BR_Win32_GetWindow(w, 2) n = n + 1
end
return nil'''

try:
    if alive(): print("REAPER already running - abort"); sys.exit(2)
    subprocess.run(["rm", "-rf", str(RUN)])
    out = subprocess.run(["bash", str(Path.home() / "mp_launch_linux.sh")] + sys.argv[1:],
                         capture_output=True, text=True).stdout
    print(out.strip().splitlines()[-1] if out else "(no launcher output)")
    b = BridgeClient(RUN, is_alive=alive)
    R["bridge_env"] = b.wait_ready(120)
    toggle = b.eval('return reaper.NamedCommandLookup("_SneakPeak_Toggle")')
    if b.eval(f'return reaper.GetToggleCommandState({toggle})') != 1:
        b.eval(f'reaper.Main_OnCommand({toggle}, 0) return true', hang_timeout=30)
    time.sleep(2.0)
    R["logical"] = b.eval(RECT, timeout=30, hang_timeout=20)
    R["x_windows"] = xwin("SneakPeak")
    R["reaper_main_x"] = xwin("REAPER")[:3]
    print("logical (SWS):", json.dumps(R["logical"]))
    print("X windows    :", R["x_windows"])
    lg = R["logical"]
    if lg and R["x_windows"]:
        lw, lh = lg["win"][2] - lg["win"][0], lg["win"][3] - lg["win"][1]
        geo = R["x_windows"][0].split()[2].split("+")[0]
        xw, xh = (int(v) for v in geo.split("x"))
        R["verdict"] = {"logical_w": lw, "logical_h": lh, "x_w": xw, "x_h": xh,
                        "pixels_1_to_1": abs(lw - xw) <= 4 and abs(lh - xh) <= 4}
        print("verdict:", json.dumps(R["verdict"]))
    # quit: deferred 40004, then SIGTERM if the save prompt holds it
    b.eval('local n=0 local function q() n=n+1 if n>=15 then reaper.Main_OnCommand(40004,0) else reaper.defer(q) end end reaper.defer(q) return true')
    t0 = time.monotonic()
    while alive() and time.monotonic() - t0 < 8: time.sleep(0.5)
    if alive(): os.kill(pid(), signal.SIGTERM); time.sleep(3)
except Exception as e:
    R["ok"] = False; R["exception"] = repr(e); print("EXCEPTION", repr(e), flush=True)
finally:
    OUT.parent.mkdir(exist_ok=True); OUT.write_text(json.dumps(R, indent=1))
    print("RESULT ->", OUT, flush=True)
