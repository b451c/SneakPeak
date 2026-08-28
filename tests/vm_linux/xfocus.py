#!/usr/bin/env python3
"""xfocus.py <name-substring> [key] : focus the X window whose name contains the substring, optionally send a key via XTEST."""
import os, sys, time
from pathlib import Path
xa = sorted(Path("/run/user/1000").glob(".mutter-Xwaylandauth.*"))
if xa: os.environ["XAUTHORITY"] = str(xa[0])
os.environ.setdefault("DISPLAY", ":0")
from Xlib import display, X, XK
from Xlib.ext import xtest
d = display.Display(); root = d.screen().root
NET_NAME = d.intern_atom("_NET_WM_NAME"); ACTIVE = d.intern_atom("_NET_ACTIVE_WINDOW")
def name(w):
    try:
        p = w.get_full_property(NET_NAME, 0)
        if p and p.value: return p.value.decode(errors="replace")
        n = w.get_wm_name(); return n if isinstance(n, str) else (n.decode(errors="replace") if n else "")
    except Exception: return ""
def find(w, sub, depth=0):
    for k in w.query_tree().children:
        try:
            if sub in name(k) and k.get_wm_class() and k.get_wm_class()[0] != "mutter-x11-frames": return k
        except Exception: pass
        if depth < 2:
            r = find(k, sub, depth + 1)
            if r: return r
    return None
w = find(root, sys.argv[1])
if not w: print("not found"); sys.exit(1)
# ask the WM to activate it (EWMH), then set X focus directly as well
from Xlib import protocol
ev = protocol.event.ClientMessage(window=w, client_type=ACTIVE, data=(32, [2, X.CurrentTime, 0, 0, 0]))
root.send_event(ev, event_mask=X.SubstructureRedirectMask | X.SubstructureNotifyMask); d.sync(); time.sleep(0.3)
w.set_input_focus(X.RevertToParent, X.CurrentTime); d.sync(); time.sleep(0.3)
f = d.get_input_focus().focus
print("focused:", repr(name(w)), "now-focus:", repr(name(f)) if hasattr(f, "id") else f)
if len(sys.argv) > 2:
    kc = d.keysym_to_keycode(XK.string_to_keysym(sys.argv[2]))
    xtest.fake_input(d, X.KeyPress, kc); xtest.fake_input(d, X.KeyRelease, kc); d.sync(); print("sent", sys.argv[2])
