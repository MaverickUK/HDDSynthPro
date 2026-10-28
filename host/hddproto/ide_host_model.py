"""An ATA PIO host (modes 0-2), co-simulated against a PIO program and a model of
core 1, at 1 ns resolution. It honours IORDY the way a host does and records
every timing the device must meet, so tests can check them.

Times are in picoseconds internally; results report nanoseconds.
"""
from __future__ import annotations

from dataclasses import dataclass, field

from .pio_sim import CYCLE_PS, PioSim, synced

# Sample layout (src/bus_sample.h)
B_DA0, B_CS0, B_CS1, B_DIOR, B_DIOW, B_IORDY, B_INTRQ, B_RESET = 16, 19, 20, 22, 23, 24, 25, 26
DD_MASK = 0xFFFF


@dataclass(frozen=True)
class Mode0:
    """ATA PIO mode 0 timings, ns."""
    t0: float = 600    # cycle time
    t1: float = 70     # address/CS valid to strobe
    t2: float = 165    # strobe pulse width (minimum)
    t3: float = 60     # write data setup to strobe rising
    t4: float = 30     # write data hold
    t5: float = 50     # read data setup to strobe rising
    t6z: float = 30    # read data tri-state after strobe rising (maximum)
    t9: float = 20     # address/CS hold
    tA: float = 35     # IORDY setup: the host samples IORDY this long after the strobe falls
    iordy_max: float = 1250  # longest the device may hold IORDY low (tB)
    host_iordy_delay: float = 10  # host's own delay from IORDY high to strobe rising
    # Strobe recovery (t2i) between accesses with CS held. Mode 0 only sets
    # the cycle time t0, so once IORDY has stretched a strobe past it, a host
    # may strobe again this soon after the last one rose. 0: the host waits
    # t9 + t1 as before any access.
    t2i: float = 0


@dataclass(frozen=True)
class Mode1(Mode0):
    """ATA PIO mode 1 timings, ns (ATA/ATAPI-6's table; the rest as mode 0)."""
    t0: float = 383
    t1: float = 50
    t2: float = 125
    t3: float = 45
    t4: float = 20
    t5: float = 35
    t9: float = 15


@dataclass(frozen=True)
class Mode2(Mode0):
    """ATA PIO mode 2 timings, ns."""
    t0: float = 240
    t1: float = 30
    t2: float = 100
    t3: float = 30
    t4: float = 15
    t5: float = 20
    t9: float = 10


@dataclass(frozen=True)
class Mode3(Mode0):
    """ATA PIO mode 3 timings, ns. IORDY is required from mode 3 up, and a
    strobe may follow the last one t2i after it rose, CS held."""
    t0: float = 180
    t1: float = 30
    t2: float = 80
    t3: float = 30
    t4: float = 10
    t5: float = 20
    t9: float = 10
    t2i: float = 70


@dataclass(frozen=True)
class Mode4(Mode0):
    """ATA PIO mode 4 timings, ns."""
    t0: float = 120
    t1: float = 25
    t2: float = 70
    t3: float = 20
    t4: float = 10
    t5: float = 20
    t9: float = 10
    t2i: float = 25


PIO_MODES = {0: Mode0(), 1: Mode1(), 2: Mode2()}
FAST_PIO_MODES = {3: Mode3(), 4: Mode4()}  # 3 is offered; 4 fails the bus program (writes)


@dataclass
class Access:
    write: bool
    block: str          # "CMD" or "CTL"
    addr: int
    data: int = 0       # for writes
    hold_cs: bool = False  # keep CS/DA asserted afterwards (back-to-back data)


@dataclass
class Result:
    access: Access
    fall_ns: float = 0
    rise_ns: float = 0
    iordy_low_at_tA: bool = False
    iordy_low_lead_ns: float | None = None  # how long before the strobe IORDY was low (negative: after it)
    wait_ns: float | None = None            # strobe falling to IORDY seen high (if it waited)
    read_value: int | None = None           # what the host latched (None: nobody drove DD)
    release_ns: float | None = None         # strobe rising to DD released (reads)
    data_setup_ns: float | None = None      # DD driven to strobe rising (reads)


