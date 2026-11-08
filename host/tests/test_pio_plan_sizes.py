"""src/pio_plan.h's instruction counts against the programs as assembled:
the allocator decides what fits in each PIO block from them, so a program
that grows must change them too (or a module would fail to start)."""
from __future__ import annotations

import re
from pathlib import Path

import pytest

pytestmark = pytest.mark.sim

REPO = Path(__file__).resolve().parents[2]
SDFAT_SDIO = Path.home() / ".platformio/packages/framework-arduinopico/libraries/SdFat/src/SdCard/PioSdio/PioSdioCard.pio.h"


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


def test_plan_sdio_slots_match_sdfat():
    """SdFat's driver adds the five programs it lists in total_pio_length."""
    if not SDFAT_SDIO.exists():
        pytest.skip("SdFat isn't installed here (build once with `pio run`)")
    text = SDFAT_SDIO.read_text()
    lengths = dict(re.findall(r"static const struct pio_program (\w+)_program = \{.*?\.length = (\d+)", text, re.S))
    used = re.search(r"total_pio_length =(.*?);", text, re.S).group(1)
    total = sum(int(lengths[name]) for name in re.findall(r"(\w+)_program\.length", used))
    assert plan_constants()["SDIO_SLOTS"] == total
