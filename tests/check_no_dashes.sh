#!/bin/bash
# check_no_dashes.sh - no em/en dashes (U+2014 / U+2013) in any user-facing text.
#
# Scans: string literals in src/*.cpp and src/*.h (code comments may use them),
# docs/, README.md and CHANGELOG.md. Prints every offending line; exit 1 if any.
# Run from anywhere; part of the pre-commit routine alongside tests/run_*.sh.

set -u
cd "$(dirname "$0")/.." || exit 2

python3 - <<'EOF'
import glob, re, sys

bad = 0
DASHES = ("—", "–")

def report(path, lineno, line):
    global bad
    bad += 1
    print(f"{path}:{lineno}: {line.strip()[:120]}")

# Source: only what can reach the user - text inside "..." string literals.
STRING = re.compile(r'"(?:[^"\\]|\\.)*"')
for path in sorted(glob.glob("src/*.cpp") + glob.glob("src/*.h")):
    with open(path, encoding="utf-8", errors="replace") as f:
        for n, line in enumerate(f, 1):
            code = line.split("//", 1)[0]          # drop a trailing line comment
            for lit in STRING.findall(code):
                if any(d in lit for d in DASHES):
                    report(path, n, line)
                    break

# Docs and public text: any occurrence at all.
for path in sorted(glob.glob("docs/**/*", recursive=True) + ["README.md", "CHANGELOG.md"]):
    if not path.endswith((".html", ".md", ".txt", ".xml")):
        continue
    with open(path, encoding="utf-8", errors="replace") as f:
        for n, line in enumerate(f, 1):
            if any(d in line for d in DASHES):
                report(path, n, line)

if bad:
    print(f"check_no_dashes: {bad} line(s) with em/en dashes - use ASCII hyphens in user-facing text")
    sys.exit(1)
print("check_no_dashes: OK (no em/en dashes in string literals, docs, README, CHANGELOG)")
EOF
