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


def patch_text(text: str) -> str:
    """The linker script with core 1's block after `.data`'s vtable."""
    if MARK in text:
        return text  # already patched
    nl = "\r\n" if "\r\n" in text else "\n"
    anchor = ANCHOR.replace("\n", nl)
    if text.count(anchor) != 1:
        raise RuntimeError("core1_layout: the linker script has no single `*(vtable)` line in .data "
                           "to place core 1's code after (did Arduino-Pico's memmap_default.ld change?)")
    block = [
        "",
        "        /* HDDSynthPro (scripts/core1_layout.py): core 1's hard disk code first, in a",
        "           fixed order, so no other code can move it. */",
        f"        {MARK} = .;",
        *(f"        *{name}(.time_critical.core1)" for name in FILES),
        "        *(.time_critical.core1)",
        "        __core1_hd_end = .;",
        "",
    ]
    return text.replace(anchor, anchor + nl.join(block) + nl)


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
