#!/usr/bin/env python3
import json, subprocess, sys
from pathlib import Path
sys.path.insert(0, str(Path.home() / "reaproof" / "src"))
from reaproof.control.bridge_client import BridgeClient
RUN = Path.home() / ".config" / "REAPER" / "_reaproof"
lua = Path(sys.argv[1]).read_text() if len(sys.argv) > 1 else sys.stdin.read()
b = BridgeClient(RUN, is_alive=lambda: subprocess.run(["pgrep","-x","reaper"],capture_output=True).returncode == 0)
b._seq = int(sys.argv[2]) if len(sys.argv) > 2 else 500
print(json.dumps(b.eval(lua, timeout=20, hang_timeout=10), indent=1))
