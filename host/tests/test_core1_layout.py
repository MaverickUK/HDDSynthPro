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
    } > RAM AT> FLASH
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


def test_the_hard_disk_files_are_listed_and_the_cd_rom_files_are_not():
    # The CD-ROM's code (CORE1_RAM_CD) goes after all of this, in .after_data.
    assert core1_layout.FILES[0] == "ata_device.cpp.o"
    for cd in ("atapi_device.cpp.o", "hdcd_device.cpp.o", "mmc.cpp.o"):
        assert cd not in core1_layout.FILES
