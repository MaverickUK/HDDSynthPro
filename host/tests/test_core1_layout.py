"""scripts/core1_layout.py: core 1's hard disk code placed first in RAM."""
import importlib.util
from pathlib import Path

import pytest

pytestmark = pytest.mark.sim

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "core1_layout.py"
spec = importlib.util.spec_from_file_location("core1_layout", SCRIPT)
core1_layout = importlib.util.module_from_spec(spec)
spec.loader.exec_module(core1_layout)

# The part of Arduino-Pico's rp2350 memmap_default.ld the patch works on.
LD = """    .data : {
        __data_start__ = .;
        *(vtable)

        *(.time_critical*)

        *(.text*)

        *(.data*)
        *(.sdata*)
    } > RAM AT> FLASH

    .tbss (NOLOAD) : {
        . = ALIGN(4);
        __bss_start__ = .;
    } > RAM

    .heap (NOLOAD):
    {
        __end__ = .;
        . = ORIGIN(RAM) + LENGTH(RAM);
        __HeapLimit = .;
    } > RAM
"""


def test_core1_code_goes_first_in_the_listed_order():
    out = core1_layout.patch_text(LD)
    order = [out.index(f"*{name}(.time_critical.core1)") for name in core1_layout.FILES]
    assert order == sorted(order)
    assert out.index("*(vtable)") < out.index("__core1_hd_start = .;") < order[0]
    assert order[-1] < out.index("        *(.time_critical.core1)") < out.index("__core1_hd_end = .;")
    assert out.index("__core1_hd_end = .;") < out.index("*(.time_critical*)")


def test_patching_twice_changes_nothing():
    once = core1_layout.patch_text(LD)
    assert core1_layout.patch_text(once) == once


def test_crlf_scripts_keep_their_line_endings():
    out = core1_layout.patch_text(LD.replace("\n", "\r\n"))
    assert "__core1_hd_start" in out
    assert "\n" not in out.replace("\r\n", "")


def test_a_changed_framework_script_fails_the_build():
    with pytest.raises(RuntimeError, match="vtable"):
        core1_layout.patch_text(LD.replace("*(vtable)", "*(vtables)"))


def test_the_data_keeps_its_ram_banks_whatever_the_code_size():
    # The end of core 1's block, .data's variables and .bss start on 32-byte
    # boundaries: code growing or shrinking moves the data in whole bank periods.
    out = core1_layout.patch_text(LD)
    align = f". = ALIGN({core1_layout.BANK_PERIOD});"
    assert core1_layout.BANK_PERIOD == 32
    assert out.index("__core1_hd_end = .;") < out.index(align) < out.index("*(.time_critical*)")
    data = out.index("        *(.data*)")
    assert out.rindex(align, 0, data) > out.index("*(.text*)")
    assert out.index(align + "\n        __bss_start__ = .;") > data
    assert out.count(align) == 4  # after core 1's block, after core 0's, .data's variables, .bss


def test_core0_per_sector_code_follows_core1s_block():
    # CORE0_RAM (serveFetches and the disks' reads and writes): in RAM, right
    # after core 1's block, before anything else that runs from RAM.
    out = core1_layout.patch_text(LD)
    assert (out.index("__core1_hd_end = .;") < out.index("__core0_hot_start = .;")
            < out.index("*(.time_critical.core0)") < out.index("__core0_hot_end = .;")
            < out.index("        *(.time_critical*)"))


def test_a_framework_script_without_the_data_anchors_fails_the_build():
    for missing in ("        *(.data*)\n", "        __bss_start__ = .;\n"):
        with pytest.raises(RuntimeError, match="align the variables"):
            core1_layout.patch_text(LD.replace(missing, ""))


def test_the_aligned_rings_sit_at_the_top_of_ram_below_the_heaps_end():
    # In .bss, a little more code pushed the 16 KB ring to the next 16 KB
    # boundary and the link ran out of RAM (0.35.0-dev.5).
    out = core1_layout.patch_text(LD)
    limit = f". = ORIGIN(RAM) + LENGTH(RAM) - {core1_layout.RINGS_BYTES:#x};"
    assert out.index(limit) < out.index("__HeapLimit = .;")
    places = [out.index(f"    {name} (NOLOAD) : {{") for name in core1_layout.RINGS]
    assert out.index("__HeapLimit = .;") < places[0] < places[1] < out.index("__ram_rings_end = .;")
    assert "ASSERT(__ram_rings_end <= ORIGIN(RAM) + LENGTH(RAM)" in out
    # smallest first, so both land aligned: 4 KB, then 16 KB at a 16 KB boundary
    assert core1_layout.RINGS == (".ram_rings.wring", ".ram_rings.observer")
    assert (0x80000 - core1_layout.RINGS_BYTES + 0x1000) % 0x4000 == 0


def test_the_hard_disk_files_are_listed_and_the_cd_rom_files_are_not():
    # The CD-ROM's code (CORE1_RAM_CD) goes after all of this, in .after_data.
    assert core1_layout.FILES[0] == "ata_device.cpp.o"
    for cd in ("atapi_device.cpp.o", "hdcd_device.cpp.o", "mmc.cpp.o"):
        assert cd not in core1_layout.FILES
