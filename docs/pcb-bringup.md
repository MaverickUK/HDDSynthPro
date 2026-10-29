# PCB bring-up: rev 0.3

The first day with the HDDSynthPro board (the `hardware/` KiCad project, [pcb.html](pcb.html)). In order: each step either passes or says what to fix before the next. It mirrors the BB48 rig's [bringup.md](bringup.md), with what's new on the board: the RP2350B on the board itself, the bus switches, card detect, and DMARQ/DMACK#.

You'll need: a multimeter with a continuity beeper, a USB-C cable, the USB-IDE adapter and a 40-pin IDE cable, the microSD card, and firmware **0.16.6 or later** (it switches the bus on; earlier firmware needs JP1 closed).

## 1. Before any power

- [ ] **Look it over.** The four SN74CB3T3245 bus switches (U2, U3, U5, U6) and the RP2350B the right way round (pin 1 marks), no solder bridges on the QFN-80 and TSSOP-20 legs, the IDE header's key at pin 20.
- [ ] **No shorts on the supplies.** 3V3 to GND and VBUS/5 V to GND should read well above 100 Ω (not a beep).
- [ ] **JP1 open.** Since 0.16.6 the firmware drives BUS_EN# (GPIO 41) low to connect the bus once it's ready; with JP1 open the bus stays disconnected until then. Close JP1 only to run firmware older than 0.16.6.
- [ ] **No IDE cable yet.**

## 2. First power: USB-C only

- [ ] Plug in USB-C. The power LED (D4) lights. 3V3 reads 3.3 V.
- [ ] The board appears as **RP2350 Boot** (hold BOOTSEL while plugging in, if it doesn't). Flash the drive build: `pio run -e bb48_drive -t upload`. It's the BB48's firmware unchanged: the board keeps the same GPIOs.
- [ ] Console: `h` answers `READY HDDSynthPro <version> slice3`.
- [ ] `p`: `BUS_EN#=0` (the firmware has the bus switches on). With no cable the bus lines float.
- [ ] **Solder-bridge test**, cable still unplugged: `x` then `Y`. It now also checks through the bus switches to the IDE header. `SHORTTEST OK` passes; a `SHORT` or `STUCK` line names the pins to look at (see [bringup.md](bringup.md), step 2).

## 3. The microSD card

- [ ] Card out: `p` shows `SD_CD=1` (the socket's switch, pulled up).
- [ ] Card in: `SD_CD=0`. If it reads the other way round, note it: the re-mount code will use card detect later, and needs to know (rev 0.3's schematic notes this as unchecked).
- [ ] `S`: `SD ok ...` and the CONFIG lines. The card runs over SPI at 25 MHz, as on the rig.

## 4. Connect the adapter

The bus switches isolate the bus when the board is unpowered (Ioff), so the BB48's power-order rule no longer applies. Powering the board first is still a good habit on the first day.

- [ ] Plug the IDE cable in, then the adapter's USB.
- [ ] `p`: DIOR#, DIOW#, CS0#, CS1#, RESET# = 1; IORDY = 1; INTRQ = 0; `DMARQ=0 DMACK#=1`.
- [ ] `S`: **`CONFIG dmarq=wired`**. The drive tests DMARQ with a pull-up against the host's pull-down after switching the bus on. `absent` means DMARQ isn't reaching the host: check U5's channel and the IDE header's pin 21.
- [ ] The adapter detects the drive: `CMD ... IDENTIFY`, `SET_FEATURES 3 11` (it chose PIO mode 3), and the host sees the disk.

## 5. The drive's tests, in PIO

- [ ] `scripts/rig.sh -m slice1` and `-m slice2` (detection, IORDY timing; `HDDPRO_REPLUG=redetect` runs them hands-free).
- [ ] On the built-in disk (`E image = none`, `!`): `-m slice3`, `-m slice4`, with the bus observer checking every sector on the cable (`s`: `bus_sectors`, `bus_bad=0`).
- [ ] `-m speed`. Compare with the rig's 0.16.x figures in [speed-log.md](speed-log.md) (PIO mode 3: reads about 1.36 MB/s, writes 1.79 on the built-in disk).
- [ ] `s`: `iordy_max_ns` under 1,250, `iordy_ignored=0`.

## 6. Multiword DMA

- [ ] `E mode = mdma2`, `!`. The adapter asks for `SET_FEATURES 3 34` (multiword DMA mode 2) and reads with `READ_DMA`.
- [ ] Slices 3-4 and `-m speed` again. The rig's figures: reads about 5.1 MB/s, random 4 KB reads about 970 a second, writes about 9.8 MB/s (2.6 MB/s reads with the observer checking every DMA word, which costs core 0).
- [ ] `E mode = auto`, `!` before serving an image, until DMA has had a soak (`cfg::AUTO_DMA`).

## 7. The SD image

- [ ] `E image = HDDSYNTH.IMG`, `!`. `-m slice5`, `-m card` (the card pulled and put back).
- [ ] **SDIO** ([pio-allocation.md](pio-allocation.md)): unreliable on the rig's hand-wired leads, so the first real test is here. The quick experiment from 29 Oct was four small edits: `cfg::SD_SDIO = true` and `SD_SDIO_CLKDIV` (2.0 for 25 MHz at 200 MHz) in `config.h`; `startCard()` calling `s_sd.begin(SdioConfig(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_DAT0, clkdiv))`; `mount()` skipping `spiPins()`; `ide_dma::begin()` skipped to free a PIO block. **Take the card out and put it back after flashing it**: a card left in SPI mode by earlier firmware won't start over SDIO until its power is cut. Check with `S` repeated 20 times (every line `image=open`) and `host/runs/tools/writeverify.py` on `HDDBENCH.IMG`, at 25 MHz and then 50 MHz.

## 8. A soak

- [ ] Overnight on the built-in disk (`host/runs/tools/overnight.cmd`, or `soak_dma.sh` for DMA). The rig's reference: 10 hours clean, 28.9 GB, 49.5 million sectors checked on the cable (0.16.0, PIO mode 3).

## If something fails

| Symptom | Look at |
|---|---|
| No `RP2350 Boot` | 3V3, the crystal (Y1) and its 1 kΩ, the flash (U7), the USB 27 Ω resistors |
| `SHORT` / `STUCK` in the solder-bridge test | The pins named, on the RP2350B, the bus switch, and the IDE header |
| `p` shows the bus floating with the adapter on | BUS_EN# reads 0? If not, the firmware is older than 0.16.6 (close JP1) or GPIO 41 isn't reaching the switches' OE |
| `dmarq=absent` with the adapter on | U5's DMARQ channel, IDE pin 21; DMA isn't offered until it's fixed, PIO still works |
| The adapter doesn't detect the drive | `p` levels against step 4; the 33 Ω on DIOR#/DIOW# (R14, R15) |
| Reads slower than the rig, or `SLOWREAD` lines | The bus switches add about 1 ns; anything more points at routing. Compare `iordy_max_ns` with the rig's 1,030-1,100 ns |
