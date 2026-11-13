# The register table: every read answered without core 1

**Status (14 Nov 2026, morning): on the rig. The Innostor bridge passes slices 3 and 4 in PIO 3 since R0-R2 keep their answer on DD's output latch before the strobe (*14 Nov, morning*); offered PIO 2 it passed before. The JMicron adapter passes slices 3 and 4 on it too, and reads 1.69 MB/s and writes 1.77, the fastest PIO through it yet. Branch `register-table`, console `P t`; slices 1-2 need console `V` to work in this mode (*Open*). The bench hooks (console `Q`, `R`, `Z`, `G`, the write log and observer snapshots, extra `s` fields, `HDDPRO_FETCH_DELAY_US`) come out before it merges.**

## Why

Every bus program today answers register reads from core 1, between about 0.4 and 2.5 µs after the strobe. Hosts that wait for IORDY get the answer late but right. Hosts that don't wait get whatever is on the cable:

- **The Innostor USB bridge** (`1F75:0611`, 12 Nov 2026), until it has set a transfer mode: chip select 170 ns before DIOR#, a fixed 600 ns strobe. On `ide_bus` it latched each register's value one read late (Error read as the Status before it, so the drive "failed" its self-test). Early answers was in time but stale: the bridge holds its chip select between reads, so an answer prepared at the chip select belonged to the read before (a whole IDENTIFY block one word late; sector data read on a Status that had already changed). With early answers it once sized the disk right; on `ide_bus` it reported 2.2 TB, from a late IDENTIFY.
- **The Pentium 200's PIIX** in compatible timing: chip select and strobe together, strobes of 175-195 ns ([no-iordy.md](no-iordy.md)).
- **The Amiga 1200's Gayle**, which checks IORDY before each strobe.

And on hosts that wait, the same late answers are the slow reads in *Known issues*: early answers up to 2.1 µs, the CD-ROM's Status polls up to 2.4 µs, the adapter's Device reads at detection 3.6-5.5 µs.

## The idea

