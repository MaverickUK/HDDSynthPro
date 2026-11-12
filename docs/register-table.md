# The register table: every read answered without core 1

**Status (13 Nov 2026): the read side reworked at the desk so its outputs follow the bus while DIOR# is low (*13 Nov*, below): simulated, built, not yet on hardware. Before it (12 Nov): branch `register-table`, console `P t`. Through the JMicron USB adapter it passes rig slices 3 and 4 (the whole disk at 1.16 MB/s, 1,000 write-and-read-back cycles); slices 1-2 need console `V` to work in this mode (*Open*). Through the Innostor bridge, not yet (*Open*). The bench hooks (console `Q`, `R`, `Z`, extra `s` fields, `HDDPRO_FETCH_DELAY_US`) come out before it merges.**

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

## Open

- **The Innostor bridge in PIO 3:** above, and *13 Nov*: a likely cause found in simulation and removed; to try on the bridge. `mode = pio2` works meanwhile, at no cost through USB.
- **Slice 4 through the JMicron adapter fails on today's read side:** a write stops after about 11 ms (the adapter soft-resets the drive, status 80h), and after its retry byte 0 of the first sector reads back wrong. Slice 3 passes (1.07 MB/s, against 1.16 on the morning's programs). Bisect so far: not DD0-7 (16 lines fail the same). Step 2 (the morning's busy write, C's word straight into Status, and 80h, with the new read programs) never reached a write: four times the adapter's I/O hung in the kernel (the last ended in I/O errors after 14 minutes) (the test process in state `U`, holding the board's serial port until a replug) after an IDLE IMMEDIATE the drive answered normally. Whether that was the firmware or the adapter declining after a day of resets (only a restart clears that) is open: restart the Mac and run step 2 again first. The `BISECT` lines of f769cb3 mark what step 2 reverted (taken out again on 13 Nov, with the read side reworked: run slice 4 on that first).
- **Interrupt acknowledgement:** a Status read now acknowledges an interrupt only if the host saw the device not busy (`statusReadSince`): a read whose strobe began before the raise and ended after it lands after the raise in the ring. Right in principle; it didn't fix slice 4.
- **Console `V` (rig re-detection):** in table mode the drive stops answering, but the JMicron adapter doesn't reset, so slices 1-2 time out.
- **Bench tools in the tree, to come out before merging:** console `G` (core 0 logs the control lines' edges from the next read command, `edgeLog`; in the listen build too), the write log printed by `R` (`WLOG`), `Z` re-arming the observer's register print, and that print's limits and ring positions (`HDDPRO_OBS_REGS`, bus_observer.cpp). The register print's `OBSDATA` numbering is wrong after a re-arm.
- **CI's core 1 layout check fails on this branch** until reads are benchmarked and the layout recorded (below).
- **Console `E`/`C` in table mode:** "the card's PIO block couldn't be borrowed" (the write side holds the diagnostics' block). `P 0`, `E`, `P t` meanwhile.
- **The core 1 layout** of `main.cpp`'s functions moved (`loop1` grew): benchmark reads before recording it.
