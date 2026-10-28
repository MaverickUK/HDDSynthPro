# HDD Synth Pro

<img src="media/HDDSynthProWide3.png" alt="HDD Synth Pro logo" width="300" align="left">

An RP2350 that pretends to be an IDE hard disk, as the base for a future HDD Synth that *is* the drive rather than one that listens to it.

At this stage it is a proof of concept on an Olimex RP2350-PICO2-BB48, tested through a USB-IDE adapter from the same Mac that builds it. See [docs/plan.md](docs/plan.md) for the slices and [docs/wiring.md](docs/wiring.md) for the connections. For an interactive wiring diagram, open [docs/wiring.html](docs/wiring.html) in a browser. [docs/bringup.md](docs/bringup.md) is the checklist for hardware day. [docs/pcb-bom.md](docs/pcb-bom.md) sketches the custom PCB that follows the prototype. [docs/activity.md](docs/activity.md) explains how the drive tells sequential from random activity, for more realistic sounds.

The logo? The pro part is meant to be an IDE cable, with pin 1 being red :)

<br clear="left">

## Prototyping

<img src="media/prototype1.jpg" >

<img src="media/prototype2.jpg" >

Currently I'm creating a proof of concept on a BB48 dev board that exposes all of the GPIO lines of the RP2350 chip so that I'm able to hook them up to the IDE bus.

<img src="media/success1.jpg" >

<img src="media/success2.jpg" >

Which means it's possible to create an emulated IDE drive mounted on an SD card.

I'm not yet sure if this will exist alongside the original HDD Synth or will supersede it, time will tell

## Status

The BB48 works as a read/write IDE hard disk (PIO mode 0) behind a USB-IDE adapter: the Mac sees it, partitions and formats it, and copies files to and from it.

- **The drive build** (`pio run -e bb48_drive`) serves `HDDSYNTH.IMG` from a microSD card as the disk, so files survive a power cycle. Without a card, or without an image it can use, it serves a 64 MB welcome disk: a FAT16 volume holding `README.TXT` (from [welcome/README.TXT](welcome/README.TXT)) and a sample `HDDSYNTH.INI`, with writes kept in RAM until reboot. The rig tests use a generated 64 MB test disk instead (`image = none`, or console `T`). A second image, `HDDBENCH.IMG`, is a scratch disk for benchmarks that write. `HDDSYNTH.INI` on the card chooses another image and the transfer mode: see [docs/settings.md](docs/settings.md).
- **Speed:** reads about 1.25 MB/s from either disk, writes 1.55 MB/s to RAM and 0.92 MB/s to the card. The USB adapter's own waits cap this rig below about 3 MB/s whatever the drive does. Each step is in [docs/speed-log.md](docs/speed-log.md).
- **Tested on hardware:** detection, IDENTIFY, reads, writes, standby and wake, FLUSH CACHE, no card at boot, a card pulled mid-read, and soaks of mixed reads and writes: 13.5 hours clean on 0.8.22 (from macOS and Windows), with every sector checked on the cable by the drive itself. Every read is answered within ATA's 1.25 µs limit.
- **The board's LED** lights for drive activity, as a hard disk's would.
- **Next:** an overnight soak, faster write timing, PIO mode 4, and the activity link to the HDD Synth ([docs/activity-link.md](docs/activity-link.md)).

The listen-only build (`pio run -e bb48`) records every IDE register access it sees without driving any pin, for studying what a host does.

## Quick start

```sh
test/run_native_tests.sh                 # C++ unit tests (no hardware)
cd host && python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
.venv/bin/pytest -m sim                  # simulator and parser tests (no hardware)

pio run -e bb48_drive -t upload          # build and flash the drive
pio run -t upload                        # or the listen-only build
pio device monitor                       # h help, p pin levels, s stats, S SD card

PIO_ENV=bb48_drive scripts/rig.sh -m slice3   # tests, build, flash, rig test
```

Read the power-order rule in [docs/wiring.md](docs/wiring.md) before connecting the adapter. The SD card's wiring is in [docs/wiring-sd.html](docs/wiring-sd.html).

## Licence

To be decided (HDDSynth_Native is GPL-3.0).