class Core1:
    """Core 1: answers reads after `latency_ns`, records writes. With
    `service_ns`, it takes at most one word from the RX FIFO per that long, as
    the real one spends time on each access; otherwise it drains it at once."""

    def __init__(self, latency_ns: float, read_value, service_ns: float = 0):
        self.latency_ps = int(latency_ns * 1000)
        self.read_value = read_value  # (block, addr) -> value
        self.service_ps = int(service_ns * 1000)
        self.writes: list[tuple[str, int, int]] = []
        self._pending: list[tuple[int, int]] = []
        self._busy_until = 0

    def ready(self, t_ps: int) -> bool:
        """True if core 1 can take a word now (and is then busy with it)."""
        if t_ps < self._busy_until:
            return False
        self._busy_until = t_ps + self.service_ps
        return True

    def on_word(self, t_ps: int, sample: int):
        block = {(1, 0): "CMD", (0, 1): "CTL"}.get(
            (((sample >> B_CS0) & 1) ^ 1, ((sample >> B_CS1) & 1) ^ 1), "NONE")
        addr = (sample >> B_DA0) & 7
        if not (sample >> B_DIOR) & 1:  # read
            self._pending.append((t_ps + self.latency_ps, self.read_value(block, addr) & 0xFFFF))
        else:
            self.writes.append((block, addr, sample & DD_MASK))

    def deliver(self, t_ps: int, sim: PioSim):
        while self._pending and self._pending[0][0] <= t_ps and len(sim.tx_fifo) < sim.tx_depth:
            sim.tx_fifo.append(self._pending.pop(0)[1])


@dataclass
class Run:
    results: list[Result]
    errors: list[str] = field(default_factory=list)
    rx_words: int = 0


