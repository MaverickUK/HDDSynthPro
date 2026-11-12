# The register table: every read answered without core 1

**Status (12 Nov 2026): branch `register-table`, console `P t`; works through the JMicron USB adapter (the whole 64 MB test disk read back, writes read back), not yet through the Innostor bridge, whose sector reads come back one command behind (*Open*). The bench hooks (console `Q`, `R`, `Z`, extra `s` fields, `HDDPRO_FETCH_DELAY_US`) come out before it merges.**

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
- **INTRQ** is C's: low as a Command write starts, raised by core 1.

## Open

- **The Innostor bridge, after the first sector:** it reads data straight after its next Command write, and gets the previous block's leftover words; the drive streams the right sector each time. Not BSY timing (BSY by one DMA hop, about 70 ns), not INTRQ, not the data being ready too soon (results held 200 µs). The captures (core 1 sampling, starting up to 2 µs after the command) may miss its first Status read: next, a PIO logic-analyser capture.
- **Console `E`/`C` with the test disk:** the card borrows the diagnostics' PIO block, which the write side now holds.
- **The core 1 layout** of `main.cpp`'s functions moved (`loop1` grew): benchmark reads before recording it.
