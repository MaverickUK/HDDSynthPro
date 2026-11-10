#!/usr/bin/env python3
"""Checks that core 1's hard disk code hasn't moved in RAM.

Core 1's speed depends on where its code sits (CLAUDE.md, Core split):
0.32.0-dev.2-4 read 2.5% slower in PIO with byte-identical functions 3.6 KB
further on. scripts/core1_layout.py places the code first in RAM, between
the symbols __core1_hd_start and __core1_hd_end; this compares every
function there (address, size, name) with the layout recorded in
scripts/core1_layout/<env>.txt, and fails on any difference.

A change to core 1's code moves it by design. Then benchmark reads on the rig
(`-m speed` against the last release, or 4 MB sequential), record the result
in docs/speed-log.md, and update the layout with --update.

Usage: scripts/check_core1_layout.py [--update] [env ...]
       (default: bb48_drive pcb_drive; the ELFs in .pio/build/<env>/)
"""
from __future__ import annotations

import difflib
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_core1_ram import tool  # noqa: E402

ENVS = ("bb48_drive", "pcb_drive")
LAYOUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "core1_layout")


def layout(elf: str) -> list[str]:
    """Core 1's hard disk functions, as 'address size name' lines."""
    out = subprocess.run([tool("nm"), "-S", "-n", "-C", elf],
                         capture_output=True, text=True, check=True).stdout
    symbols = {}
    rows = []
    for line in out.splitlines():
        parts = line.split(maxsplit=3)
        if len(parts) == 3 and parts[2].startswith("__core1_hd_"):
            symbols[parts[2]] = int(parts[0], 16)
        elif len(parts) == 4 and parts[2] in ("t", "T", "w", "W"):  # code, local or global or weak
            rows.append((int(parts[0], 16), int(parts[1], 16), parts[3]))
    if "__core1_hd_start" not in symbols or "__core1_hd_end" not in symbols:
        sys.exit(f"check_core1_layout: {elf} has no __core1_hd_start/__core1_hd_end: "
                 "was it built without scripts/core1_layout.py (platformio.ini's extra_scripts)?")
    start, end = symbols["__core1_hd_start"], symbols["__core1_hd_end"]
    lines = [f"{a:08x} {s:5d} {name}" for a, s, name in rows if start <= a < end]
    return [f"block {start:08x}-{end:08x} ({end - start} bytes)"] + lines


def main(argv: list[str]) -> int:
    update = "--update" in argv
    envs = [a for a in argv if not a.startswith("--")] or list(ENVS)
    failed = False
    for env in envs:
        elf = os.path.join(".pio", "build", env, "firmware.elf")
        if not os.path.exists(elf):
            sys.exit(f"check_core1_layout: no {elf}; build it first (pio run -e {env})")
        now = layout(elf)
        path = os.path.join(LAYOUT_DIR, f"{env}.txt")
        if update:
            os.makedirs(LAYOUT_DIR, exist_ok=True)
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write("\n".join(now) + "\n")
            print(f"check_core1_layout: {env}: recorded {len(now) - 1} functions in {path}")
            continue
        if not os.path.exists(path):
            sys.exit(f"check_core1_layout: no recorded layout {path}; record one with --update")
        with open(path, encoding="utf-8") as f:
            was = f.read().splitlines()
        if now == was:
            print(f"check_core1_layout: {env}: {len(now) - 1} functions, none moved")
            continue
        failed = True
        print(f"check_core1_layout: {env}: core 1's hard disk code moved or changed:")
        for line in difflib.unified_diff(was, now, "recorded", "built", lineterm="", n=0):
            print("  " + line)
    if failed:
        print("\nCore 1's speed depends on where its code sits. If the change was meant, benchmark\n"
              "reads on the rig (-m speed, against the last release), note the result in\n"
              "docs/speed-log.md, then record the new layout: scripts/check_core1_layout.py --update")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