def run_host(sim: PioSim, core1: Core1, accesses: list[Access], timing: Mode0 = Mode0(),
             start_ns: float = 100.0, tail_ns: float = 1000.0, timeout_ns: float = 5000.0) -> Run:
    T = {k: int(v * 1000) for k, v in timing.__dict__.items()}
    results: list[Result] = []
    errors: list[str] = []
    history: list[int] = []
    next_cycle = 0
    k = 0
    rx_words = 0

    # Host state
    i = 0
    state = "gap"
    t_next = int(start_ns * 1000)
    cs0 = cs1 = 1
    da = 0
    dior = diow = 1
    host_dd: int | None = None
    host_dd_until = 0
    cur: Result | None = None
    tA_checked = False
    iordy_seen_high_at = None
    iordy_low_since = None
    last_read: Result | None = None
    drive_since = None
    t = 0
    end_ps = None

    def device_iordy_low() -> bool:
        return bool((sim.pindirs >> B_IORDY) & 1)

    while True:
        dev_drive = sim.pindirs & DD_MASK
        if dev_drive == DD_MASK and drive_since is None:
            drive_since = t
        elif dev_drive != DD_MASK:
            drive_since = None
        iordy_low = device_iordy_low()
        if iordy_low and (sim.pin_out >> B_IORDY) & 1:
            errors.append(f"{t / 1000:.0f} ns: IORDY driven high")

        # Track when IORDY went low (for the lead-time measurement)
        if iordy_low and iordy_low_since is None:
            iordy_low_since = t
        elif not iordy_low:
            iordy_low_since = None

        # ---- host state machine ----
        if state == "gap" and t >= t_next:
            if i >= len(accesses):
                if end_ps is None:
                    end_ps = t + int(tail_ns * 1000)
            else:
                a = accesses[i]
                cur = Result(a)
                if a.block == "CMD":
                    cs0, cs1 = 0, 1
                else:
                    cs0, cs1 = 1, 0
                da = a.addr
                state, t_next = "setup", t + T["t1"]
        elif state == "setup" and t >= t_next:
            a = cur.access
            if a.write:
                diow = 0
                host_dd, host_dd_until = a.data, 1 << 62
            else:
                dior = 0
            cur.fall_ns = t / 1000
            cur.iordy_low_lead_ns = (t - iordy_low_since) / 1000 if iordy_low_since is not None else None
            fall = t
            tA_checked = False
            iordy_seen_high_at = None
            state = "strobe"
        elif state == "strobe":
            # IORDY falling after the strobe (in time or not): a negative lead.
            if cur.iordy_low_lead_ns is None and iordy_low and not tA_checked:
                cur.iordy_low_lead_ns = (fall - t) / 1000
            if not tA_checked and t >= fall + T["tA"]:
                cur.iordy_low_at_tA = iordy_low
                tA_checked = True
            end = None
            if tA_checked:
                if not cur.iordy_low_at_tA:
                    end = fall + T["t2"]
                else:
                    if iordy_seen_high_at is None and not iordy_low:
                        iordy_seen_high_at = t
                        cur.wait_ns = (t - fall) / 1000
                    if iordy_seen_high_at is not None:
                        end = max(fall + T["t2"], iordy_seen_high_at + T["host_iordy_delay"])
            if t - fall > int(timeout_ns * 1000):
                errors.append(f"access {i}: IORDY still low after {timeout_ns:.0f} ns")
                return Run(results, errors, rx_words)
            if end is not None and t >= end:
                a = cur.access
                if a.write:
                    diow = 1
                    host_dd_until = t + T["t4"]
                else:
                    cur.read_value = (sim.pin_out & DD_MASK) if dev_drive == DD_MASK else None
                    if drive_since is not None:
                        cur.data_setup_ns = (t - drive_since) / 1000
                    dior = 1
                    last_read = cur
                cur.rise_ns = t / 1000
                state, t_next = "hold", t + T["t9"]
        elif state == "hold" and t >= t_next:
            results.append(cur)
            if not cur.access.hold_cs:
                cs0 = cs1 = 1
                da = 0
            i += 1
            state = "gap"
            t_next = max(t, int(cur.fall_ns * 1000) + T["t0"] - T["t1"])
            if cur.access.hold_cs and T["t2i"] and i < len(accesses):
                # CS and DA stay valid, so no t1: only recovery and cycle time
                cur = Result(accesses[i])
                state = "setup"
                t_next = max(t, int(results[-1].rise_ns * 1000) + T["t2i"],
                             int(results[-1].fall_ns * 1000) + T["t0"])

        if host_dd is not None and t >= host_dd_until:
            host_dd = None

        # ---- read data release ----
        if last_read is not None and last_read.release_ns is None and dev_drive == 0:
            last_read.release_ns = t / 1000 - last_read.rise_ns
        if host_dd is not None and dev_drive:
            errors.append(f"{t / 1000:.0f} ns: bus contention on DD")

        # ---- the PIO and core 1, once per PIO cycle ----
        if t >= next_cycle:
            if host_dd is not None:
                dd = host_dd
            elif dev_drive:
                dd = sim.pin_out & DD_MASK
            else:
                dd = DD_MASK  # floating
            pins = (dd | (da << B_DA0) | (cs0 << B_CS0) | (cs1 << B_CS1) | (dior << B_DIOR)
                    | (diow << B_DIOW) | ((0 if iordy_low else 1) << B_IORDY) | (1 << B_RESET))
            history.append(pins)
            sim.step(synced(history, k, sim.bypass_mask))
            while sim.rx_fifo and core1.ready(t):
                rx_words += 1
                core1.on_word(t, sim.rx_fifo.pop(0))
            core1.deliver(t, sim)
            k += 1
            next_cycle += CYCLE_PS

        if end_ps is not None and t >= end_ps:
            break
        t += 1000  # 1 ns

    # Deduplicate repeated messages
    seen = []
    for e in errors:
        key = e.split(": ", 1)[-1]
        if key not in [s.split(": ", 1)[-1] for s in seen]:
            seen.append(e)
    return Run(results, seen, rx_words)
