# Retro PC test: the BB48 rig in a real PC

The first time the drive sits on a real IDE controller rather than the USB adapter. In order: each step either passes or says what to note before the next. Plan item 6 in [plan.md](plan.md) (a 486-class host); firmware **0.17.1 or later**.

Two passes, ideally on separate evenings:
1. **The test disk, PIO mode 0.** Every sector read is checked on the cable by the bus observer, so this pass says whether the controller works with the drive at all, and whether it waits for IORDY.
2. **The scratch image, `auto`.** FDISK, FORMAT, copying files and benchmarks, on `HDDBENCH.IMG`. The SD card's `HDDSYNTH.IMG` isn't touched.

## Session, 3 Nov 2026: the data lane and two drives (0.29.0-2d.4)

Two questions for the ICL 386 and the Evesham. The answers decide what comes next:
- **Should the data lane become the default?** On the USB adapter, `early_answers = on` (today's default) holds about 50,000 reads a speed run past ATA's 1.25 µs. The lane ([data-lane.md](data-lane.md), chosen with `early_answers = off`) answers data reads without core 1: faster, and in time for hosts that ignore IORDY. But it has no IORDY watchdog, so ISA cards can't use it, and register reads go to core 1 at the strobe, as `ide_bus` does.
- **Does a BIOS find a second drive?** `device1` ([two-drives.md](two-drives.md)): an image or a folder on the card, served read-only. The BB48 has no DASP#, which some BIOSes use to find device 1.

### On the Mac, before you start

- [ ] `git checkout two-drives && git pull` (at b64d613 or later). The host tools: `python3 -m venv host/.venv && host/.venv/bin/pip install -r host/requirements.txt`, if the venv isn't there.
- [ ] The BB48 already runs **0.29.0-2d.4**. Check with console `h` (`READY HDDSynthPro 0.29.0-2d.4 slice3`). To flash it again: `~/.platformio-venv/bin/pio run -e bb48_drive -t upload`.
- [ ] On the card, in its root folder: make a folder **`GAMES`** with a few files (a large one for `COPY` and `FC`, a subfolder, a long file name), from the Mac's Finder. The firmware never creates or changes files there. The card holds `ICL386MB.IMG` (MS-DOS 5, which both PCs have booted) and `HDDSYNTH.INI`.
- [ ] The card's settings as left: `device0 = ICL386MB.IMG`, `early_answers = on`, `isa = off`, `device1 = none`. Console `S` shows them.
- [ ] The rest of the set-up is as below: [What to bring](#what-to-bring) and [the power-order rule](#2-connecting-the-power-order-rule). Log each step with `python3 scripts/retro_log.py <pc>-<step>`, e.g. `icl386-1-early` (it saves every line to `host/runs/`).

To change a setting: console `E`, then `key = value` and Enter, then `!` (the drive restarts and reads the file again). Then reboot the PC, so its BIOS detects the drive afresh.

### What to note at each step

- **The BIOS:** does it detect `HDDSYNTH PROTO` (and in step 3 `HDDSYNTH PRO D1`, its slave or device 1)? Photos.
- **It boots** MS-DOS 5 from C:.
- **Console `s`:**
  - `bus=` (which program served the bus);
  - `iordy_ignored` (**0**, or the host read without waiting);
  - `iordy_max_ns` and `iordy_max_reg`;
  - `iordy_slow`;
  - `bus_identify_bad` (0);
  - `commands`;
  - with the lane, `lane_blocks`.
- **The log's `!!` lines:** `NOIORDY`, `BUSERR`, `STUCK`, `SLOWREAD`, resets.
- **Speed:** `HDDBENCH` from the benchmark image if it's on C: (or time a `COPY` of a large file to NUL).

### Step 1: as it is (early answers)

| ICL 386 (ISA card, IOCS16# wire on) | Evesham |
|---|---|
| Settings as left. Boot, a benchmark or `COPY`, then `s` | The same |

The reference for steps 2 and 3, on the firmware the steps use.

### Step 2: the other bus program

| ICL 386 | Evesham |
|---|---|
| `E isa = on`, `!`. `s` shows `bus=ide_bus`: the old program with the IORDY watchdog. The lane isn't for ISA cards (no watchdog: a memory or refresh cycle aliasing the drive's address would stall the bus). This checks the old program on this firmware | `E early_answers = off`, `!`. `s` shows **`bus=lane`**. The main test: does the BIOS detect the drive, does it boot, is it faster, is `iordy_ignored` still 0? On 31 Oct this PC had 247 reads that didn't wait for IORDY, which early answers covered. The lane covers data reads, not register reads |

- **The Evesham boots and runs cleanly on the lane:** the lane becomes the default, with `isa = on` for ISA machines.
- **It fails** (no detection, a hang, `iordy_ignored` above 0 with errors): note which registers (`NOIORDY` lines), then `E early_answers = on`, `!`. Early answers stays the default.

### Step 3: two drives

On each PC, with step 2's bus program (or early answers, if step 2 failed):

- [ ] `E device1 = GAMES`, `!`. `S` shows `CONFIG device1=GAMES kind=folder sectors=...` and a `CONFIG folder entries=...` line.
- [ ] Reboot the PC. In the BIOS set-up, auto-detect the primary slave (or let it find it). Does it show `HDDSYNTH PRO D1`? Note its geometry. If it doesn't find it, note that, and try setting the slave by hand to the geometry `S` gives.
- [ ] In DOS: `DIR D:`, `DIR D:\SUBFOLDER`. `COPY D:\BIGFILE C:\` then `FC /B D:\BIGFILE C:\BIGFILE` (no differences). Then delete the copy from C:.
- [ ] A write to D: (`COPY C:\COMMAND.COM D:\`): it should fail cleanly ("write fault", abort or fail), and leave the PC working. Nothing on the card changes.
- [ ] `s`: the console's `CMD` lines for device 1 end in `d1`.
- [ ] Optional: `E device1 = HDDBENCH.IMG`, `!`, a second image as D: (FDISK and FORMAT it, then copy files to it). `HDDBENCH.IMG` is scratch.

### Afterwards

- [ ] `E device1 = none`, `E early_answers = on`, `E isa = off`, `!`: the card as it was. (Or edit `HDDSYNTH.INI` on the Mac: `device0 = ICL386MB.IMG`, `device1 = none`.)
- [ ] The logs (`host/runs/retro-*.log`), the `s` lines and the photos: the results go into [data-lane.md](data-lane.md) and [two-drives.md](two-drives.md), and `CHANGELOG.md`.

### Results, 3 Nov 2026 evening

Both PCs ran all three steps (0.29.0-2d.4), each step benchmarked with `DOSBENCH.IMG` (`tools/bench-image`) as device 0, so the numbers compare: HDDBENCH's sequential read and write, and its random read time. Logs: `host/runs/retro-20261103-*`.

| | ICL 386 (ISA card) | Evesham (Athlon XP, PIO 3) |
|---|---|---|
| **1. Early answers** | PASS: 1.65 MB/s read, 1.42 ms random, 0.54 MB/s write | PASS: 1.78 MB/s, 1.09 ms, 1.24 MB/s |
| **2. Other program** | `isa = on`: PASS, 1.49 MB/s, 1.64 ms, 0.74 MB/s | **Data lane: PASS, 2.18 MB/s, 0.98 ms, 1.98 MB/s** |
| **3. Two drives** | `device1 = HDDBENCH.IMG` (`isa = on`): found by the BIOS; FDISK, FORMAT and `FC /B` on D: fine; 1,489 device 1 commands, none failed | The same image, on the lane: found; `FC /B` identical; 345 device 1 commands, none failed |
| **3. A folder** (`device1 = GAMES`) | The board doesn't start (below) | Not tried |

- **The answers.** The lane boots and runs the Evesham cleanly and much faster than early answers: reads +22%, writes +60%, and above the 1.8 MB/s once taken for the SD card's limit. And both BIOSes find a second drive without DASP#.
- **Hosts not waiting for IORDY:** none, on either PC in any step, once the PC was out of reset (the bus floats while a PC powers up).
- **The lane's Status reads run long:** about 1,400 a benchmark were held past ATA's 1,250 ns (mean 1,283 ns, up to 1,540). Data reads never are. Harmless on the Evesham, which waits for IORDY, but over ATA's limit. Early answers' worst there was 1,030 ns.
- **The bus observer checks nothing on the lane** (`bus_sectors=0`): those runs rest on HDDBENCH's own check (0 bad sectors).
- **Early answers on the ICL 386:** IORDY was still low when the card strobed DIOR# with no chip select, until `iordy_timer` hit its limit (655,770 ns, register `-`), about 200 times a benchmark, though it caused no errors; core 1 released IORDY 293,361 times. With `isa = on` the worst read was 1,210 ns and none were slow. The bus observer's false flags on this card (DIOR# with no chip select) were 76 and 116 sectors; HDDBENCH found none bad.
- **The folder drive stops the board.** With `device1 = GAMES` (made by `host/hddproto/test_folder.py`: 12 files, 3.3 MB, a subfolder two deep and a long name), the BB48 never comes up: no USB serial port, at every power-up. It starts at once without the card. `make_folder_img` lays out a copy of the same folder cleanly under ASan, and the image mounts on the Mac with every file right (`LONGFI~1.BIN` too), so the fault is in the board-only path: `sd_card`'s `SdFolderSource` or `chooseDevice1()`. Not debugged yet.
- **A misplugged IDE cable** looks like a dead PC: no picture, RESET# pulsing every few seconds, no commands, and the bus lines at odd levels (both chip selects low). Twice tonight, cured by reseating the cable. Unplugged, the PC starts.
- **Changing `device0` while DOS runs** sends its writes to the new disk: two stray WRITE SECTORS failed out of range (no harm). Switch the PC off, or reset it at once, when C: changes.

## What to bring

- The BB48 rig, with its IDE cable left on the BB48. Only the cable's other end moves, from the USB adapter to the PC.
- A laptop (the MacBook) and a USB cable for the BB48: it powers the BB48 and is the console. The PC's IDE cable carries no power.
- A DOS boot floppy (or a second drive the PC boots from) with `FDISK`, `FORMAT`, `SYS`, and benchmarks: CORETEST, SPEEDSYS, Norton SI or CheckIt, whichever you have. A large file (a few MB) to time a `COPY` with.
- A phone, for photos of the BIOS screens and the benchmark results.

## 1. On the bench, before you go

With the rig on the USB adapter as usual:

- [ ] Console `h`: `READY HDDSynthPro 0.17.1 slice3` (or later).
- [ ] `E device0 = none`, Enter; `E mode = pio0`, Enter; `!`. Then `S`: `CONFIG ... device0=none mode=pio0` and `dma=off`.
- [ ] `K` (creates `HDDBENCH.IMG`, 64 MB of zeros, if it isn't there yet), for pass 2.

`pio0` offers only the slowest mode, so a BIOS that sets its controller from the IDENTIFY block can't choose a timing the drive hasn't been tried at with this controller. `dma` stays off: a 486's ISA or VLB controller has no multiword DMA anyway.

## 2. Connecting: the power-order rule

The BB48 has no buffers. Its pins are only 5 V tolerant while it's powered, and the PC's controller drives the bus whenever the PC is on, so **the BB48 is powered whenever the PC is**:

- [ ] Everything off. Plug the cable into the PC's IDE header, **red stripe to pin 1**.
- [ ] Put the drive **alone on its channel**, as device 0 (the master): it can't share a cable with a real drive (no DASP#/PDIAG# handshake). If the PC boots from its own hard disk, use the secondary channel for the BB48; otherwise boot from floppy.
- [ ] **Power the BB48 first** (the laptop's USB). Start the console logger: `python3 scripts/retro_log.py 486-pass1` (from the repo, with `host/requirements.txt` installed). It saves every line to `host/runs/retro-<date>-<time>-486-pass1.log`, and shows resets, commands other than reads and writes, errors and `NOIORDY`/`BUSERR`/`STUCK`/`SLOWREAD` lines (marked `!!`), plus a summary of `s` every minute and whenever you press Enter. Type console commands as usual (`S`, `p`, `E key = value`); `q` quits. It prints the READY line on start.
- [ ] **Then switch the PC on.**
- [ ] When you're done: **PC off first**, then the BB48's USB. Never unplug the BB48 while the PC is on.

## 3. Pass 1: detection

Watch the console as the PC starts. The BIOS resets the bus (a `RESET` line), then sends `IDENTIFY`.

- [ ] The BIOS detects **`HDDSYNTH PROTO`** (auto-detect, or the IDE setup utility). Photo of the screen.
- [ ] Its geometry: **130 cylinders, 16 heads, 63 sectors** (64 MB). A BIOS without auto-detect: user type (47) with those numbers, landing zone 130, write precompensation none (or 65535).
- [ ] Console `s`, and note:
  - **`iordy_ignored=0`**: the controller waits while the drive holds IORDY. **This is the result that matters most.** If it isn't 0 (with `NOIORDY` lines), the controller reads the registers without waiting, and gets values the drive hasn't put out yet. Many plain ISA IDE cards don't wire IORDY. Then detection may be flaky or garbled, and that's expected: such hosts need the register table designed in [no-iordy.md](no-iordy.md). Note the card's make and stop pass 1 here.
  - `bus_identify` and `bus_identify_bad=0`: the IDENTIFY block was right on the cable.
  - `iordy_max_ns` (under 1,250).
- [ ] Which commands the BIOS sent: the `CMD` lines (e.g. `ec IDENTIFY`, `91 INIT_PARAMS`, `ef SET_FEATURES`, `20 READ_SECTORS`). An unknown one shows as `?` after its code, and gets ABRT: note them all.

## 4. Pass 1: reads on the test disk

The test disk holds a pattern computed from each LBA, with no partitions or file system: DOS sees an unpartitioned disk. That's what lets the observer check every sector read.

- [ ] Boot DOS from floppy. `FDISK`, option 4 (display): no partitions, about 64 MB. Don't create any yet.
- [ ] Benchmarks that only read (CORETEST's transfer rate and seek tests, SPEEDSYS's disk test). Photos of the results.
- [ ] `s`: `bus_sectors` has grown, **`bus_bad=0`** and no `BUSERR` lines; `iordy_ignored=0` still; `iordy_max_ns`. `SLOWREAD` or `STUCK` lines: note them.
- [ ] Optional, writes: `FDISK` a primary partition, reboot, `FORMAT C:` (without `/S`). The test disk keeps up to 512 written sectors in RAM (enough for FDISK and FORMAT, not for system files), lost when the BB48 restarts. A write past that fails at the PC ("write fault"): that's the limit, not a fault.
- [ ] Console `s` once more, and save the log.

## 5. Pass 2: the scratch image

On the console (the PC off, or at least not using the disk):

- [ ] `E device0 = HDDBENCH.IMG`, Enter; `E mode = auto`, Enter; `!`. `S`: `serving=HDDBENCH.IMG`, and `mode=auto` (PIO up to mode 3). Use `E device0`, not `J`: `J` is forgotten when the BB48 loses power, and it would serve `HDDSYNTH.IMG` next time.
- [ ] Reboot the PC; it detects the drive again (the geometry is the same, 64 MB). Note the mode it sets: `SET_FEATURES 3 ...` (`8` = PIO 0, `9`-`11` = PIO 1-3). Many 486 BIOSes never send it, and stay at their default timing.
- [ ] `FDISK`, a primary partition, active; reboot; `FORMAT C: /S`. Boot from it.
- [ ] Copy files on and off; time copying the large file (from floppy to C: is limited by the floppy; C: to C: is the disk's).
- [ ] The benchmarks again, now on a real file system. Photos.
- [ ] `s`: `iordy_ignored=0`, `iordy_max_ns`, `card=present`, no `CARD` or `STUCK` lines. Save the log.

Write-behind is on, so a power cut in the few ms while a sector is being stored loses that sector: wait a few seconds after the last write before switching the PC off. It's a scratch image either way.

## 6. Afterwards, back on the bench

- [ ] `E device0 = HDDSYNTH.IMG`, `E mode = auto`, `!`: the rig as it was.
- [ ] The logs, the `s` lines and the photos come back to the repo: `host/runs/` for the logs, the results into [speed-log.md](speed-log.md) and `CHANGELOG.md`. Note the PC: CPU, the board or the IDE card (ISA, VLB or PCI), and the BIOS make and date.

## If something fails

| Symptom | Look at |
|---|---|
| No `RESET` or `CMD` lines at all | The cable (pin 1, the right header), the channel enabled in the BIOS, `p` showing the bus lines high with the PC on |
| `IDENTIFY` seen, but the BIOS finds nothing, or a garbled name | `s`: `iordy_ignored` above 0 (the card doesn't wait for IORDY), `bus_identify_bad`; and the `CMD` lines after IDENTIFY |
| Detected, but DOS hangs or reports errors reading | `STUCK`, `SLOWREAD`, `BUSERR` lines; the `CMD` before the hang. Save the log |
| The BIOS shows the name with every other character missing or garbled, yet `bus_identify_bad=0` | A 16-bit ISA card that needs the drive's IOCS16#, which the BB48 doesn't have (PCB rev 0.4 adds it); [no-iordy.md](no-iordy.md). Note the card and stop |
| The wrong size, or the BIOS rejects the geometry | Enter 130/16/63 as a user type; a BIOS with a 504 MB limit is no problem at 64 MB |
| Detection works once, not after a warm reboot | The `RESET` lines on the reboot: some BIOSes only do a soft reset (SRST) |
