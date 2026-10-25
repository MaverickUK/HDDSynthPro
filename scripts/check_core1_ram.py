#!/usr/bin/env python3
"""Checks that everything core 1 calls from loop1() runs from RAM.

Code in flash runs through a cache that core 0 keeps evicting, so a call
into flash can stall core 1 for tens of microseconds while the host waits on
IORDY. This walks the call graph from loop1 in the built firmware and fails
if it reaches a function in flash, other than the few one-offs below.
Flash calls crept in three times before this check (0.5.1, and twice in
0.8.0: one, an out-of-line std::atomic<bool>::load, cost about 1 us a read).
It can't see calls through function pointers; core 1 makes none.

Usage: scripts/check_core1_ram.py [firmware.elf]
"""
from __future__ import annotations

import glob
import os
import re
import subprocess
import sys

ELF = sys.argv[1] if len(sys.argv) > 1 else ".pio/build/bb48_drive/firmware.elf"
RAM = (0x20000000, 0x20082000)

# Called by core 1 only at start-up or on a bus reset, never per access.
ONE_OFFS = (
    "ata::Device::Device(",       # loop1's static device, constructed once
    "ide_bus::restart(",          # when the host releases RESET#
    "iordy_timer::requestReset(",  # likewise
    "__cxa_guard_acquire",        # guards for function-local statics
    "__cxa_guard_release",
)


def tool(name: str) -> str:
    found = glob.glob(os.path.expanduser(f"~/.platformio/packages/toolchain-rp2040-earlephilhower/bin/*-{name}"))
    if not found:
        sys.exit(f"check_core1_ram: no {name} in the PlatformIO toolchain")
    return found[0]


def main() -> int:
    dump = subprocess.run([tool("objdump"), "-d", "--no-show-raw-insn", ELF],
                          capture_output=True, text=True, check=True).stdout
    funcs: dict[str, tuple[int, list[str]]] = {}  # mangled name -> (address, callees)
    current = None
    for line in dump.splitlines():
        m = re.match(r"^([0-9a-f]{8}) <(.+)>:$", line)
        if m:
            current = m.group(2)
            funcs[current] = (int(m.group(1), 16), [])
            continue
        call = re.search(r"\sbl?x?\s+[0-9a-f]+ <([^>+]+)>", line)
        if current and call and re.search(r"\sblx?\s", line):
            funcs[current][1].append(call.group(1))

    names = list(funcs)
    demangled = dict(zip(names, subprocess.run([tool("c++filt")], input="\n".join(names),
                                               capture_output=True, text=True, check=True).stdout.splitlines()))

    def in_ram(name: str) -> bool:
        return RAM[0] <= funcs[name][0] < RAM[1]

    def one_off(name: str) -> bool:
        return any(demangled.get(name, name).startswith(p) for p in ONE_OFFS)

    problems = []
    seen = set()
    stack = [("loop1", ["loop1"])]
    while stack:
        name, path = stack.pop()
        if name in seen or name not in funcs:
            continue
        seen.add(name)
        if name.endswith("_veneer"):  # a RAM stub that jumps to flash
            target = name[2:-len("_veneer")]
            if target in funcs and not in_ram(target) and not one_off(target):
                problems.append(path[:-1] + [target])
            continue
        if not in_ram(name):
            if not one_off(name):
                problems.append(path)
            continue
        for callee in funcs[name][1]:
            stack.append((callee, path + [callee]))

    if problems:
        print("check_core1_ram: core 1 calls into flash:")
        for path in problems:
            print("  " + " -> ".join(demangled.get(p, p) for p in path))
        return 1
    print(f"check_core1_ram: ok, {len(seen)} functions reachable from loop1, all in RAM or one-offs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
