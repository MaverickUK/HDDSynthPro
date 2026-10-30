# Hosts that don't wait for IORDY: a design

**Status: a design, not built. A first step, early answers (`early_answers`, 0.23.x on branch `no-iordy`), didn't get a Pentium's PIIX through detection; next, measure its timing, then build the table.** Build it if a retro PC shows `iordy_ignored` above 0 ([retro-test.md](retro-test.md)). It's the firmware half of [pcb-bom.md](pcb-bom.md)'s "register reads answered from a table" (section 4, and open question 3).

## The problem

Today every register read is answered by core 1. `ide_bus.pio` pulls IORDY low as soon as a chip select goes active, sends the access to core 1, and releases IORDY once core 1 has put the value in its FIFO. That takes up to about 1,100 ns, and IORDY makes the host wait for it.

Many early controllers don't wait. Plain ISA IDE cards often leave IORDY unconnected, and ATA-1 made it optional. Such a host reads DD at the end of its strobe whatever IORDY says:

| | PIO mode 0 (ATA) | A typical ISA card (DIOR# = ISA IOR#) |
|---|---|---|
| Chip select valid before DIOR# falls (t1) | 70 ns | about 100 ns |
| DIOR# low (t2) | 165 ns at least | about 375-500 ns (8 MHz, one wait state) |
| Data needed by, from chip select | about **215 ns** | about 400-550 ns |

Core 1's 1,100 ns misses either deadline, so the host reads whatever was on DD (usually the pulls: `FF`) or a half-driven value. `iordy_check.pio` (0.16.5) already detects this: `iordy_ignored` counts the reads the host ended while IORDY was still low.

## Seen on a Pentium 200 (30 Oct 2026)

An Intel PIIX-era Pentium 200 (Phoenix BIOS) runs its IDE controller in "compatible" timing until the BIOS has identified the drive, and compatible timing doesn't sample IORDY. With 0.22.0 the BIOS never got that far:
- **Drive type Auto:** a soft reset, status reads, then IDENTIFY five times, about a second apart. Each failed: `iordy_ignored` counted the status and LBA reads the host ended before the drive answered, and the bus observer found 31 of IDENTIFY's 256 words wrong on the cable (`bus_identify_bad`). Then "Insert bootable media".
- **Drive type User (7769/16/63, LBA, PIO 3, multiple sectors off):** the BIOS selects the master (DEVICE = A0h), reads the status four times, selects it again and reads DEVICE back. The read-back ended before core 1 answered (the bus held `9004`), so the BIOS decided there was no drive, and stopped. So a manual PIO mode doesn't get round it: this BIOS checks the drive in compatible timing first.
- Most data words were in time (225 of 256), but register reads weren't. Core 1 learns which register is read only once the strobe has started, and takes 150-250 ns, sometimes µs straight after a command or a reset.

## First step: early answers (planned)

Before the full table design below, a smaller change, which may be enough for hosts like the Pentium whose strobe is a little short rather than very short:
- **The bus program asks core 1 at the chip select, not at the strobe.** On `arm` it pushes an answer request: the address bits, with bits 21-31 zero, so both strobe bits read as asserted, which no real access shows. Core 1 answers into the TX FIFO, and on DIOR# the program has the value at once: t1 (70 ns or more) earlier than now.
- **A write discards the answer;** an address or chip select that changes while armed discards it and asks again (Y holds the address asked about).
- **Reads still report themselves to core 1,** in a sample taken during the strobe, for their side effects (STATUS clears the interrupt; data words advance the block).
- **Data words come ahead for free:** after each access the program re-arms and asks again, so the next data word is usually in the FIFO before the host's next strobe.
- **IORDY works as now** for hosts that wait, which just see shorter holds. About 30 instructions, so no room for `iordy_watchdog`: a separate variant chosen by a setting, until it has been through the USB adapter's tests.

### What early answers did on the Pentium (0.23.0-0.23.1)

Not enough. With 0.23.0 the BIOS got as far as a NOP and IDENTIFY, but data reads took 1.3-2 µs and 28 words were wrong: core 1 answered each request only after a pass of its main loop. 0.23.1 added a fast lane and 50 µs of bus-only after each access. Then the BIOS stopped at the status and DEVICE read-back checks again, because the chip select gives no warning:
- **`iordy_early=4`:** on 4 of the 5 failed reads, IORDY wasn't yet low when DIOR# fell. The PIIX's compatible timing asserts the chip select and the strobe almost together (t1 of a cycle or two, against ATA's 70 ns), so an answer asked for at the chip select is no earlier than one asked for at the strobe.
- **The slowest failed read was answered 540 ns after DIOR# fell;** the host had ended its strobe long before. Two of the five failed reads were answered within 400 ns.

**So for this host the answer must be on DD within the strobe, with no warning,** in well under 300 ns: the table design below, not core 1. **Before building it, measure the PIIX's compatible timing** (t1, the strobe width, the cycle) with a PIO probe on the drive. `early_answers` stays as an experiment (off by default); it may still help hosts with a real t1.

## The idea: answers ready before the host asks

Core 1 keeps doing all the work, but **ahead of time**. It keeps a register image in RAM, and the hardware answers from it without core 1:

1. **A register table**: 32 words, one per DA2-0/CS0#/CS1# combination (the 5 consecutive pins `ide_bus` already decodes with `s_reg_for`). Core 1 rewrites the entries whenever the device's state changes: after a write to a register, at each step of a command, when BSY or DRQ changes. Reads of unused addresses hold `FF`.
2. **A lookup in PIO and DMA, no CPU.** When a chip select goes active, the PIO program builds the table entry's address from the 5 address pins and pushes it (the ISR shifting left, so the address bits land below Y's):
   ```
   mov isr, y          ; Y = the table's address >> 7 (set once by the C side)
   in pins, 5          ; DA2-0, CS0#, CS1#: the entry
   in null, 2          ; x 4 bytes
   push                ; = table + entry * 4
   ```
   DMA channel A copies that address into channel B's READ_ADDR trigger register; channel B copies one word from the table into the PIO's TX FIFO, and the program puts it on DD when DIOR# falls. The usual PIO and DMA lookup trick: about 15-20 cycles at 200 MHz (under 100 ns) from chip select to the word in the FIFO, inside the 215 ns.
3. **The data register streams.** The data register's entry can't be a fixed word: each read must return the next word of the sector. A second state machine answers it, fed by a DMA channel from the sector buffer, as `ide_dma_read` is in multiword DMA. Core 1 starts that channel **before** it sets DRQ in the table, so the first word is there before the host can see DRQ. Each state machine checks the address and only one drives DD.
4. **Writes** still go to core 1 as samples. The data register's writes go to the sector buffer by DMA (as `ide_dma_write`), so a 256-word burst can't overrun core 1 now that IORDY can't hold the host.

The PIO program replaces `ide_bus.pio` in its block when the mode is chosen: a setting, `iordy = off` in `HDDSYNTH.INI` (`auto` could choose it later, from `iordy_ignored` after detection). IORDY stays released throughout. That keeps today's path untouched for hosts that wait.

## The hard part: BSY after a command

A host may read STATUS 400 ns after writing a command, and must see BSY (or DRQ). With the table, BSY appears only when core 1 has rewritten the STATUS entry. Core 1 answers today's accesses within about 1 µs, too slow to rely on. Two answers, in order of preference:

- **The PIO sets BSY itself**: on a write to the command register, it switches the STATUS and ALTSTATUS entries to a second, fixed table entry that reads `80` (BSY) (Y holds two table addresses; the program picks one), until core 1 has updated the table and tells it to switch back (an `irq` or a word in its FIFO). Then a STATUS read after any command always sees BSY, however late core 1 is.
- Or core 1 polls only the command register, in a tight RAM loop, and updates the entry within 400 ns. Simpler, but it depends on core 1's timing again, which is what this design removes.

The same applies to SRST (a write to DEVICE CONTROL with bit 2 set): BSY within 400 ns.

## IOCS16#: a hardware limit of the BB48

Separate from IORDY, and found the same evening: a 16-bit ISA IDE card may need the drive to assert **IOCS16#** (IDE pin 32) when the data register is addressed, or the ISA bus splits every data read into two 8-bit reads. Most later cards decode it themselves. The symptom: on the raw trace (`t`), every data read is followed by a read of the error register (1F1h), and the BIOS gets a garbled IDENTIFY. Seen on an ICL 386's ISA card (30 Oct 2026). Since 0.20.0 the BB48 drives it from `iocs16.pio` on GPIO 2 (one wire from IDE pin 32 to EXT1-6; only when DMA isn't offered); the PCB rev 0.3 doesn't have it (rev 0.4 adds it, [pcb-rev04.md](pcb-rev04.md)).

## Testing it

- **In the simulator first** ([pcb-bom.md](pcb-bom.md)'s open question 3): `pio_sim` with a DMA model for the two-channel lookup, and an `ide_host_model` that never waits for IORDY, at PIO mode 0's minimum timings. Status after a command, and after SRST, must show BSY within 400 ns.
- **On the rig**: the USB adapter waits for IORDY, so a rig test can't prove a host that doesn't. `iordy = off` on the rig shows the table path works (and `iordy_check` would show the adapter never needed to wait); the proof is the retro PC.

## Cost

About 3-4 days: the PIO program and its DMA chain (1 day), core 1 keeping the table and the BSY handover (1 day), the simulator's DMA model and a no-IORDY host model (1 day), the rig and the retro PC (an evening or two). The programs fit: `ide_bus`'s block is free when the no-IORDY program replaces it, and the RP2350 has DMA channels to spare (this needs 3-4).
