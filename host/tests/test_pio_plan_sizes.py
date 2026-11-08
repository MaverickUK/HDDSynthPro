"""src/pio_plan.h's instruction counts against the programs as assembled:
the allocator decides what fits in each PIO block from them, so a program
that grows must change them too (or a module would fail to start)."""
from __future__ import annotations

import re
from pathlib import Path

import pytest

pytestmark = pytest.mark.sim

REPO = Path(__file__).resolve().parents[2]


def plan_constants() -> dict[str, int]:
    text = (REPO / "src" / "pio_plan.h").read_text()
    out = {}
    for name, expr in re.findall(r"constexpr unsigned (\w+_SLOTS) = ([0-9 +]+);", text):
        out[name] = sum(int(n) for n in expr.split("+"))
    return out


@pytest.mark.parametrize("constant, programs", [
    ("IDE_DMA_SLOTS", [("ide_dma", "ide_dma_read"), ("ide_dma", "ide_dma_write")]),
    ("IOCS16_SLOTS", [("iocs16", None)]),
    ("IORDY_TIMER_SLOTS", [("iordy_timer", None), ("iordy_check", None)]),
    ("BUS_OBSERVER_SLOTS", [("bus_observer", None)]),
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
