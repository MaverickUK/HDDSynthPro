"""Compares RAM objects' banks between two firmware builds.

The RP2350 spreads RAM across 8 banks a word at a time, so an object's bank is
its address mod 32. Data that core 1, core 0 and the DMA share for every sector
(the device's sector buffers, the fetch mailbox, the card driver) runs slower on
another bank: 40 bytes of new variables ahead of the device made the data lane's
reads 8% slower (0.36.0-dev.2, docs/speed-log.md, 19 Nov 2026). The linker sorts
.bss by alignment and then by name, so a change anywhere can move what follows.
Lists the objects below `--limit` (the USB buffers on) that changed bank, and the
new or resized ones, whose growth should be a whole number of 32 bytes.

    python3 scripts/check_ram_banks.py OLD.elf NEW.elf [--limit 0x20076000]
"""
import subprocess
import sys

import os
NM = os.path.expanduser('~/.platformio/packages/toolchain-rp2040-earlephilhower/bin/arm-none-eabi-nm')


def load(elf):
    out = subprocess.run([NM, '-C', '-n', '-S', elf], capture_output=True, text=True).stdout
    d = {}
    for line in out.splitlines():
        p = line.split(None, 3)
        if len(p) == 4 and p[2] in 'bBdD':
            d[p[3].strip()] = (int(p[0], 16), int(p[1], 16))
    return d


def main():
    a, b = load(sys.argv[1]), load(sys.argv[2])
    limit = int(sys.argv[sys.argv.index('--limit') + 1], 16) if '--limit' in sys.argv else 0x20076000
    moved = [(a[k][0], k) for k in a if k in b and 0x20000000 <= a[k][0] < limit and (b[k][0] - a[k][0]) % 32]
    print(f'objects below {hex(limit)} that changed bank: {len(moved)}')
    for addr, k in sorted(moved)[:40]:
        print(f'  {hex(addr)} -> {hex(b[k][0])} ({b[k][0] - addr:+}) size {a[k][1]:6}  {k[:70]}')
    print('new or resized:')
    for k, (addr, size) in sorted(b.items(), key=lambda x: x[1][0]):
        if addr < 0x20000000 or addr >= limit:
            continue
        if k not in a:
            print(f'  {hex(addr)} new  {size:6} ({size % 32:2} mod 32)  {k[:70]}')
        elif a[k][1] != size:
            print(f'  {hex(addr)} grew {size - a[k][1]:+6} ({(size - a[k][1]) % 32:2} mod 32)  {k[:70]}')


if __name__ == '__main__':
    main()
