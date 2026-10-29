# Retro PC test: the BB48 rig in a real PC

The first time the drive sits on a real IDE controller rather than the USB adapter. In order: each step either passes or says what to note before the next. Plan item 6 in [plan.md](plan.md) (a 486-class host); firmware **0.17.1 or later**.

Two passes, ideally on separate evenings:
1. **The built-in disk, PIO mode 0.** Every sector read is checked on the cable by the bus observer, so this pass says whether the controller works with the drive at all, and whether it waits for IORDY.
2. **The scratch image, `auto`.** FDISK, FORMAT, copying files and benchmarks, on `HDDBENCH.IMG`. The SD card's `HDDSYNTH.IMG` isn't touched.

## What to bring

- The BB48 rig, with its IDE cable left on the BB48. Only the cable's other end moves, from the USB adapter to the PC.
- A laptop (the MacBook) and a USB cable for the BB48: it powers the BB48 and is the console. The PC's IDE cable carries no power.
- A DOS boot floppy (or a second drive the PC boots from) with `FDISK`, `FORMAT`, `SYS`, and benchmarks: CORETEST, SPEEDSYS, Norton SI or CheckIt, whichever you have. A large file (a few MB) to time a `COPY` with.
- A phone, for photos of the BIOS screens and the benchmark results.

## 1. On the bench, before you go

With the rig on the USB adapter as usual:

- [ ] Console `h`: `READY HDDSynthPro 0.17.1 slice3` (or later).
- [ ] `E image = none`, Enter; `E mode = pio0`, Enter; `!`. Then `S`: `CONFIG ... image=none mode=pio0` and `dma=off`.
- [ ] `K` (creates `HDDBENCH.IMG`, 64 MB of zeros, if it isn't there yet), for pass 2.

`pio0` offers only the slowest mode, so a BIOS that sets its controller from the IDENTIFY block can't choose a timing the drive hasn't been tried at with this controller. `dma` stays off: a 486's ISA or VLB controller has no multiword DMA anyway.

## 2. Connecting: the power-order rule

The BB48 has no buffers. Its pins are only 5 V tolerant while it's powered, and the PC's controller drives the bus whenever the PC is on, so **the BB48 is powered whenever the PC is**:

- [ ] Everything off. Plug the cable into the PC's IDE header, **red stripe to pin 1**.
- [ ] Put the drive **alone on its channel**, as master: the firmware doesn't do master/slave yet (no DASP#/PDIAG# handshake). If the PC boots from its own hard disk, use the secondary channel for the BB48; otherwise boot from floppy.
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

## 4. Pass 1: reads on the built-in disk

The built-in disk holds a test pattern, with no partitions or file system: DOS sees an unpartitioned disk. That's what lets the observer check every sector read.

- [ ] Boot DOS from floppy. `FDISK`, option 4 (display): no partitions, about 64 MB. Don't create any yet.
- [ ] Benchmarks that only read (CORETEST's transfer rate and seek tests, SPEEDSYS's disk test). Photos of the results.
- [ ] `s`: `bus_sectors` has grown, **`bus_bad=0`** and no `BUSERR` lines; `iordy_ignored=0` still; `iordy_max_ns`. `SLOWREAD` or `STUCK` lines: note them.
- [ ] Optional, writes: `FDISK` a primary partition, reboot, `FORMAT C:` (without `/S`). The built-in disk keeps up to 512 written sectors in RAM (enough for FDISK and FORMAT, not for system files), lost when the BB48 restarts. A write past that fails at the PC ("write fault"): that's the limit, not a fault.
- [ ] Console `s` once more, and save the log.

## 5. Pass 2: the scratch image

On the console (the PC off, or at least not using the disk):

- [ ] `E image = HDDBENCH.IMG`, Enter; `E mode = auto`, Enter; `!`. `S`: `serving=HDDBENCH.IMG`, and `mode=auto` (PIO up to mode 3). Use `E image`, not `J`: `J` is forgotten when the BB48 loses power, and it would serve `HDDSYNTH.IMG` next time.
- [ ] Reboot the PC; it detects the drive again (the geometry is the same, 64 MB). Note the mode it sets: `SET_FEATURES 3 ...` (`8` = PIO 0, `9`-`11` = PIO 1-3). Many 486 BIOSes never send it, and stay at their default timing.
- [ ] `FDISK`, a primary partition, active; reboot; `FORMAT C: /S`. Boot from it.
- [ ] Copy files on and off; time copying the large file (from floppy to C: is limited by the floppy; C: to C: is the disk's).
- [ ] The benchmarks again, now on a real file system. Photos.
- [ ] `s`: `iordy_ignored=0`, `iordy_max_ns`, `card=present`, no `CARD` or `STUCK` lines. Save the log.

Write-behind is on, so a power cut in the few ms while a sector is being stored loses that sector: wait a few seconds after the last write before switching the PC off. It's a scratch image either way.

## 6. Afterwards, back on the bench

- [ ] `E image = HDDSYNTH.IMG`, `E mode = auto`, `!`: the rig as it was.
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
