"""src/pio_plan.h's instruction counts against the programs as assembled:
the allocator decides what fits in each PIO block from them, so a program
that grows must change them too (or a module would fail to start)."""
from __future__ import annotations

import re
from pathlib import Path

import pytest

pytestmark = pytest.mark.sim

REPO = Path(__file__).resolve().parents[2]


def plan_constants(strobe: bool = False) -> dict[str, int]:
    """The *_SLOTS constants, as built without HDDPRO_OBSERVER_STROBE (or, with
    `strobe`, with it: the first of a constant's `#if` / `#else` pair)."""
    text = (REPO / "src" / "pio_plan.h").read_text()
    out = {}
    for name, expr in re.findall(r"constexpr unsigned (\w+_SLOTS) = ([0-9 +]+);", text):
        if strobe and name in out:
            continue  # the #else line: not this build's
        out[name] = sum(int(n) for n in expr.split("+"))
    return out


def test_plan_slots_for_the_strobe_observer(assemble):
    """With HDDPRO_OBSERVER_STROBE the observer is bus_observer_strobe."""
    total = len(assemble("bus_observer", "bus_observer_strobe").instructions)
    assert plan_constants(strobe=True)["BUS_OBSERVER_SLOTS"] == total


@pytest.mark.parametrize("constant, programs", [
    ("IDE_DMA_SLOTS", [("ide_dma", "ide_dma_read"), ("ide_dma", "ide_dma_write")]),
    ("IOCS16_SLOTS", [("iocs16", None)]),
    ("IORDY_TIMER_SLOTS", [("iordy_timer", None), ("iordy_check", None)]),
    ("BUS_OBSERVER_SLOTS", [("bus_observer", "bus_observer")]),
    ("BUS_TIMING_SLOTS", [("bus_timing", None)]),
])
def test_plan_slots_match_the_programs(assemble, constant, programs):
    total = sum(len(assemble(name, program).instructions) for name, program in programs)
    assert plan_constants()[constant] == total


def test_plan_sdio_slots_match_the_driver(assemble):
    """Our copy of SdFat's driver (src/sdio_card.pio) loads these five."""
    total = sum(len(assemble("sdio_card", name).instructions)
                for name in ("cmd_rsp", "rd_clk", "rd_data", "wr_data", "wr_resp"))
    assert plan_constants()["SDIO_SLOTS"] == total
