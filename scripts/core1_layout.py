"""Places core 1's hard disk code first in RAM, in a fixed order.

Core 1's speed depends on where its code sits in RAM (CLAUDE.md, Core split):
0.32.0-dev.2-4 read 2.5% slower in PIO than 0.31.0 with byte-identical
functions, because the CD-ROM's code (atapi_device.cpp) linked between
ata_device.cpp and ide_bus.cpp and moved everything after it 3.6 KB on.

Arduino-Pico's linker script takes every `.time_critical*` section in link
order, which is the source files' alphabetical order, so a new file with
CORE1_RAM code, or a renamed one, could land among them again. PlatformIO runs
this after generating the linker script (extra_scripts in platformio.ini), and
it adds, first in `.data` (RAM):

- the files whose code core 1 runs per access, in this order (the order they
  had in 0.31.0 and 0.32.0-dev.5, so their addresses don't change);
- any other file's CORE1_RAM code after them;
- the symbols __core1_hd_start and __core1_hd_end around the block, which
  scripts/check_core1_layout.py reads to check that nothing in it moved.

The CD-ROM's core 1 code (CORE1_RAM_CD) stays in `.after_data.*`, later.
Adding a file here moves the code after it: benchmark reads (`-m speed`) and
update the layout (`scripts/check_core1_layout.py --update`).

After core 1's block comes core 0's per-sector code (CORE0_RAM:
serveFetches and the disks' reads and writes, between __core0_hot_start and
__core0_hot_end). It ran from flash, so any change to flash code moved it in
the flash cache: the data lane, whose reads wait on core 0, read about 10%
slower after 0.33.1-dev.10 added a few hundred bytes of start-up code
(docs/speed-log.md, 16 Nov 2026).

It also rounds four places up to a multiple of BANK_PERIOD (32 bytes): the
end of core 1's block, the end of core 0's, the start of `.data`'s variables
and the start of `.bss`. Every RAM variable (the rings, the device and its buffers, the PIO
FIFOs' DMA buffers) comes after the code, and the RP2350 spreads RAM across 8
banks a word at a time, so a buffer's bank repeats every 32 bytes. Without
this, any change in the size of the code before them moved every buffer to
other banks: 0.33.1-dev.10's SET MULTIPLE MODE check (44 bytes) and then a
28-byte change in loop1 each made the data lane's reads about 10% slower with
the lane's code untouched (docs/speed-log.md, 16 Nov 2026). Now the code can
grow or shrink and the data moves only in whole 32-byte steps, keeping its
banks. A change to the variables themselves can still move them.
"""
from __future__ import annotations

import os

# Core 1's hard disk code, by object file, in its order in RAM.
FILES = (
    "ata_device.cpp.o",
    "ide_bus.cpp.o",
    "ide_dma.cpp.o",
    "ide_listen.cpp.o",
    "identify.cpp.o",
    "main.cpp.o",
)

ANCHOR = "        *(vtable)\n"
MARK = "__core1_hd_start"
BANK_PERIOD = 32  # bytes: 8 banks, word-interleaved (RP2350 datasheet, SRAM)
DATA_ANCHOR = "        *(.data*)\n"
BSS_ANCHOR = "        . = ALIGN(4);\n        __bss_start__ = .;\n"


def patch_text(text: str) -> str:
    """The linker script with core 1's block after `.data`'s vtable."""
    if MARK in text:
        return text  # already patched
    nl = "\r\n" if "\r\n" in text else "\n"
    anchor = ANCHOR.replace("\n", nl)
    if text.count(anchor) != 1:
        raise RuntimeError("core1_layout: the linker script has no single `*(vtable)` line in .data "
                           "to place core 1's code after (did Arduino-Pico's memmap_default.ld change?)")
    data_anchor = DATA_ANCHOR.replace("\n", nl)
    bss_anchor = BSS_ANCHOR.replace("\n", nl)
    for found, what in ((data_anchor, "`*(.data*)`"), (bss_anchor, "`. = ALIGN(4); __bss_start__ = .;`")):
        if text.count(found) != 1:
            raise RuntimeError(f"core1_layout: the linker script has no single {what} to align the "
                               "variables at (did Arduino-Pico's memmap_default.ld change?)")
    block = [
        "",
        "        /* HDDSynthPro (scripts/core1_layout.py): core 1's hard disk code first, in a",
        "           fixed order, so no other code can move it. */",
        f"        {MARK} = .;",
        *(f"        *{name}(.time_critical.core1)" for name in FILES),
        "        *(.time_critical.core1)",
        "        __core1_hd_end = .;",
        f"        . = ALIGN({BANK_PERIOD});  /* HDDSynthPro: the code after it, and the data, keep their RAM banks */",
        "        /* Core 0's per-sector code (CORE0_RAM: serveFetches and the disks' reads",
        "           and writes), in RAM so the flash cache's contents can't slow it. */",
        "        __core0_hot_start = .;",
        "        *(.time_critical.core0)",
        "        __core0_hot_end = .;",
        f"        . = ALIGN({BANK_PERIOD});",
        "",
    ]
    text = text.replace(anchor, anchor + nl.join(block) + nl)
    text = text.replace(data_anchor, f"        . = ALIGN({BANK_PERIOD});  /* HDDSynthPro (core1_layout.py): RAM banks */"
                        + nl + data_anchor)
    return text.replace(bss_anchor, bss_anchor.replace("ALIGN(4)", f"ALIGN({BANK_PERIOD})"))


def patch_file(path: str) -> None:
    with open(path, encoding="utf-8", newline="") as f:
        text = f.read()
    patched = patch_text(text)
    if patched != text:
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(patched)


try:
    Import("env")  # noqa: F821 (SCons: run by PlatformIO as a post: extra script)
except NameError:
    env = None

if env is not None:
    ld = os.path.join("$BUILD_DIR", "memmap_default.ld")  # the framework builder's target
    env.AddPostAction(ld, lambda target, source, env: patch_file(str(target[0])))
    # Regenerate (and so patch) the linker script when this file changes.
    env.Depends(ld, os.path.join(env.subst("$PROJECT_DIR"), "scripts", "core1_layout.py"))
