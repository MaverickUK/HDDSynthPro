# The drive choosing its own bus program: a design

**Design, 4 Nov 2026 (0.30.0-dev.9); nothing built.** How the drive could pick the fastest safe bus program for the computer it's plugged into, so nobody has to know what an ISA card or a PIIX is. It needs evidence from more machines first: [what to record](#what-to-record-on-each-pc) on each PC is at the end, and in [bench-checklist.md](bench-checklist.md).

## Today

| Program | Chosen by | Speed (the Evesham, PIO 3) | Safe with |
|---|---|---|---|
| **Early answers** (`ide_bus_early`) | the default | reads 1.78 MB/s, writes 1.24 | everything tried: the ICL 386's ISA card, the Pentium 200's PIIX, the Evesham, the USB adapter |
| **The data lane** (`ide_bus_data` + `ide_data_in`) | `early_answers = off` | reads **2.18 MB/s**, writes **1.98** | hosts that wait for IORDY on every read: the Evesham, the USB adapter |
| `ide_bus` with `iordy_watchdog` | `isa = on` | (the 386: 1.51 reads, 0.72 writes) | the ICL 386, where it was proven first |
| `ide_bus_noiordy` | `iordy = off` | | the Amiga 1200, in progress |

The program is chosen once, when core 1 starts (`ide_bus::begin`), from `HDDSYNTH.INI` or console `P`, which restarts the board. The CD-ROM builds always run early answers (the lane was written for the hard disk's 256-word blocks).

**Early answers are the safe default** and the ICL 386 boots on them, so `isa = on` is no longer needed. **What a choice could gain is the lane's speed** on machines that can take it: about 20% faster reads and 60% faster writes on the Evesham. Only in PIO: with multiword DMA the data goes by DMA whatever the program.

## What makes the lane unsafe somewhere

Two kinds of host, and the lane is wrong for both:

1. **ISA cards.** They decode the drive's chip selects from the address alone, so memory and refresh cycles select the drive with no strobe, and they pass IORDY to the ISA bus's IOCHRDY. A drive that holds IORDY at a chip select with no strobe freezes the PC (the ICL 386, 0.21.x). Early answers survive it: their program tells core 1 about every chip select, and core 1 lets IORDY go after 23 µs without a strobe (`checkIordyStall`; 44,002 releases in a session on the 386). The lane can't: core 1 hears of an access only at the strobe, and both its programs fill their PIO block, so there's no room to report chip selects.
2. **Hosts that don't wait for IORDY on register reads.** The Pentium 200's PIIX runs "compatible" timing while its BIOS detects the drive, and ignores IORDY there; the lane answers register reads from core 1 at the strobe, too late, so the BIOS finds no drive ([no-iordy.md](no-iordy.md)). Early answers ask core 1 at the chip select, which got the Pentium through its BIOS. **Windows 95's own disk driver later went back to compatible timing** when it started, so a host that waits for IORDY at one moment may not later.

## The evidence the drive already collects

| Signal | What it shows | Known values |
|---|---|---|
| `dior_no_cs` (and the `NOCS` line, ms after RESET# release) | DIOR# with no chip select, while no DMA is offered (`bus_observer`): an ISA card passes every I/O read the PC makes | ICL 386: about 19.9 million in a session (0.27.0). USB adapter: 0. **Evesham, Pentium: not yet measured** (the counter came after) |
| `iordy_ignored` (`NOIORDY` lines) | reads the host ended while IORDY was still low (`iordy_check.pio`): a host that didn't wait | Pentium (BIOS detection): above 0. Evesham, USB adapter: 0. Under early answers it also counts reads whose data was already on DD when the strobe ended (the program lets IORDY go 40 ns after driving DD): the data was good, but the host still didn't wait, which is what the rule needs to know |
| `iordy_slow` (with `iordy_max_ns`) | reads IORDY held over 400 ns (`iordy_timer.pio`) | On the lane, both waited through every long hold: the Evesham about 1,400 reads over 1,250 ns a benchmark, the USB adapter about 10 a speed run, `iordy_ignored` 0 on both. Under early answers most holds are shorter, so a long one is the test of whether a host waits |
| `rd_t1_ns`, `rd_strobe_ns` (`bus_timing.pio`, when no DMA is offered) | each read's chip-select-to-strobe time and strobe length: compatible timing has a t1 of a cycle or two | Pentium, Windows 95's driver: t1 80 ns, strobes 175-195 ns |
| `iordy_early` | reads that started before IORDY was low | most reads in PIO 3, harmlessly: not a signal by itself |

## The design

### A setting, opt-in first

`bus = auto | early | lane | ide_bus` in `HDDSYNTH.INI`. `early_answers` and `isa` keep working as today (they pin the program, as `bus = early` or `ide_bus` would), and console `P` still overrides everything until a power cycle. **`auto` is opt-in until it has run on several machines, then the default.**

### The rule

Every power-on, and every RESET# and soft reset (SRST), starts in early answers, whatever happened before. **The drive moves to the lane only when all of these hold since the last reset:**

1. **No ISA signature:** `dior_no_cs` hasn't moved. An ISA card shows it constantly (it passes every I/O read the PC makes, many per millisecond), so a quiet minute is strong evidence. The `NOCS` times recorded on ISA machines say how short this wait can be.
2. **The host waits for IORDY:** no read it didn't wait for (`iordy_ignored` unchanged), **and** proof that it does wait: at least N reads (say 100) held longer than the host's own shortest strobe (`iordy_slow` counts holds over 400 ns; `rd_strobe_ns` gives the shortest strobe), every one waited for. Without that proof a host that ignores IORDY looks the same as one that waits, because under early answers most holds end before the host's strobe would anyway: the ISA card's 375-500 ns strobes outlast nearly every hold. The counters exist; the rule needs `iordy_slow` per reset rather than since `r`. (An earlier draft said `iordy_check` over-counted and needed fixing: the reads it counts beyond the bad ones are real cases of a host not waiting, so it's the right count.)
3. **Past detection:** at least M data sectors served (say 1,000) since the reset, so the BIOS's detection and an operating system's driver start-up, which is when hosts re-identify in their slowest timing, have gone by.
4. **No DMA offered:** with DMA offered the data goes by DMA anyway, and `dior_no_cs` and the timing probe aren't available.

**And it moves back to early answers:**
- at every RESET# and SRST (hosts re-detect after one, perhaps in compatible timing);
- at once if a read isn't waited for while on the lane (`iordy_check` keeps running, in another PIO block), or `dior_no_cs` moves.

### Switching while the drive runs

Today switching means restarting the board. In `auto`, core 1 would swap block 0's program itself, which the bus layer doesn't do yet:
- **When:** only when the host is quiet: no command running, neither BSY nor DRQ, no chip select asserted, and nothing from the host for, say, 10 ms (between commands, which a DOS program or BIOS leaves plenty of). Back to early answers at a reset happens while the host holds RESET#, when the bus is idle by definition.
- **How:** stop the state machine, remove the program, load the other one (each fills block 0's 32 instructions on its own: early answers 32, the lane's two 27 + 5), set DD's drive strength (8 mA for early answers, the default 4 mA on the lane), claim or free the lane's DMA channel, and start it with empty FIFOs, as `ide_bus::restart()` does at a reset. Tens of microseconds, from RAM (it runs on core 1, which must never reach flash: `check_core1_ram.py`).
- **If the host reads during the gap,** DD floats and the pull-ups give FFh: BSY set, so a host polling status waits. Keeping the switch to quiet moments makes that rare; the `pio_sim` tests and the USB adapter's slices check it doesn't happen during a transfer.

### What it leaves open

- **A host that changes timing without a reset.** If Windows 95's driver moved the PIIX to compatible timing with no SRST, the lane would answer a few register reads too late before the drive noticed, and those reads would be wrong (the Pentium's boots with that driver already corrupted files on early answers). Rule 3 and the switch back on any unwaited read narrow it; only the Pentium can show whether it happens. Until then, `auto` stays opt-in.
- **An ISA card the signature misses.** None known: every I/O read reaches the drive's lines on an ISA card, so the count should be in the millions. The recordings below test that on every ISA machine.
- **The alternative: a lane safe on ISA cards.** If the lane could release IORDY on a stall as early answers do, rule 1 would go. It needs the program to report chip selects to core 1, which needs instructions the block doesn't have; the register table ([no-iordy.md](no-iordy.md)) reshuffles the blocks and might make room. Worth weighing when the table is built.

## Cost

- **The rule and the setting:** core 0 watches the counters it already has (`dior_no_cs`, `iordy_ignored`, `iordy_slow` and the shortest strobe, sectors served) and asks core 1 to switch: about half a day, host-tested.
- **Switching programs live:** core 1's part, in `ide_bus.cpp`, with `pio_sim` tests of a switch between accesses and the USB adapter's slices 1-4 and `-m speed` while switching often (a debug flag that switches every second): about a day.
- **The machines:** each PC run in `auto` with the bus observer on, watching for `bus_bad`, `iordy_ignored` and switches back.

## What to record on each PC

For the design's numbers (how quickly an ISA card shows itself; whether any host stops waiting for IORDY without a reset). At each of three moments: **after the BIOS has found the drive, at the DOS prompt, and after a benchmark** (and in Windows, if it runs), type `s` and note:

| Field | Why |
|---|---|
| The PC, its IDE controller (ISA card and its make, or the chipset: PIIX, VIA...), its BIOS and PIO mode | which kind of host |
| `bus=` | the program it ran |
| `dior_no_cs` and the `NOCS ... ms` line (or none) | the ISA signature, and how soon it shows |
| `iordy_ignored` and any `NOIORDY` lines (when) | whether the host stopped waiting, and when |
| `rd_t1_ns`, `rd_strobe_ns` (with `r` before each moment) | its timing at that moment: compatible or not |
| `iordy_slow`, `iordy_max_ns` | reads held long: did the host wait through them? (with `iordy_ignored`) |
| `bus_sectors`, `bus_bad` | the usual health check |

Then, **on any PC that isn't an ISA machine,** `P l` (the lane) and boot again: does it find the drive, run the benchmark, show `iordy_ignored=0`? That says directly whether the rule would have been right. **Never `P l` on an ISA machine:** without the watchdog it may freeze.
