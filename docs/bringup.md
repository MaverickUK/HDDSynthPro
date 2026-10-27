# Bring-up checklist

Hardware day, in order; it is also the checklist for a rebuilt or rewired rig. Each step either passes or tells you what to fix before the next one. The wiring itself is in [wiring.md](wiring.md) and the [interactive diagram](wiring.html).

You'll need: a multimeter with a continuity beeper, the Mac, the USB-IDE adapter (USB-powered; its brick isn't needed), and a plain **BB48** (not the BB48R).

## 1. Before any power

- [ ] **Header orientation.** Check that the BB48's silkscreen matches the diagram: USB-C at the top, GPIO 0–23 on the EXT1 row.
- [ ] **IDC helper strips.** Check with the meter that IDE pins 1 and 2 are **not** connected to each other; nor are any other neighbouring pins. Each header pin needs its own strip.
- [ ] **Continuity, end to end.** For each row of the checklist in the diagram, beep from the IDE header pin to the BB48 header pin. Tick it off as you go. The 33 Ω resistors on DIOR#/DIOW# will read about 33 Ω, not a beep.
- [ ] **Grounds.** Check that every IDE ground you wired (2, 19, 22, 24, 26, 30, 40) beeps to BB48 GND.
- [ ] **No power across.** Check that BB48 +3.3V, VBUS and VSYS do **not** beep to any IDE pin, and that IDE pin 20 (key) is connected to nothing.

## 2. First power: BB48 only, IDE cable unplugged

- [ ] Plug the BB48 into the Mac and flash the listen build: `pio run -e bb48 -t upload`.
- [ ] Open the console: `pio device monitor`. Type `h`; it should answer `READY HDDSynthPro <version> slice0`.
- [ ] **Solder-bridge test.** Type `x`, then `Y`. With the cable unplugged, every pin should read high on its pull-up, and pulling each one low should move no other pin.
  - `SHORTTEST OK 26 pins`: no solder bridges.
  - `SHORT GPIO9 (DD5) GPIO10 (DD6)`: those two are bridged. Fix it, then run the test again.
  - `STUCK GPIO24 (CS1#) low`: that pin is shorted to ground, or to the LED pin.
  - The board reboots when the test finishes.

## 3. Connect the adapter (power order)

