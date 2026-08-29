#!/usr/bin/env python3
"""The guide's Dynamics knob table == the code's knob ranges (audit A7.6).

Parses src/dynamics_ranges.h (lo/hi per index), the knob labels/units from
DynamicsPanel::SLIDER_DEFS (src/dynamics_panel.cpp) and the <table> rows of
docs/guide.html, and asserts every knob has a row whose Range cell states its
minimum and maximum. Ratio is prose (extended encoding) and exempt; kHz cells
are normalised. Run: python3 tests/docs_knob_ranges_test.py (exit code).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXEMPT = {"Ratio"}   # "1:1 to Inf:1 and beyond" - the encoding is explained in prose


def ranges():
    src = (ROOT / "src/dynamics_ranges.h").read_text()
    body = src[src.index("kDynParamRanges[") :]
    out = []
    for m in re.finditer(r"\{\s*(-?[\d.]+),\s*(-?[\d.]+)\s*\},\s*//\s*(\d+)", body):
        out.append((int(m.group(3)), float(m.group(1)), float(m.group(2))))
    return out


def labels():
    src = (ROOT / "src/dynamics_panel.cpp").read_text()
    body = src[src.index("SLIDER_DEFS[NUM_SLIDERS] = {") :]
    body = body[: body.index("};")]
    out = {}
    for m in re.finditer(r'\{\s*"([^"]+)",\s*R\((\d+)\)\.lo,\s*R\(\d+\)\.hi,\s*"([^"]*)"', body):
        out[int(m.group(2))] = (m.group(1), m.group(3))
    return out


def guide_rows():
    html = (ROOT / "docs/guide.html").read_text()
    rows = {}
    for m in re.finditer(r"<tr><td>([^<]+)</td><td>([^<]+)</td>", html):
        rows.setdefault(m.group(1).strip(), m.group(2).strip())
    return rows


def numbers(cell: str):
    scale = 1000.0 if "kHz" in cell else 1.0
    return [float(x) * scale for x in re.findall(r"-?\d+(?:\.\d+)?", cell)]


def main() -> int:
    failures = 0
    rows = guide_rows()
    labs = labels()
    for idx, lo, hi in ranges():
        label, unit = labs[idx]
        if label in EXEMPT:
            continue
        cell = rows.get(label)
        if cell is None:
            print(f"FAIL: no guide row for {label} (index {idx}: {lo:g} to {hi:g} {unit})")
            failures += 1
            continue
        nums = numbers(cell)
        ok = lo in nums and hi in nums
        print(f"{'PASS' if ok else 'FAIL'}: {label}: guide '{cell}' vs code {lo:g} to {hi:g} {unit}")
        failures += 0 if ok else 1
    print(f"\n{'DOCS KNOB RANGES: RED' if failures else 'DOCS KNOB RANGES: GREEN'} ({failures} failure{'' if failures == 1 else 's'})")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