Core 1 keeps the drive's registers in the PIO's own FIFO storage, and the PIO answers each read from there at the strobe, in a few cycles. Nothing on the read path waits for core 1, so every answer is both fresh (read at the strobe, not prepared at the chip select) and fast (within about 60 ns of DIOR# falling).

The RP2350's `.fifo txget` mode turns a state machine's RX FIFO into four 32-bit registers: core 1 writes them (`pio->rxf_putget[sm][i]`, or a DMA channel does), and the program reads one with `mov osr, rxfifo[y]` in one cycle. Four registers per state machine is the limit, so the registers are shared out between three state machines, by address.

## Block 0: the read side (owns DD and IORDY)

| State machine | Serves | Its four entries (index DA1-0) |
|---|---|---|
| R0 | CS0#, DA2 = 0 | 0 Data (never driven here: D's), 1 Error, 2 Sector Count, 3 LBA Low |
| R1 | CS0#, DA2 = 1 | 0 LBA Mid, 1 LBA High, 2 Device, 3 Status |
| R2 | CS1#, DA2 = 1 | 0, 1 not the drive's, 2 Alternate Status, 3 Drive Address |
| D | IORDY, and the data register's pace | its TX FIFO: a control word per data read, fed by DMA |

Every pin goes through the input synchroniser (no bypass): with the PIIX's chip select and strobe together, a bypassed DIOR# arrived before the address it went with.

Each entry is the TX answer word `ide_bus` already uses: bits 0-15 the value for DD0-15, bits 16-31 which of them to drive (all, or none for an address that isn't the drive's, such as an ISA card's floppy registers under CS1#).

**R0-R2 run one program, `ide_table_reg`,** each with its claim (DA2, CS0#, CS1#) in ISR, loaded once:

```
rise:   wait 1 gpio 26          ; DIOR# high
        wait 0 gpio 26          ; DIOR# falls: a read
        mov osr, pins
        out null, 18            ; DD0-15, DA0-1
        out x, 3                ; DA2, CS0#, CS1#
        mov y, isr              ; my claim
        jmp x!=y rise           ; not mine
        mov osr, pins
        out null, 16
        out y, 2                ; DA1-0: the entry
        mov osr, rxfifo[y]      ; value and drive mask
        out pins, 16
        out pindirs, 16
        wait 1 gpio 26          ; hold until DIOR# rises
        mov pindirs, null       ; release DD (then wrap)
```

14 instructions, about 12 cycles (60 ns) from DIOR# falling to DD driven, plus the input synchroniser.

**Data words go through R0's entry 0**, so only R0-R2 ever drive DD (two state machines writing DD's output latch in one read would race). After each data read a DMA pair puts the next word there and the next control word in D's TX FIFO.

**D runs `ide_table_data`:** it pulls IORDY low at each strobe and releases it after a fixed hold long enough for R0-R2 to have driven DD (about 80 ns). On a data read it pulls that read's control word, but only once R0 has read its entry (seven cycles' wait: pulling at once let the DMA put the next word there first). A non-zero control word marks the block's last word: it's the busy Status entry, and D pushes it for a DMA channel to write into the Status entries. Hosts that wait get that short wait; hosts that don't have strobes of 165 ns or more (PIO mode 0) and the answer well inside them. IORDY is only ever held during a strobe, never at a chip select, so an ISA card's address-only cycles can't stall on it (no watchdog needed).

## Block 1: the write side (reads pins only)

| State machine | Does |
|---|---|
| W | every write, sampled as `ide_bus` samples it, into a DMA ring that core 1 drains |
| WD | data register writes, by DMA straight into the sector buffer, 256 words per block |
| C | a Command write: DMA writes BSY into R1's Status and R2's Alternate Status entries at once |
| O | `bus_observer`, as now; core 1 also reads its ring for Status reads, which clear INTRQ |

`iordy_timer` and `iordy_check` are off in this mode: IORDY is never held long, so there's nothing for them to measure.

## Busy, without core 1

ATA lets a host read Status 400 ns after a Command write, or at once after a data block, and expects BSY (or the next block's DRQ). Core 1 can't promise 400 ns, so DMA does it:

- **After a Command write:** C pushes one word; a DMA channel copies the busy Status word core 1 keeps ready into the Status entries. Core 1 then runs the command and writes the real registers.
- **After a data-in block:** the channel feeding D's TX FIFO completes after the block's last word, and chains to a channel that writes the busy word. (It completes when the last word enters the FIFO, up to four words before the host reads it; a host reading Status mid-block would see BSY early. None seen does.)
- **After a data-out block:** WD's channel completes after the 256th word, and chains the same way.
- **SRST** (Device Control bit 2): core 1, as now; ATA gives the drive 5 µs before the host clears it.

## Core 1

Core 1 keeps the device logic as it is. What changes is how it shows its state:
- After every state change (a write, a command step, a fetch done, a reset), it writes the register entries: a `publish()` of eight words.
- Before showing DRQ for a data-in block, it starts the channel feeding D; for data-out, WD's channel.
- It drains W's ring for writes and Status reads, in place of the bus FIFO.

### Core 1's loop in table mode (step 3)

Hard disk (`ata::Device`) only at first: no CD-ROM, no DMA offered.

1. **Drain W's ring:** each write sample to `dev.write()` or `dev.queueCommand()`, except data register writes (WD has them).
2. **Drain the observer's ring for Status reads:** `dev.consumeRead(Status)`, which acknowledges the interrupt (core 1 keeps its own read position; core 0 keeps checking sectors as now).
3. **Run a queued command,** post and take fetches, as the other loops do.
4. **Data in:** when `dev.dataInRemaining()` gives a block and none is streaming, prime R0's entry 0 with its first word and D's TX FIFO with the first four control words, set the pair's sources, then publish (the host now sees DRQ). When the block-end busy has fired, `dev.consumeDataWords()` moves the device on.
5. **Data out:** when the device is receiving, start WD's channel on the buffer (`dev.dataOutRemaining()`, new), then publish. When it completes, `dev.consumeDataOut()` (new) finishes the block.
6. **Publish** when anything changed: `peek()` each register into its entry (eight register writes).

**The busy rule.** C's and the block ends' DMA writes of BSY come before core 1 knows of the command or the block's end, so core 1 must never publish a Status over them that predates them. Their channels count down per event (TRANS_COUNT); core 1 compares those counts with the commands it has taken from W's ring and the blocks it has finished, and while either is behind, it publishes BSY in the Status entries whatever the device says.

### DMA channels

| Channel | DREQ | From | To |
|---|---|---|---|
| F1 | D's TX | the block's next word | R0's entry 0; chains F2 |
| F2 | (chained) | the next control word | D's TX FIFO |
| B1 | D's RX | the busy word D pushed | R1's Status entry; chains K2 |
| C1 | C's RX | C's word | a dummy; chains K1 |
| K1 | (chained) | the busy word | R1's Status entry; chains K2 |
| K2 | (chained) | the busy word | R2's Alternate Status entry |
| WD | WD's RX | 16-bit data words | the sector buffer, 256; chains K1 |
| W | W's RX | write samples | a ring core 1 drains |

With `bus_observer`'s, nine. F1, F2, K1 and K2 run one transfer per trigger, reloaded each time (TRANS_COUNT's reload value); B1, C1 and W run endless, and their counts are the event counters.

## Fitting it in

| Block | Table mode |
|---|---|
| 0 | `ide_table_reg` (15) on R0-R2, `ide_table_data` (14): 29 of 32, 4 state machines |
| 1 | W (4), WD (8), C (7) and `bus_observer` (8): 27 of 32 |
| 2 | as now: SDIO on the PCB; `iocs16` and `bus_timing`, or `ide_dma`, on the BB48 |

DMA channels: D's feed, WD's, the two busy writes (command, block end), W's ring and the observer's: six.

Not with DMA offered, at first: `ide_dma` needs DD in block 0 for its bursts.

## Testing

1. **The simulator:** `pio_sim` learns `.fifo txget`, `mov osr, rxfifo[]`, several state machines sharing pins, and a DMA write into an entry. A host model that never waits (PIO mode 0's timings, and the PIIX's: no t1, 175 ns strobes) must read every register right, and Status as BSY 400 ns after a Command write. One that waits (PIO 3 and 4) must see IORDY within 35 ns of the strobe.
2. **The Innostor bridge on the Mac:** detection, the size, `-m slice2`-style reads, writes.
3. **The old USB adapter:** slices 1-4 and a soak, for hosts that wait.
4. **The Evesham, then the Pentium 200 and the ICL 386.**

It comes in as a fourth bus program (console `P t`), so the others stay as measured until it has earned its place.

## What the hardware taught (12 Nov 2026)

- **FIFO registers only after `pio_sm_init`:** written before, the entries kept stale values, and R0's entry 0 answered every data read over D.
- **Emptying a TX-joined FIFO:** toggling FJOIN_RX (as `pio_sm_clear_fifos` does) leaves it as it was; toggling FJOIN_TX empties it behind the DMA's back, and the stream's channel then stopped after its first FIFO-full. D's reset pulls the words out instead. The data lane uses the FJOIN_RX toggle too: a block cut short may leave words in `ide_data_in`'s FIFO.
- **Never abort an idle DMA channel** before restarting it.
- **IORDY on data reads:** the Innostor bridge's IORDY rises about a microsecond after release, so holding it for every word (1.3 µs each) made it give up on sectors. D holds IORDY on register reads only; DIOR# skips the synchroniser in block 0 so D can decide inside tA.
- **Publish after starting the block:** DRQ published before the stream started let the host read before it.
- **INTRQ** is C's: low as a Command write starts, raised by core 1 on each new interrupt event (a command done, a block started or done), not on a change of level: with data ready at once, the interrupt went off and on within one pass and INTRQ was never raised again (the JMicron adapter waited for good). A Status read acknowledges it only if it comes after the raise in the observer's ring, so a late-seen read for one block can't clear the next block's interrupt.

### The Innostor bridge, 12 Nov 2026 (evening)

Measured with the bus observer's register print (`HDDPRO_OBS_REGS`, its strobe lengths with `HDDPRO_OBSERVER_STROBE`), a write log of core 1's ring, and core 0 logging the control lines' edges (console `G`: INTRQ, DIOR#, DIOW#, IORDY, CS0#, DA0-2, from the next read command). The CF card was recorded the same way on the listen build.

- **Answers from the strobe were too slow for some reads:** the first Status read after a Command write came back undriven (7Fh, the bare bus), which the bridge took as ready. R0-R2 now spin with their entry ready and drive DD within about 35 ns of DIOR# (they were about 90 ns from it), and so does D. The "75 ns strobe" that pointed there was an artefact: the observer's strobe count wraps every 960 ns, and the reads were about 1 µs.
- **A host can change the address with the strobe:** the bridge goes from its last Status poll to the first data read with DA2 changing as DIOR# falls (no t1). R1 caught the address half-way (LBA Mid) and its write to the output latch replaced D's first data word: word 0 of every sector came back as the LBA Mid register (0x001b for LBA 7000). D now looks at the address 2 cycles after the strobe, and R0-R2 once more no earlier than D, so a data read always wins (`test_the_first_data_read_after_status_polls_with_no_address_setup`).
- **The busy word is D0h** (BSY, DRDY, DSC), as drives show, never FFh (C's word, which went straight into Status) or a bare 80h.
- **Registers drive DD0-7 only**, as drives do, leaving DD8-15 to the host's pulls (the CF card reads as `ffd0`, the table drove `00d0`). It made no difference to the bridge.
- **The bridge offered PIO 2 (`mode = pio2`) works:** it polls Status until DRQ, then reads. The whole 64 MB test disk read clean (1.23 MB/s), and 40 random writes of 1-8 sectors read back right.
- **Offered PIO 3, it doesn't:** it sets flow-control mode 3 (SET FEATURES 03h, 0Bh) and doesn't wait for DRQ before the data. Its first data read after each Command starts while IORDY is low, as if D had taken it for a register read: the bridge may read Status and go on to the data register within one strobe, which a real drive's outputs follow (they track the address and the drive's state while DIOR# is low) and ours, decided at the strobe, don't. The CF card's first Status read after a Command lasted 2.2 µs, as if the bridge held it until BSY cleared. Not proven: core 0's edge log sampled only every 690 ns there. Next: a PIO logic analyser, and, if it shows that, register outputs that follow the address and Status while DIOR# is low.
- **Ruled out for PIO 3:** READ MULTIPLE (accepting SET MULTIPLE made no difference), IORDY held or never held on register reads, INTRQ (low throughout), the state the drive is in (the first read after a clean detection fails too).

## 13 Nov 2026: the outputs follow the bus (simulated; not yet on hardware)

A review at the desk, with no board or adapter to hand, so everything here rests on the simulator.

**What was wrong.** R0-R2 and D each decided once per read whose it was, from the address as of 2 cycles (5-10 ns) after DIOR# fell. A host that changes its address *with* the strobe can settle later than that. The host model gained such a host (`address_lag`: from one held address to the next, the address changes that long after the strobe), and with 10 ns or more the first data read after a Status poll went wrong in every mode: D took it for a register's (IORDY low, no word driven, the word kept), R1 answered Status or nobody answered, and the block ran one word late from there, so it never finished and Status stayed busy. That is what the bridge showed in PIO 3: "its first data read after each Command starts while IORDY is low, as if D had taken it for a register read". PIO 2 working would then be the bridge's address landing inside those 5-10 ns in that mode, by luck. Not proven on the bridge; it fits, and it was a real fault either way.

**The change** (`ide_table.pio`, still 32 instructions):
- **R0-R2** no longer latch an answer at the strobe. While DIOR# is low they loop (`hold`, 6 cycles): re-read the entry, put it on DD, and let go as soon as the address isn't theirs. A register read is driven about 30 ns after the strobe. Status changes under a long strobe, which also covers the other theory for PIO 3 (the bridge holding one Status strobe until the drive is ready, then moving to the data register under it).
- **D**, once it has taken a read for a register's, keeps looking at the address (every 8 cycles, first with IORDY still low) and answers with the next word if it becomes the data register's, 25 ns before it releases IORDY. It covers an address up to 50 ns late for a host that waits, any lateness for one with a long strobe.
- **D restarted mid-read** (`dReset`, before each data-in block, which is just when the host is polling Status) took the read as new and pulled IORDY low a second time; it now waits that read out. With the bridge's IORDY taking a microsecond to rise, each stray pulse stretched a poll.
- **WD's FIFO is emptied before each data-out block:** words taken outside a block (one cut short by a reset, the host still writing) came first in the next. It fits the wrong byte 0 after slice 4's retry, though not the stall before it.
- **The bisect lines are gone:** DD0-7, D0h and C1's word to a dummy are back, as f769cb3 describes them. Its bisect step 2 is in that commit if it's wanted again.

**The costs, to check on hardware:**
- DD is released up to 35 ns after DIOR# rises (one pass of `hold` and an instruction), against t6z's 30 ns; typically 15-20. A PIO 3 host's next strobe is 70 ns away.
- D notices DIOR# rising up to 45 ns late after a register read. Inside every mode's recovery time but PIO 4's (25 ns): another reason PIO 4 stays unoffered here.
- An R that sees its address glitch away mid-read lets go and takes about 45 ns to drive again. The address lines haven't shown such glitches, but DIOR# has (0.17.1).
- On its way from `sample` to `spin`, an R passes `mov pindirs, null`. Nothing of its own is driven then; it would cut short a data word D was still holding for an address that had already moved to a register.

**Tests:** `test_a_data_read_whose_address_settles_after_the_strobe` (5-40 ns late; PIO 0, 2, 3 and the bridge's timing), `test_status_and_then_the_data_register_within_one_strobe`, `test_status_changes_under_a_long_strobe`. 496 simulator tests and the native tests pass; both drive builds build, core 1's code all in RAM.

**To run on hardware, in this order:**
1. Restart the Mac (the JMicron adapter's decline, *Open*), then the JMicron adapter in table mode: slices 3 and 4. This says whether slice 4's failure was the adapter, the old read side, or is still there.
2. The Innostor bridge with `mode = pio2`: the whole disk and the random writes again (no regression).
3. The bridge with `mode = auto` (PIO 3). If it still reads before DRQ, the cause is neither theory, and it needs a capture finer than core 0's 690 ns.

## 13 Nov 2026, on the rig: one pin read per answer; the bridge in PIO 3

Run on the Mac (restarted first) with the PCB, `pcb_drive`, console `P t` and the test disk (`T`).

**The JMicron adapter, slice 4, as 03bdd00 left it:** a write still stopped about 12 ms in (several sectors into the command), with the same `DONE ... 30 80`. A bench trigger froze the write log at the first SRST during a data-out block and took a snapshot of the observer's ring (the reads, with their address and DD), and showed:
- the adapter's SRST (Device Control 04h, then 00h) was real, not a data write mis-decoded; it kept writing data words after it;
- just before it, its Status read after a sector returned **D0h**, then in a second run (busy word set back to 80h) **F2h**: neither the busy word nor DRQ. Both were **the write's LBA Low** (LBA 100048 = 186D0h, LBA 100082 = 186F2h). So the busy value was never the cause (80h failed the same way), nor the adapter's decline (the Mac had been restarted).

**The cause:** going from the data register (address 10000b) to Status (10111b), the adapter's address can pass through 10011b, LBA Low's, when DA2 settles last. R0-R2's `sample` read the pins twice: once for the claim (DA2, CS0#, CS1#), then `mov y, pins` for the entry. R0 claimed on 10011b, then took y = 10111b (Status) on the second read, and so held Status as its own with `rxfifo[3]`, its LBA Low, driving it over R1 for as long as the address stayed on Status. The two-read `sample` was in f769cb3 as well: this was slice 4's failure there too.

**The fix** (`ide_table.pio`, `ide_table.cpp`): one pin read, `mov y, pins`, both picks the entry (DA0-1) and must equal the address the entry carries, now in its bits 8-15 (DD8-15's value bits, never driven for a register; `publish` and the busy words add it; `at()` in ide_table.cpp). The ISR claims are gone, and `sample` is 4 instructions shorter (block 0 has 4 spare). The host model gained `da2_skew` (DA2 settling that long after DA0-1 and CS) and `test_status_after_data_writes_with_da2_settling_late` reproduced F2h before the fix; 522 simulator tests pass after it. A limit: DA2 settling *after* the strobe (60 ns skew in modes 2-3, outside t1) can still read undriven, as the R that let go clears DD's output enables after the right one drove them.

**Results:**
- The JMicron adapter (PIO 3): 3,000 write-and-read-back cycles with no SRST (before: a failure within 6-111 cycles), then slices 3 and 4 pass. A 32 MB sequential read in 1 MB requests: 1.69 MB/s (a different measure from slice 3's 1.16).
- The Innostor bridge, `mode = pio2`: slices 3 and 4 pass (12 Nov: only the whole disk and random writes had been tried).
- The Innostor bridge, `mode = auto` (PIO 3): **fails, differently.** Reads return whole, well-formed sectors, but **the previous command's**: reading LBA 200, 300, 400 returned 100, 200, 400 (the third right), and a later one junk (`1048576` in the LBA field: D's `pull noblock` repeating X outside a block). The trace: each `CMD 20 READ_SECTORS` is followed 1-2 us later by `DONE 20 d0`, which is the previous READ being closed by this one (`startCommand` abandons a data-in block still unread; recordDone prints it with the new command's busy status), not the new one cut short. The write log agrees: every command's register writes (Device E0h, the count, the LBA, Device, Command 20h) arrived with DRQ still set, so **the bridge never read the block of the command before**. `iordy_early`/`iordy_ignored` are 0, `table_out` 0, `intrq_rises` 1.

**Not known:** why the bridge in PIO 3 skips the data phase, and where it gets the previous sector from (its own buffer, it seems, since the drive's block went unread). 12 Nov's guesses (it doesn't wait for DRQ; it merges a Status read and data under one strobe) predate the outputs following the bus (03bdd00) and this fix, and fit this badly: a bridge reading data early would get junk, not the last sector whole. Ideas to try next:
- **What it reads after a Command:** the firmware on the branch now snapshots the observer's ring 150 reads after every Command write (`g_cmd_snap_pos` in main.cpp); console `R` prints it (`OBSSNAP`, `O @n a=.. dd=..`; `a`'s low 5 bits are the address: 17h Status, 0Eh Alternate Status, 10h data). Flashed, not yet run: one read at PIO 3, then `R`. If there are no data reads at all, the bridge gave up after its Status polls: look at the Status values it saw (BSY, DRQ, ERR, DF), and at INTRQ (it waited for INTRQ at PIO 2, 12 Nov).
- **Its INTRQ:** `intrq_rises=1` over several reads: in PIO 3 it may wait for INTRQ and never see it. A READ's interrupt comes at the block's DRQ, raised by core 1 (`irq_events` in runTableT).
- **Compare with a real drive:** the listen build (`bb48`) recording the CF card through the bridge at PIO 3 (12 Nov did this at power-up only).

**To pick it up (on any host):** flash `pcb_drive` from this commit; console `T` (the test disk) and `P t` (the table), both until a power cycle (the board loses them when the host it's powered from restarts: on 13 Nov a Mac restart put it back on `ICL386MB.IMG`, and macOS mounted it and wrote to it before it was unmounted); `HDDSYNTH.INI` has `mode = auto` (PIO 3; `P 0`, `E mode = pio2`, `P t` for PIO 2). Replug the bridge after every flash or restart. Then one raw read and console `R`. `host/bench/catch_write_fail.py` (bench, macOS paths; `PORT` and the device are at its top) runs slice-4-like writes and sends `R` and `s` at the first `DONE ... 30 80`; it writes LBAs 100000-100287 and leaves them changed, so restart the drive (or write the generated sectors back) before slices 3-4.

## After the rig session: a review at the desk (no hardware)

Read from the note above and the code at d4b9088, on the Windows PC, with no board or adapter. Nothing here is measured.

**The JMicron fault and its fix.** The two-read `sample` was the cause, and it predates 03bdd00: that commit's late-address rework neither caused nor cured slice 4's failure. The one-read fix is sound, and the address in the entry's bits 8-15 costs nothing (a register never drives DD8-15). 03bdd00's theory for the bridge in PIO 3 (the address settling after the strobe) is not its fault: the read side now follows the bus and PIO 3 still fails.

**Three doubts about the PIO 3 evidence:**

1. **"The bridge never read the block" isn't shown yet.** The write log's DRQ flag is the device's own state, which clears only once core 1 sees D's FIFO down to the spare word (`dataInDone`). A block the drive counts one word short looks the same in that log as one never touched. Console `R` already prints what tells them apart: `TABLETEST blocks=... [... fetched= end_level= ... why=3]`, written by `stopDataIn` at each stop. While the stream is still running, `fetched - end_level` is the number of words D gave the host: 0 (and `fetched=8 end_level=8`) is a block never read; 255 or so is a block read but miscounted. The note quotes neither.
2. **`intrq_rises=1` says nothing in table mode.** `s` prints `ide_bus::intrqRises()`, which counts in `ide_bus`'s `setIntrq`; the table raises INTRQ through `ide_table::setIntrq` (forced SETs on C), which counts nothing. So the idea that the bridge waits for an INTRQ that never comes has no evidence for or against it.
3. **An abandoned block leaves its words in D's FIFO.** `stopDataIn` aborts the stream, but only `startDataIn` (`dReset`) empties the FIFO, and that comes after the fetch. A host that reads data between a Command write and the next block's DRQ gets up to 8 words of the block before, then the last of them repeated. That is 8 words of the previous sector, not the whole of it, so it doesn't explain what the Mac saw; but a bridge that reads early should get junk, not old data that looks right.

**What would explain a whole previous sector.** The drive can only have given sector N whole between N's DRQ and the next Command write. If the block records show it did (`fetched - end_level` near 256, `why=3`), the bridge reads the data, the drive's count is short, and the fault is ours: look at the block's first and last reads in `OBSSNAP`. If they show 0, the data the host got for the next command came from the bridge itself, and the question is what the bridge was waiting for: INTRQ (count it), or a Status value (the snapshot's `a=17`/`a=0e` reads).

**Changes made for the next rig session** (built for `pcb_drive` and `bb48_drive`, core 1's code all in RAM; not run on hardware):
- **D's FIFO is emptied when a block is cut short** (`stopDataIn`: the stream was still running, or more than the spare word is left). A data read before the next block's DRQ now gets junk, never the block before. If the bridge in PIO 3 now returns junk where it returned the previous sector, those sectors came from the drive after all.
- **`intrq_rises` in `s` counts the table's own** (`ide_table::intrqRises()`: each time core 1 drives INTRQ high) while the table runs. One per READ SECTORS block is right.
- **The block records say what the host took:** `R`'s `TABLETEST blocks=` entries gain `taken=` (`fetched - end_level`). It is exact for a block cut short while the stream ran (`why=3` or `2`), and 256 of a 256-word block; a whole block reads 257 - 1 = 256.

**To run on the Mac** (as *To pick it up* above: `T`, `P t`, `mode = auto`, the bridge replugged): `s` for `intrq_rises` before; three raw reads of different LBAs; `R` and `s`. Wanted: what each read returned; `taken=` and `why=` for each block; `intrq_rises` against the number of reads; and the `OBSSNAP` after the last Command write.

## 13 Nov 2026, late: the review's run, and why PIO 3 reads go early

On the Mac with the bridge at PIO 3 (`mode = auto`), `T`, `P t`, firmware 43d70ee plus bench logging: core 1's events against the observer's ring position (`elog`, printed by `R` as `EV1 @pos k=`: 1 Command taken, 2 INTRQ set, 3 interrupt acknowledged, 4 fetch result, 5 block started), the snapshot moved to 100 reads before the Command and 60 after, and the strobe build (`PLATFORMIO_BUILD_FLAGS=-DHDDPRO_OBSERVER_STROBE=1`) with its pass count widened to 10 bits (15.4 us; the sequence number down to 1 bit, bench only). The bridge re-detected the drive by itself after each flash, with no replug.

**The review's three doubts, answered:**
1. **The bridge doesn't read the block:** a failing read's record is `fetched=8 end_level=8 taken=0 why=3` (11 of 16 records after detection were like it); good ones `taken=256 why=1`.
2. **INTRQ:** counted now (`intrq_rises` 118 to 123 over three reads). The event log shows INTRQ low (driven) from the Command to the block, and raised with the block (`k=5` then `k=2 v=3`): the bridge isn't going on INTRQ.
3. **With D's FIFO emptied** (43d70ee), a failing read now returns junk (`1048576`/`1048592` in the LBA field: X repeated), no longer the previous sector: those sectors came from the drive's leftover FIFO words and stream.

**What a failing read looks like** (LBA 13041; reads, then strobe lengths):
- Status polls before the Command: 50h, **1,605 ns** each (D's IORDY, then the bridge's slow rise);
- the Command (`EV1 k=1`), then **one** Status read, D0h on the cable, **75 ns**;
- then the data register at once: the first read 1,755 ns (D took it for a register's, the address moving with the strobe), the rest 105 ns, all junk; the fetch arrives about 300 reads later.

**A good read** (LBA 13000): after the Command, Status D0h read again and again, every one **75 ns**, about 100 times and more, until DRQ. So the bridge does wait for BSY, and reads Status with 75 ns strobes that don't wait for IORDY (before the Command it waits). The observer saw D0h in the failing case too: the bridge latched something else. **Theory:** its strobe is 75 ns and it samples IORDY early; D pulled IORDY low 30-35 ns after DIOR# fell (it waited 2 cycles for the address synchroniser), at tA's limit, so the bridge sometimes misses it and latches DD near when R1 starts driving (25-45 ns): a stale bus, BSY clear, and it goes for the data. A CF card holds that read 2.2 us (12 Nov): it pulls IORDY in time.

**Tried, not yet seen to work:** D watches the address while DIOR# is high (`top`: a register's address goes to `regwatch`, which pulls IORDY low the instruction after DIOR# falls, about 10-20 ns); a data read looks at the address again after the strobe, so a host moving the address with the strobe still gets a register's IORDY. 31 of 32 instructions. `test_iordy_falls_within_20_ns_of_a_register_read` (modes 0-4) fails on the old D and passes on the new; 522 simulator tests pass. **On the rig, unknown:** the upload failed part-way (picotool, after 12 s) and the board left USB altogether (no serial port, no bootloader drive) while still serving the bridge, so which firmware ran isn't known; it read 20 of 30 sectors wrong (about 1 in 4 before). Morning: replug the board's USB, check `h`/`R` (`ELOG` present, the new D: `R`'s `EV1`), set `T` and `P t` again if it restarted, and repeat 30 reads at PIO 3 (`host/bench/snap_one.py LBA out.log` reads one sector and saves `R`; `host/bench/show_snap.py out.log` lists the reads with strobe lengths and the events). If it's worse, the new D is wrong on hardware: compare the first Status read's strobe after a Command (it should now be about 1.6 us, as before a Command).


## 14 Nov 2026, morning: Status on DD sooner; the bridge passes in PIO 3

**Last night's D didn't help.** Flashed again (the board was back on USB): 23 of 30 reads wrong at PIO 3. The first Status read after a Command was still 75 ns: the bridge doesn't wait for IORDY there at all, so when IORDY falls doesn't matter.

**Ruled out: the busy word arriving late.** C pushes about 9 cycles after DIOW# falls, and C1 and Ka put the busy word in R1's Status entry about 25 cycles after that (`Q`'s new `busy latency`, polled from core 0): BSY is in the table about 170 ns after the Command write starts. Console `G`'s edge log puts the bridge's first Status read about 8.6 us after the Command write.

**The cause: DD floating at the bridge's sample.** Each poll is 75 ns, and R1 drove DD 26-36 ns after DIOR# fell (its spin looked at DIOR# every 3 cycles, then 5 instructions to `out pindirs`). Until then DD holds what was last on it. For every poll but the first, that's D0h from the poll before. For the first, it's the command byte (20h, written by the bridge itself, the write log shows), decaying toward the host's DD7 pull-down: BSY clear. A bridge latching at about 30-40 ns gets the first poll wrong some of the time, takes BSY as clear, and reads the data before DRQ. That fits 1 in 4 to 3 in 4 bad, the observer's D0h at the end of the strobe, and why PIO 2 (longer strobes, which wait) passes.

**The fix: R keeps its entry on DD's output latch while it waits** (`spin`: the address, then the entry onto the latch, undriven, then DIOR#: 5 cycles a pass), so DIOR# falling costs one `out pindirs`: Status is on DD 11-30 ns after the strobe in the simulator (26-45 before), for a register whose address was already on the bus. `hold` re-drives every pass and checks the address before it re-latches, so a read that becomes the data register's (the bridge's Status to Data with the strobe) is let go before D drives. Even so, R can let go as late as cycle 9 and D drove on cycle 8, so D now drives twice (cycles 8 and 10). D is back to 43d70ee's (13 instructions; last night's `regwatch` is gone), which makes room: 17 + 14 = 31 of 32. `test_status_after_a_command_is_on_dd_within_30_ns_of_its_strobe` (Innostor PIO 4 timing, modes 0-3; PIO 4's 25 ns recovery is shorter than R's release) fails on the old R and passes; 526 simulator tests pass. The host model now counts a register's DD0-7 as driven for `data_setup_ns`.

**On the rig (the Innostor bridge, PIO 3, `T`, `P t`):** 60 random reads, none wrong; 1,500 write and read-back cycles of 1-32 sectors, none wrong; **slice 3 (4 passed) and slice 4 (3 passed; 1,000 cycles, 16,236 sectors, 1.90 MB/s)**. Then the JMicron adapter in PIO 3: slice 3 (4 passed), slice 4 (3 passed; 1,000 cycles, 0.58 MB/s); `-m speed`: reads 1.69 MB/s, 80.0 IOPS, writes 1.77 MB/s, above the data lane's 1.53 and 1.68 (`docs/speed-log.md`), and the core 1 layout recorded.

**Bench:** `Q` hung core 0 (`busyTest`'s wait used core 0's cycle counter, which only `G` turned on): fixed. Careful with slice 3 after hand-written tests: it checks the whole disk, so writes to the test disk (LBA 100000 on) must be put back first (`gen_disk.sectors`), or restart with `T` twice.

## Open

- **The Innostor bridge in PIO 3:** passes slices 3 and 4 since R drives from its latch (*14 Nov, morning*). The JMicron adapter passes on it too.
- **Slice 4 through the JMicron adapter:** fixed (*13 Nov, on the rig*: R0 answered Status with LBA Low).
- **Interrupt acknowledgement:** a Status read now acknowledges an interrupt only if the host saw the device not busy (`statusReadSince`): a read whose strobe began before the raise and ended after it lands after the raise in the ring. Right in principle; slice 4's failure was something else (*13 Nov, on the rig*).
- **Console `V` (rig re-detection):** in table mode the drive stops answering, but the JMicron adapter doesn't reset, so slices 1-2 time out.
- **Bench tools in the tree, to come out before merging:** `host/bench/`, the write log's SRST trigger and the observer snapshots after it and after each Command write (`g_wlog_stop`, `g_obs_snap`, `g_cmd_snap_pos`, `snapObserver`), console `G` (core 0 logs the control lines' edges from the next read command, `edgeLog`; in the listen build too), the write log printed by `R` (`WLOG`), `Z` re-arming the observer's register print, and that print's limits and ring positions (`HDDPRO_OBS_REGS`, bus_observer.cpp). The register print's `OBSDATA` numbering is wrong after a re-arm.
- **Console `E`/`C` in table mode:** "the card's PIO block couldn't be borrowed" (the write side holds the diagnostics' block). `P 0`, `E`, `P t` meanwhile.
- **The core 1 layout** was recorded on 14 Nov with the bench hooks in: record it again once they come out (and benchmark).
