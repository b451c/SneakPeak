# Linux VM smoke (v2.5 increment 9)

Action-level smoke of the built `.so` inside the Ubuntu aarch64 VM's own REAPER
(7.69, `/opt/REAPER`, GNOME/Wayland). The VM has no js_ReaScriptAPI build for
aarch64, so the macOS ReaProof specs cannot run there; this leg drives REAPER
through the ReaProof bridge (file-queue JSON-RPC, deployed as
`Scripts/__startup.lua`) and reads window titles through SWS `BR_Win32_*`.
Native modals are dismissed with Xlib (`_NET_ACTIVE_WINDOW` + `XSetInputFocus`)
followed by an XTEST key (`xfocus.py`).

## Files
- `sp_smoke.py` - the smoke: launch -> extension loaded -> new project + 20-min
  WAV -> toggle window -> select item (selection poll) -> `_SneakPeak_Reverse`
  (native confirm dismissed) -> file + track-accessor ground truth -> quit.
  Heartbeat gaps are measured on REAPER's clock; result JSON + step log.
- `sp_eval.py <file.lua> [seq]` - run one Lua chunk in the live REAPER.
- `probe_titles.lua` - visible top-level titles via `BR_Win32_GetWindow`.
- `acc.lua` - track-accessor mean-abs of the item head/tail.
- `sp_xdg_probe.py` - the support links reach `xdg-open` (A5.6): a shim `xdg-open`
  first in PATH logs its arguments while the four menu commands are fired through
  WM_COMMAND (`BR_Win32_SendMessage`), exactly as the popup menu delivers them.
- `xwin.py` - X window list (name/map/geometry/pid); the screenshot substitute.
- `xfocus.py <substr> [key]` - focus a window by title, optionally send a key.
- `gen_long.py <out.wav> <minutes>` - 44.1k/16-bit stereo, 1 kHz tone in the
  first 2 s, noise after (Reverse moves the tone to the tail).

## VM prerequisites (once)
`~/reaproof` = ReaProof copy (bridge at `bridge/reaproof_bridge.lua`, client at
`src/reaproof/control/bridge_client.py`); `python3-soundfile`, `numpy`;
`python-xlib` (`pip install --user --break-system-packages python-xlib`; no
venv module on the VM); `~/mp_launch_linux.sh` (launches REAPER inside the GNOME
session by borrowing a session client's env - ReDockIT lore).

## Run
```
# host: sync the branch (the VM has feat/v250 checked out -> fetch + ff)
git bundle create /tmp/v250.bundle <vm-head>..feat/v250
scp /tmp/v250.bundle basic@192.168.64.4:/tmp/
ssh basic@192.168.64.4 'cd ~/dev/SneakPeak && git fetch /tmp/v250.bundle feat/v250 && git merge --ff-only FETCH_HEAD && cmake --build build -j8'
# VM: install (fresh inode), deploy the bridge, run, REMOVE the bridge
rm -f ~/.config/REAPER/UserPlugins/reaper_sneakpeak-aarch64.so
cp ~/dev/SneakPeak/build/reaper_sneakpeak.so ~/.config/REAPER/UserPlugins/reaper_sneakpeak-aarch64.so
cp ~/reaproof/bridge/reaproof_bridge.lua ~/.config/REAPER/Scripts/__startup.lua
python3 gen_long.py ~/sp_smoke/long20.wav 20
python3 -u sp_smoke.py -new -nosplash
rm -f ~/.config/REAPER/Scripts/__startup.lua; rm -rf ~/.config/REAPER/_reaproof
```
The smoke expects the SneakPeak window CLOSED at start (`was_visible=0` in the
`[SneakPeak]` section of `reaper-extstate.ini`; edit only while REAPER is closed,
and only that section - other extensions persist the same key name).

## Lessons (2026-08-28)
- A SWELL `MessageBox` (generic `DialogBoxParam`) blocks REAPER's defer loop, so
  the bridge is dead while a confirm is up: fire the action from inside a defer,
  then dismiss from outside (focus + XTEST Return = the default Yes).
- `Main_openProject` kills the startup script's defer loop (bridge gone) - never
  call it from the bridge; quit = `Main_SaveProjectEx` + deferred 40004, and the
  "REAPER Query" save prompt does not take `n`: SIGTERM an instance you launched.
- REAPER's evaluation "About" box and the "New Version Notification" are
  modeless; both were up only in run 1, the one run whose selection poll did not
  load the selected item (4 later runs clean) - see plan finding F18.
- Xwayland: XTEST goes to the focused window; windows launched over SSH have no
  focus until `_NET_ACTIVE_WINDOW` + `XSetInputFocus` (xfocus.py).
