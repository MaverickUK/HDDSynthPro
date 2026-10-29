"""An ATA multiword DMA host, co-simulated against src/ide_dma.pio, at 1 ns
resolution. It strobes only while DMARQ is up, as a host does, and records
every timing the device must meet, so tests can check them. The device side
is modelled too: RP2350 DMA keeps the FIFOs fed (TX) or drained (RX), and
core 1 starts each burst and clears IRQ 0 after it.

Times are in nanoseconds; the PIO runs one cycle every pio_sim.CYCLE_PS.
"""
from __future__ import annotations

from dataclasses import dataclass, field

from .pio_sim import CYCLE_PS, PioSim, synced

# Sample layout (src/bus_sample.h), plus DMARQ: GPIO 31 is bit 27.
B_CS0, B_CS1, B_DIOR, B_DIOW, B_IORDY, B_RESET, B_DMARQ = 19, 20, 22, 23, 24, 26, 27
DD_MASK = 0xFFFF
IDLE = (1 << B_CS0) | (1 << B_CS1) | (1 << B_DIOR) | (1 << B_DIOW) | (1 << B_IORDY) | (1 << B_RESET)
UNDRIVEN = 0x0000  # DD with nobody driving it (the host pulls DD7 down; the rest float)


@dataclass(frozen=True)
class MdmaTiming:
    """ATA multiword DMA timings, ns (ATA/ATAPI-6's table)."""
    t0: float     # cycle time (minimum)
    tD: float     # DIOR#/DIOW# asserted pulse width (minimum)
    tE: float     # DIOR# data access (maximum): data valid this long after DIOR# falls
    tF: float     # DIOR# data hold (minimum) after DIOR# rises
    tG: float     # DIOW# data setup (minimum) before DIOW# rises
    tH: float     # DIOW# data hold (minimum) after DIOW# rises
    tKr: float    # DIOR# negated pulse width (minimum)
    tKw: float    # DIOW# negated pulse width (minimum)
    tLr: float    # DIOR# to DMARQ delay (maximum): DMARQ low this soon after the last strobe falls
    tLw: float    # DIOW# to DMARQ delay (maximum)


MDMA_MODES = {
    0: MdmaTiming(t0=480, tD=215, tE=150, tF=5, tG=100, tH=20, tKr=50, tKw=215, tLr=120, tLw=40),
    1: MdmaTiming(t0=150, tD=80, tE=60, tF=5, tG=30, tH=15, tKr=50, tKw=50, tLr=40, tLw=40),
    2: MdmaTiming(t0=120, tD=70, tE=50, tF=5, tG=20, tH=10, tKr=25, tKw=25, tLr=35, tLw=35),
}


@dataclass
class Strobe:
    fall_ns: float
    rise_ns: float = 0
    value: int | None = None          # read: what the host latched (None: DD undriven)
    valid_from_ns: float | None = None  # read: when DD began holding `value` this strobe
    held_ns: float | None = None      # read: how long DD held `value` after the rise
    dmarq_low_after_fall_ns: float | None = None  # DMARQ falling, relative to this fall


@dataclass
class DmaRun:
    strobes: list[Strobe] = field(default_factory=list)
    received: list[int] = field(default_factory=list)  # write: what reached the RX FIFO
    bursts_done: int = 0               # IRQ 0 raised and cleared
    errors: list[str] = field(default_factory=list)


