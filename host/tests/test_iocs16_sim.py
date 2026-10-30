"""Runs the real src/iocs16.pio in the simulator: IOCS16# (GPIO 2) is pulled
low while the host addresses the data register (CS0# low, DA2-0 = 0), soon
enough for an ISA bus, which samples it from the address before the strobe,
and released for every other register; it never glitches while the address
holds still. No hardware needed."""
from __future__ import annotations

import pytest

from hddproto import pio_sim
from hddproto.pio_sim import PioSim

pytestmark = pytest.mark.sim

NS = 1000  # ps
CYCLE_NS = pio_sim.CYCLE_PS / NS
IOCS16_PIN = 2
# Pins as the program sees them from IN_BASE = GPIO 20: DA0-2, CS0#, CS1#,
# then the LED, DIOR#, DIOW# (strobes idle high).
CS0, CS1 = 1 << 3, 1 << 4
STROBES = (1 << 6) | (1 << 7)
IDLE = CS0 | CS1 | STROBES
# The deadline: ATA-1 has IOCS16# valid within 90 ns of the address; leave
# room for the pin's own delay and the host's pull-up.
LIMIT_NS = 50


@pytest.fixture(scope="module")
def program(assemble):
    return assemble("iocs16")


def address(block: str, da: int) -> int:
    cs = CS1 if block == "CMD" else CS0  # the other chip select stays high
    return cs | STROBES | da


def trace(program, wave, duration_ns: int) -> list[tuple[float, bool]]:
    """Runs the program (configured as iocs16.cpp configures it) against
    `wave(t_ns) -> pins`; returns (t_ns, IOCS16# pulled low) per cycle."""
    sim = PioSim(program, sideset_base=IOCS16_PIN)
    history, out = [], []
    for k in range(int(duration_ns / CYCLE_NS)):
        t = k * CYCLE_NS
        history.append(wave(t))
        sim.step(pio_sim.synced(history, k, 0))
        out.append((t, bool((sim.pindirs >> IOCS16_PIN) & 1)))
    return out


def changes(out) -> list[tuple[float, bool]]:
    return [(t, low) for (t, low), (_, before) in zip(out[1:], out) if low != before]


def test_idle_never_asserted(program):
    assert not any(low for _, low in trace(program, lambda t: IDLE, 1000))


@pytest.mark.parametrize("start_ns", [100, 101.2, 102.5, 103.7, 105, 107.5, 112.5])
def test_data_register_asserted_in_time_and_steady(program, start_ns):
    end_ns = start_ns + 500
    out = trace(program, lambda t: address("CMD", 0) if start_ns <= t < end_ns else IDLE, 1000)
    edges = changes(out)
    assert len(edges) == 2, f"IOCS16# glitched: {edges}"
    (t_low, low), (t_high, high) = edges
    assert low and not high
    assert t_low - start_ns <= LIMIT_NS, f"asserted {t_low - start_ns:.1f} ns after the address"
    assert t_high - end_ns <= LIMIT_NS, f"released {t_high - end_ns:.1f} ns after the address went"


@pytest.mark.parametrize("block,da", [("CMD", 1), ("CMD", 2), ("CMD", 7), ("CTL", 0), ("CTL", 6), ("CTL", 7)])
def test_other_registers_not_asserted(program, block, da):
    """8-bit registers must stay 8-bit: with IOCS16#, the ISA bus would take
    an odd register's byte from the wrong lane."""
    out = trace(program, lambda t: address(block, da) if 100 <= t < 600 else IDLE, 1000)
    assert not any(low for _, low in out)


def test_back_to_back_status_then_data(program):
    """A status poll straight into a data read (1F7h then 1F0h): released for
    the first, asserted in time for the second, released after."""
    def wave(t):
        if 100 <= t < 400:
            return address("CMD", 7)
        if 400 <= t < 700:
            return address("CMD", 0)
        return IDLE
    edges = changes(trace(program, wave, 1000))
    assert [low for _, low in edges] == [True, False]
    assert edges[0][0] - 400 <= LIMIT_NS
    assert edges[1][0] - 700 <= LIMIT_NS
