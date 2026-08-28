#!/usr/bin/env python3
"""List X windows (names, map state, geometry, pid) under Xwayland - screenshot substitute."""
import os, sys
from pathlib import Path
xa = sorted(Path("/run/user/1000").glob(".mutter-Xwaylandauth.*"))
if xa: os.environ["XAUTHORITY"] = str(xa[0])
os.environ.setdefault("DISPLAY", ":0")
from Xlib import display, X
d = display.Display(); root = d.screen().root
NET_NAME = d.intern_atom("_NET_WM_NAME"); PID = d.intern_atom("_NET_WM_PID")
def name(w):
    try:
        p = w.get_full_property(NET_NAME, 0)
        if p and p.value: return p.value.decode(errors="replace")
        n = w.get_wm_name()
        return n if isinstance(n, str) else (n.decode(errors="replace") if n else "")
    except Exception: return "?"
def walk(w, depth=0):
    try: kids = w.query_tree().children
    except Exception: return
    for k in kids:
        try:
            a = k.get_attributes(); g = k.get_geometry(); n = name(k)
            pid = k.get_full_property(PID, 0)
            if n or a.map_state == X.IsViewable:
                print("  " * depth + repr(n), "map=%d" % a.map_state, "%dx%d+%d+%d" % (g.width, g.height, g.x, g.y),
                      "pid=%s" % (pid.value[0] if pid else "-"), "cls=%s" % (k.get_wm_class(),))
        except Exception: pass
        if depth < 2: walk(k, depth + 1)
walk(root)