def run_mdma(sim: PioSim, timing: MdmaTiming, bursts: list[list[int]], write: bool,
             core1_gap_ns: float = 2000, timeout_ns: float = 200_000,
             dma_delay_ns: float = 300) -> DmaRun:
    """Moves `bursts` (lists of 16-bit words) through the program: read, the
    device sends them; write, the host does. Core 1 starts a burst by putting
    its length less one in the TX FIFO (and, reading, DMA its words), and
    starts the next `core1_gap_ns` after IRQ 0. DMA's first word reaches the
    FIFO `dma_delay_ns` after the count, as on the hardware, where core 1 puts
    the count first and then starts the DMA channel."""
    run = DmaRun()
    feed: list[int] = []  # TX FIFO entries still to come: the count (core 1), then words (DMA)
    words_from_ns = 0.0   # DMA's words reach the FIFO from then on
    host_words = [w for b in bursts for w in b]  # write: what the host sends, in order
    host_i = 0
    next_burst = 0
    burst_due_ns = 100.0
    burst_running = False

    history: list[int] = []
    k = 0  # PIO cycles run
    t = 0.0
    strobe: Strobe | None = None
    state = "idle"  # idle | low | high
    t_next = 0.0
    dmarq_was = False
    host_dd: int | None = None   # the host driving DD (write)
    host_dd_valid_from = 0.0
    host_dd_until = 0.0
    last_read_value: int | None = None
    read_value_since = 0.0

    def device_drives() -> bool:
        return (sim.pindirs & DD_MASK) == DD_MASK

    def dd_now() -> int:
        if device_drives():
            return sim.pin_out & DD_MASK
        if host_dd is not None:
            # Before tG ahead of the rise the host's value isn't settled yet:
            # model the worst case, garbage until then.
            return host_dd if t >= host_dd_valid_from else (~host_dd & DD_MASK)
        return UNDRIVEN

    dior = diow = 1
    while t < timeout_ns:
        # ---- core 1 and DMA ----
        if not burst_running and next_burst < len(bursts) and t >= burst_due_ns:
            words = bursts[next_burst]
            next_burst += 1
            burst_running = True
            # A 16-bit DMA write to a FIFO puts the halfword in both halves.
            feed = [len(words) - 1] + ([] if write else [w * 0x10001 for w in words])
            sim.tx_fifo.append(feed.pop(0))  # the count, from core 1
            words_from_ns = t + dma_delay_ns
        while feed and t >= words_from_ns and len(sim.tx_fifo) < sim.tx_depth:
            sim.tx_fifo.append(feed.pop(0))
        if write:
            while sim.rx_fifo:
                run.received.append(sim.rx_fifo.pop(0) & DD_MASK)
        if sim.irq_flags & 1:
            sim.irq_flags &= ~1
            run.bursts_done += 1
            burst_running = False
            burst_due_ns = t + core1_gap_ns

        dmarq = bool((sim.pin_out >> B_DMARQ) & 1)
        if dmarq_was and not dmarq and strobe is not None:
            strobe.dmarq_low_after_fall_ns = t - strobe.fall_ns
        dmarq_was = dmarq

        # ---- host ----
        if state == "idle" and dmarq and t >= t_next:
            strobe = Strobe(fall_ns=t)
            run.strobes.append(strobe)
            if write:
                diow = 0
                host_dd = host_words[host_i] if host_i < len(host_words) else 0
                host_i += 1
                host_dd_valid_from = t + timing.tD - timing.tG
            else:
                dior = 0
            state, t_next = "low", t + timing.tD
        elif state == "low" and t >= t_next:
            strobe.rise_ns = t
            if write:
                diow = 1
                host_dd_until = t + timing.tH
            else:
                dior = 1
                strobe.value = dd_now() if device_drives() else None
                strobe.valid_from_ns = read_value_since if strobe.value is not None else None
            recovery = timing.tKw if write else timing.tKr
            state, t_next = "high", max(t + recovery, strobe.fall_ns + timing.t0)
        elif state == "high":
            if not write and strobe.value is not None and strobe.held_ns is None:
                if not device_drives() or (sim.pin_out & DD_MASK) != strobe.value:
                    strobe.held_ns = t - strobe.rise_ns
            if write and host_dd is not None and t >= host_dd_until:
                host_dd = None
            if t >= t_next:
                state = "idle"
                if not dmarq and next_burst >= len(bursts) and not burst_running:
                    break
        if write and host_dd is not None and device_drives():
            run.errors.append(f"{t:.0f} ns: the device drives DD while the host writes")
            break

        # Track how long DD has held its current value (read data valid time).
        v = dd_now() if device_drives() else None
        if v != last_read_value:
            last_read_value, read_value_since = v, t

        # ---- the PIO, one cycle every CYCLE_PS ----
        if t * 1000 >= k * CYCLE_PS:
            pins = IDLE | (dd_now() & DD_MASK)
            if not dior:
                pins &= ~(1 << B_DIOR)
            if not diow:
                pins &= ~(1 << B_DIOW)
            history.append(pins)
            sim.step(synced(history, k, sim.bypass_mask))
            k += 1
        t += 1.0
    else:
        run.errors.append(f"timed out at {timeout_ns:.0f} ns ({len(run.strobes)} strobes)")
    return run
