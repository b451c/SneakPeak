#!/usr/bin/env python3
"""Linux xdg-open probe (v2.5.0 audit A5.6 / A12.4).
Question: do the support links in SneakPeak's right-click menu reach xdg-open
on Linux (they used to call the macOS opener)? A shim `xdg-open` is placed
first in PATH for this REAPER launch and logs its arguments; the menu
commands are fired through WM_COMMAND (SWS BR_Win32_SendMessage) exactly as
TrackPopupMenu would deliver them. Usage: python3 -u sp_xdg_probe.py -new -nosplash
Result JSON: ~/sp_smoke/xdg_probe.json (ok = every link logged its own URL).
"""
import json, os, signal, subprocess, sys, time
from pathlib import Path
sys.path.insert(0, str(Path.home() / "reaproof" / "src"))
from reaproof.control.bridge_client import BridgeClient

HERE = Path(__file__).resolve().parent
RES = Path.home() / ".config" / "REAPER"
RUN = RES / "_reaproof"
SHIM_DIR = Path.home() / "sp_smoke" / "xdg_shim"
SHIM_LOG = Path.home() / "sp_smoke" / "xdg_open.log"
OUT = Path.home() / "sp_smoke" / "xdg_probe.json"
LAUNCH_ARGS = sys.argv[1:]
# context-menu ids (edit_view.h enum, CM_UNDO = 2000) -> the URL each opens
LINKS = {2031: "https://ko-fi.com/quickmd", 2032: "https://buymeacoffee.com/bsroczynskh",
         2033: "https://www.paypal.com/paypalme/b451c", 2034: "https://github.com/b451c/SneakPeak"}
R = {"steps": [], "ok": True}


def alive():
    return subprocess.run(["pgrep", "-x", "reaper"], capture_output=True).returncode == 0


def step(name, ok, **kw):
    R["steps"].append({"name": name, "ok": bool(ok), **kw}); R["ok"] &= bool(ok)
    print(("PASS " if ok else "FAIL ") + name, json.dumps(kw), flush=True)


SEND = '''
local main = reaper.BR_Win32_GetMainHwnd()
local w = reaper.BR_Win32_GetWindow(main, 0)
local n, found = 0, nil
while w and n < 80 do
  local _, t = reaper.BR_Win32_GetWindowText(w)
  if t and t:sub(1, 9) == "SneakPeak" and reaper.BR_Win32_IsWindowVisible(w) then found = w break end
  w = reaper.BR_Win32_GetWindow(w, 2) n = n + 1
end
if not found then return "no-window" end
local r = reaper.BR_Win32_SendMessage(found, 0x0111, %d, 0)
return "sent:" .. tostring(r)
'''


def main():
    if alive(): print("REAPER already running - abort"); sys.exit(2)
    subprocess.run(["rm", "-rf", str(RUN)])
    SHIM_DIR.mkdir(parents=True, exist_ok=True)
    shim = SHIM_DIR / "xdg-open"
    shim.write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "' + str(SHIM_LOG) + '"\nexit 0\n')
    shim.chmod(0o755)
    SHIM_LOG.write_text("")
    env = dict(os.environ, PATH=f"{SHIM_DIR}:{os.environ.get('PATH', '')}")
    out = subprocess.run(["bash", str(Path.home() / "mp_launch_linux.sh")] + LAUNCH_ARGS,
                         capture_output=True, text=True, env=env).stdout
    step("launch", "REAPER-STARTED" in out, out=out.strip().splitlines()[-1] if out else "")
    b = BridgeClient(RUN, is_alive=alive)
    benv = b.wait_ready(120)
    step("bridge_ready", True, has_sws=benv.get("has_sws"))
    toggle = b.eval('return reaper.NamedCommandLookup("_SneakPeak_Toggle")')
    step("extension_loaded", bool(toggle), toggle=toggle)
    if b.eval(f'return reaper.GetToggleCommandState({toggle})') != 1:
        b.eval(f'reaper.Main_OnCommand({toggle}, 0) return true', hang_timeout=30); time.sleep(1.5)
    step("window_open", b.eval(f'return reaper.GetToggleCommandState({toggle})') == 1)
    for cmd, url in LINKS.items():
        before = SHIM_LOG.read_text()
        r = b.eval(SEND % cmd, timeout=20, hang_timeout=10); time.sleep(1.0)
        new = SHIM_LOG.read_text()[len(before):].strip().splitlines()
        step(f"link_{cmd}", r != "no-window" and new == [url], send=r, logged=new, want=url)
    R["shim_log"] = SHIM_LOG.read_text().splitlines()
    # quit: deferred 40004; the unsaved-project prompt does not take 'n' - SIGTERM the instance we launched
    try:
        b.eval('reaper.defer(function() reaper.Main_OnCommand(40004, 0) end) return true', timeout=10, hang_timeout=5)
    except Exception:
        pass
    time.sleep(4)
    if alive():
        p = int(subprocess.run(["pgrep", "-x", "reaper"], capture_output=True, text=True).stdout.split()[0])
        os.kill(p, signal.SIGTERM); time.sleep(3)
    step("quit", not alive())
    OUT.write_text(json.dumps(R, indent=1))
    print(("RESULT OK" if R["ok"] else "RESULT FAIL") + f" -> {OUT}", flush=True)
    sys.exit(0 if R["ok"] else 1)


if __name__ == "__main__":
    main()