- [ ] The BB48 is powered **first**: it stays on USB from now on.
- [ ] Plug the IDE cable into the BB48 side, then plug the adapter's USB into the hub.
- [ ] Type `p`. The expected levels:
  - DIOR#, DIOW#, CS0#, CS1# and RESET# = 1 (idle);
  - IORDY = 1 (the host's pull-up);
  - INTRQ = 0 and DD7 low (the host's pull-downs);
  - the other DD and DA lines may float.
  - A strobe or chip select reading 0 here usually means a wire on the wrong pin.
- [ ] From now on: **adapter USB out before the BB48 is unplugged**. The adapter drives the bus whenever it is powered.

## 4. Slice 0: listen to the adapter

- [ ] Check that `host/rig.env` exists (copy `host/rig.env.example`), so replugs are hands-free. `uhubctl` should still list the hub on port 4.
- [ ] Run `scripts/rig.sh -m slice0 --save-reference`. It replugs the adapter and saves the trace and its summary to `host/reference/`.
- [ ] Read the summary. Things to look for:
  - **Answered on 23 Oct 2026:** a physical replug gives a reset (two RESET# pulses) and a fresh probe; a `uhubctl` replug doesn't, because the hub doesn't cut VBUS.
  - **Was a problem:** with no drive answering, the adapter probes forever, so the bus never goes quiet and the trace dropped events. Since 0.10.2 the test turns on the capture window (`w`), which records 3,000 accesses after each RESET# and only counts the rest, and judges the trace from the first release. Set `HDDPRO_ADAPTER` in `host/rig.env`: the reference is saved per adapter.
  - **Which registers does it touch, in what order?** Turn that into a virtual-host test in `test/native/test_ata_device.cpp`, next to `test_virtual_host_probe`.
  - **Any DMA commands?** Look for `COMMAND` writes of `c8` (READ DMA) or `ef` (SET FEATURES) with a DMA mode. Those mean the NS1081 wants more than PIO.
  - **DIOR# and DIOW# never low together** (the test checks this), and no `STALL` or `DROP` lines.
- [ ] Commit the reference trace.

## 5. The drive build

- [ ] Run `PIO_ENV=bb48_drive scripts/rig.sh -m slice1`. The adapter should get past detection on 20 replugs, and IORDY must never hold a read past ATA's 1250 ns (`iordy_max_ns` in `s`, measured by the IORDY timer PIO program). About 1 µs around detection is normal.
- [ ] Run `PIO_ENV=bb48_drive NO_FLASH=1 scripts/rig.sh -m slice2`. A 64 MB disk appears on every replug, and the trace shows IDENTIFY completing.
  - macOS will say the disk is not readable: choose **Ignore**.
  - The raw tests need `/dev/rdiskN` to be yours: run `sudo chown $USER /dev/rdiskN` after every replug.
- [ ] Run `PIO_ENV=bb48_drive NO_FLASH=1 scripts/rig.sh -m slice3`. Full-disk and random reads with zero mismatches.
- [ ] Run `PIO_ENV=bb48_drive NO_FLASH=1 scripts/rig.sh -m activity`. The drive tells sequential from random reads: each phase spends at least 90% of its time in the right state (see [activity.md](activity.md)).
- [ ] Run `PIO_ENV=bb48_drive NO_FLASH=1 scripts/rig.sh -m slice4`. 1,000 write-then-read cycles in sectors 100,000–100,255 of the RAM overlay, with zero mismatches.
- [ ] Run the soak: `HDDPRO_SOAK_MINUTES=60 PIO_ENV=bb48_drive NO_FLASH=1 scripts/rig.sh -m slice6`. Mixed random reads and writes, every byte checked. The exit test is overnight (`HDDPRO_SOAK_MINUTES=480`); add `HDDPRO_SOAK_MAX_ERRORS=5` to diagnose mismatches rather than stop at the first.
- [ ] Watch the BB48's LED: it flickers with every command, like a hard disk's activity light.

Slices 3 and 4 and the soak compare against the generated disk and write to it, so they skip when the drive serves an SD image. Run them with no `HDDSYNTH.IMG` on the card, or with the card out.

## 6. The SD card

Never format the card or touch its other files: the firmware only ever creates and uses `HDDSYNTH.IMG`.

- [ ] Wire the breakout per [wiring-sd.html](wiring-sd.html): GPIO 34–39, with 10 kΩ pull-ups on CMD and DAT0–DAT3. Power the BB48 off while wiring.
- [ ] On the drive build, type `S`. Expect `SD ok type=SDHC/SDXC size_mb=... fs=FAT32 image=none`. An `SD error` line gives SdFat's code: check the wiring, the pull-ups and that the card has a FAT or exFAT volume.
- [ ] Type `I` to create `HDDSYNTH.IMG` (64 MB of zeros, about 50 s). It refuses if the file is already there or the card lacks room.
- [ ] Reboot the BB48 and replug the adapter. `s` shows `disk=image`, and `S` shows `image=open`.
- [ ] On the Mac, use Disk Utility to erase the new disk as MS-DOS (FAT) with a Master Boot Record. (`diskutil eraseDisk` from the terminal failed with "Couldn't open device"; Disk Utility works.)
- [ ] Copy a file to the `HDDSYNTH` volume, eject it, power everything off and on in the right order, and check the file is intact.

## If something fails

- **The adapter is stuck** (no disk after a replug, a loop of failed writes, or no bus activity after the Mac slept): only a physical replug of the adapter's USB resets it. `uhubctl` doesn't cut VBUS on this hub.
- **Power order slipped** (the adapter powered while the BB48 was off): power the BB48, then replug the adapter.
- **Data tests stall or fail** with the raw trace on: turn `t` off. It slows core 0's fetches enough to stall the adapter.

- **Evidence.** Each failing test leaves a folder in `host/runs/` with the drive's trace, the macOS log and the disk list.
- **Raw accesses.** On the drive build, type `t` to add raw register accesses to the trace, then replug.
- **No bus activity at all.**
  - Check `p` with the adapter on; a strobe stuck low points at wiring.
  - Check the adapter is plugged in and listed by `uhubctl`.
  - Run the solder-bridge test again with the cable unplugged.
- **Detection fails on the drive build** but slice 0 looked fine: compare the slice 0 reference with the drive build's raw trace (`t`). The first register read whose value differs from what a real drive would return is usually the culprit.
