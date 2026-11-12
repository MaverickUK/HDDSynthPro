# IDE Synth Bay hardware

The KiCad project for the IDE Synth Bay board (formerly HDD Synth Pro): an RP2350B that acts as an IDE drive. The files keep the old name (`hddsynthpro.kicad_*`, the `HDDSynthPro` library). The microcontroller and its support parts (core regulator, crystal, flash, USB) are on the board, following Raspberry Pi's RP2350 minimal design; rev 0.1 used a Pimoroni PGA2350 module instead. The design and the reasons for it are in [docs/pcb.html](../docs/pcb.html); open it in a browser.

Status: **rev 0.4, ordered 10 Nov 2026**; `production/` holds the files sent. Rev 0.3 (ordered 28 Oct) is in `production/` and in git history (commit 4fee041). What rev 0.4 adds, and why: [docs/pcb-rev04.md](../docs/pcb-rev04.md).

## Rev 0.4: where it stands

- **Pin 20 (9 Nov):** J5 and J7 use copies of KiCad's footprints with pin 20 as an unplated hole (`make_footprints.py`), since no stocked header leaves it out; rev 0.3's had to be cut. KiCad drops an unplated hole's pin number, so DRC's schematic check gives two expected `net_conflict` warnings, "No pad found for pin 20" on J5 and J7.
- **Solder mask (12 Nov):** the RP2350B (U1) and the USB-C socket (J1) use copies of KiCad's footprints (`make_footprints.py`) with one mask opening per row of fine-pitch pads. Their pads are 0.2 mm apart, and PCBWay can't make a strip of black mask narrower than 0.22 mm between pads, so it asked whether to leave those strips out. For rev 0.4's order the answer was to leave them out on U1 and keep the black. The narrowest strip left on the board is 0.25 mm (the bus switches). The files in `production/` are still the ones sent, without this change.
- **Done:** the schematic (ERC: 0 errors, 0 warnings; `check_netlist.py` passes, and was checked to fail on a wrong pin), the Molex footprint, the placement and the routing (Freerouting 2.5.0 on Windows, then `tools/widen_power.py`). DRC with `--schematic-parity`: 0 unconnected pads, 0 differences from the schematic, only rev 0.3's two expected USB-C silkscreen warnings.
- **VB, the 5 V supply:** Freerouting ignores the Power class's 0.6 mm, so `layout_pcb.py` lays VB's two long branches (D6, Molex, up the right side; D7, 44-pin, along the bottom edge) 0.6 mm wide and locked before routing: about 0.08 ohm, 16 mV at 200 mA. The run along the top (D5, D1, C1, D2) is the router's, widened to mostly 0.3-0.5 mm (about 19 mm at 0.15-0.2 mm near pads; about 0.13 ohm from USB). Fixing that run too was tried three ways (8 Nov): each crowded the PSRAM and flash's QSPI lines, the board's tightest spot, and left one or two of them unrouted.
- **Worth a look in review:** SD_VDD (mostly 0.2-0.4 mm), and the chip side's detours, as on rev 0.3.
- **Before ordering:** a day with rev 0.3 ([pcb-bringup.md](../docs/pcb-bringup.md)) to choose the series resistors and confirm SDIO; check J8's and J9's footprints against real parts (no LCSC part found for J8); pull pin 20 (IDE's key) out of J5 and J7 with pliers after assembly (it's in an unplated hole, ringed on the silkscreen, so it isn't soldered); check J7's rotation in JLCPCB's preview (its `FT Rotation Offset` is copied from the 40-pin header's, unchecked).
- **Not yet updated for rev 0.4:** [docs/pcb.html](../docs/pcb.html), the interactive design page, and `tools/jlcpcb.py`'s `JLC_FIX` table.

## Files

| File | What |
|---|---|
| `hddsynthpro.kicad_pro` | The project: open this in KiCad (9 or 10) |
| `hddsynthpro.kicad_dru` | One custom design rule: 0.125 mm spacing inside Raspberry Pi's regulator layout only |
| `hddsynthpro.kicad_sch` | Root sheet, with one sheet per block |
| `power.kicad_sch` | USB-C, the 5 V header, the link's 5 V input, (rev 0.4) the Molex and 44-pin 5 V inputs, diode-OR, the AMS1117 3.3 V regulator, ESD, BOOTSEL/RESET, SWD |
| `mcu.kicad_sch` | The RP2350B and its support circuit, (rev 0.4) the PSRAM, the activity link socket (J3) and the LEDs |
| `ide.kicad_sch` | The four SN74CB3T3245 bus switches, the series-resistor footprints, the 40-pin and (rev 0.4) 44-pin IDE headers |
| `sd.kicad_sch` | The microSD socket, its pull-ups and (rev 0.4) its power switch |
| `audio.kicad_sch` | Rev 0.4's CD audio out: PWM, RC filters and the 4-pin header |
| `hddsynthpro.kicad_pcb` | The board: 100.33 × 69.88 mm, the HDD Synth Mk5's outline and M3 holes, 4 layers (signals, GND plane, 3V3 plane, signals) |
| `HDDSynthPro.kicad_sym`, `HDDSynthPro.pretty/` | Parts KiCad lacks: the SN74CB3T3245's symbol (pinout from TI's datasheet); Raspberry Pi's polarity-marked inductor and wide-pad 0402 footprints; the TF PUSH microSD socket; the RP2350B's QFN-80 and the USB-C socket with one mask opening per row of pads; the logo and the easter eggs. (The old PGA2350 symbol and footprint stay for reference.) |
| `tools/gen_schematic.py` | Generated the schematic, with an LCSC part number and any Fabrication Toolkit offsets on every part |
| `tools/layout_pcb.py` | Placed the parts and laid the fixed copper (run with KiCad's own Python) |
| `tools/make_footprints.py` | Made the inductor, wide 0402, TF PUSH, (rev 0.4) Molex disk-power, pin-20-unplated IDE header and one-mask-opening QFN-80 and USB-C footprints |
| `tools/make_logo.py` | Makes the silkscreen logo footprint from `media/IDESynthBay.png` |
| `tools/make_eggs.py` | Makes the easter eggs on the back (silkscreen footprints) from `media/Defrag.png` and `media/ParkHeads.png` |
| `tools/autoroute.py` | Routed the rest with Freerouting |
| `tools/jlcpcb.py` | Writes `fab/bom-jlcpcb.csv` and `fab/cpl-jlcpcb.csv` for JLCPCB's assembly service |
| `tools/check_netlist.py` | Checks the schematic against `src/config.h` and Raspberry Pi's support circuit |

Parts connect through net labels, not drawn wires: a label on a pin's short wire joins every other label of the same name on that sheet, and a global label (the arrow-shaped ones) joins its name across all sheets. GND and +3V3 are power symbols, which are global too.

## Checking it

```sh
cd hardware
kicad-cli sch erc hddsynthpro.kicad_sch                      # electrical rules: 0 violations
python3 tools/check_netlist.py                               # GPIO map and RP2350 support circuit
kicad-cli pcb drc --schematic-parity hddsynthpro.kicad_pcb   # 0 errors, 0 unconnected, 0 parity issues
$KICAD_PYTHON tools/check_mask_webs.py hddsynthpro.kicad_pcb  # no solder mask strip under 0.22 mm (KiCad's Python)
```

On Windows (KiCad 10 in `C:\Program Files\KiCad\10.0`), from Git Bash: put `/c/Program Files/KiCad/10.0/bin` on `PATH`, set `KICAD_SHARE="C:/Program Files/KiCad/10.0/share/kicad"` for `gen_schematic.py`, and run `layout_pcb.py` with that folder's `python.exe`. `gen_schematic.py` reproduces the committed schematic byte for byte there.

`check_netlist.py` exports the netlist with `kicad-cli` and follows each IDE signal from its GPIO, through the 33 Ω resistor where there is one and the bus switch, to the IDE header, and checks it lands on the ATA pin. It also checks the microSD socket, the link, the LED, the bus-switch enable, the IDE grounds, and the RP2350B's support circuit against Raspberry Pi's design: every supply pin's rail, the inductor's polarity, the VREG_AVDD filter, the crystal and its 1 kΩ, the flash wiring and the USB 27 Ω resistors. Run it after every schematic edit: CLAUDE.md requires the pin map to agree everywhere. (DRC's two remaining warnings are the USB-C socket's outline overhanging the board edge, as it's meant to.)

## The board

The outline (100.33 × 69.88 mm, 2.54 mm corner radius) and the four M3 holes are the HDD Synth Mk5's, so the two boards stack on standoffs. The holes sit 9.76 mm in from the left edge, 13.97 mm from the right and 4.09 mm from the top and bottom.

The 40-pin IDE header runs down the left side, pin 1 at the top, just inside the left-hand holes (its shroud is too long to fit between them). The four bus switches (SN74CB3T3245, 8 channels each) sit in a column beside it, IDE side (B, pins 11-18) facing it, each with its 100 nF below it. Each takes eight consecutive header signals in pin order, so those traces run straight: U2 RESET# and DD4-DD10, U3 DD0-DD3 and DD11-DD14, U5 DD15, the strobes, IORDY, INTRQ, DMA and DA1, U6 DA0, DA2 and the chip selects (four spare channels).

The RP2350B (QFN-80) is in the middle, its data pins (left side) facing the switches. The GPIO numbers are the PGA2350's, so the firmware's pin map is unchanged. Around it:
- **Above:** Raspberry Pi's core regulator (the polarity-marked 3.3 µH inductor, 4.7 µF caps, 33 Ω/4.7 µF filter), the USB 27 Ω resistors and the QSPI flash, all placed and laid out exactly as on their RP2350A minimal board, whose pins sit in the same places relative to the regulator.
- **Below:** the 12 MHz crystal with its 15 pF caps and 1 kΩ.
- **Under the chip:** each supply pin has a short stub to its own via (3V3 to the 3V3 plane, 1V1 to a bottom-layer track joining the DVDD pins to the regulator), leaving the outward lanes for signals.

USB-C, the 5 V input, the link socket and the SWD header are along the top edge, with the AMS1117 by the power parts; the microSD socket is on the right edge, card in from the right, with its pull-ups beside it; the buttons are at the bottom right, and the logo at the bottom, under the chip.

Design rules are set for a 4-layer board at JLCPCB-class fabs: 0.2 mm tracks and 0.15 mm clearance by default, 0.6 mm for the `Power` net class (VB, VBUS_USB, 5V_EXT, 5V_LINK), 0.6/0.3 mm vias (0.5/0.3 for the fixed ones). Project rules that differ from KiCad's defaults: minimum track 0.1 mm (Freerouting necks some traces to 0.12 mm between pads), minimum clearance 0.12 mm with 0.125 mm allowed only inside the `RP2350 core` area (Raspberry Pi's layout), minimum drill 0.2 mm (the QFN's thermal vias), and one thermal spoke enough on the outer GND pours.

### Routing

Some copper is laid by `layout_pcb.py` and locked, so the router works round it:
- Raspberry Pi's regulator fills, tracks and vias, and their QSPI fan-out to the flash;
- the supply stubs and vias under the RP2350B, and the 1V1 track joining the DVDD pins;
- USB D+/D- from the 27 Ω resistors through U4 to both rows of USB-C contacts (the inner contacts cross over on the bottom layer), and VBUS.

The rest is autorouted with Freerouting 2.5.0 (the Java build; the native CLI crashed). `autoroute.py` first gives every other surface-mount GND and +3V3 pad a short stub and via to its plane, passes the inner layers to Freerouting as planes so it never routes on them, then removes dangling tracks and vias and pours GND on both outer layers.

What to look at when reviewing it:
- **The IDE side is short and direct**: header to switch B pins, on the top layer.
- **The chip side is autorouter-shaped**: some runs take detours, partly on the bottom layer. Mode 0 timing doesn't mind, but shorter runs, especially for DIOR#/DIOW#/IORDY, would be better.
- **Ground return**: signals that change layer don't have a GND via beside them; the inner GND plane is under everything, which covers most of it.

To route again after moving parts (it keeps existing traces, so delete the ones you want rerouted): get the Freerouting jar and a Java 25 runtime (Eclipse Temurin; on Windows `winget install EclipseAdoptium.Temurin.25.JRE`), then

```sh
FREEROUTING=freerouting-2.5.0.jar JAVA=.../bin/java \
  /Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 tools/autoroute.py
kicad-cli pcb drc --refill-zones --save-board --schematic-parity hddsynthpro.kicad_pcb
```

Then `tools/widen_power.py` (KiCad's Python, `kicad-cli` on `PATH`) widens VB to 0.6 mm and SD_VDD to 0.4 mm wherever the clearances allow, stepping each segment down until DRC is clean: Freerouting doesn't keep the Power class's width.

The logo (17.2 × 15 mm, bottom middle of the front) is the board-only footprint `LOGO_IDESynthBay`: dark pixels of `media/IDESynthBay.png` as silkscreen (the discs' grey shading drops out), "BAY"'s thin bars closed into solid letters, strokes widened so they print. The easter eggs on the back come the same way from `tools/make_eggs.py`. `tools/layout_pcb.py --silkscreen hddsynthpro.kicad_pcb` redoes the logo, the easter eggs and the labels on the routed board without touching the traces; `--fields` copies the schematic's fields (LCSC numbers, offsets) onto it.

## Editing

The `.kicad_sch` and `.kicad_pcb` files are the source of truth once edited in KiCad. Running `gen_schematic.py` or `layout_pcb.py` again **overwrites** them (the layout loses its routing), so use them only to start over.

## Ordering assembled boards from JLCPCB

Every part carries its JLCPCB part number in an `LCSC` field, so the Fabrication Toolkit plugin's BOM has them. Parts whose JLCPCB footprint is turned or centred differently from KiCad's also carry `FT Rotation Offset` / `FT Position Offset` fields, so the plugin's placement file is right too. These were found by fitting JLCPCB's own footprints onto KiCad's.

`python3 tools/jlcpcb.py` writes the same BOM and placement files independently (its `JLC_FIX` table holds the same corrections). On 27 Oct 2026 the parts for 5 boards came to about $68.50 at JLCPCB, with 13 extended (setup-fee) lines; the bus switches are about half of it.

- **The inductor (L1, C42411119) was out of stock** at JLCPCB on 27 Oct 2026 (6 that morning, 0 that afternoon). Raspberry Pi require this part. Buy it through JLCPCB's global sourcing or pre-order, or send your own.
- **Check rotations in JLCPCB's placement preview**, especially L1's polarity dot (at its 1.1 V end, away from the RP2350's LX pin), the TSSOP switches, the SOT-23-6, the QFN, the flash and the USB-C socket.
- **Cheaper flash:** the firmware is about 100 KB, so a 2 MB W25Q16JVSSIQ (C131025, same footprint, about $1.24) would do instead of the 16 MB W25Q128 ($2.58), if the firmware's flash size is set to match.

## Before ordering a board

- **Power from the HDD Synth.** J3's pin 1 takes 5 V through D5 (a B5819W since rev 0.4; an SS14 before) into VB, so one supply can run both boards. The Mk5's J6 pin 1 carries 3.3 V: with a Mk5, leave pin 1 of the cable unconnected (3.3 V there wouldn't damage anything, but the board would brown out on it alone). Any input works on its own; with several, the highest wins.
- **Card detect.** On generic push-push sockets the detect pin usually meets the grounded shell when a card is in (reads low with R12 pulling up); check on the first board.
- **New pins.** The board adds GP41 (`BUS_EN#`, the bus switches' enable), GP40 (card detect), GP31 and GP3 (DMARQ and DMACK#, reserved). The firmware doesn't use them yet. Until it drives GP41 low, close solder jumper JP1, or the IDE bus stays disconnected.
- **Firmware build.** The RP2350B is the PGA2350's chip with the same GPIOs, 12 MHz crystal and 16 MB flash, but it's no longer the BB48 board: the PlatformIO board for this PCB needs a generic RP2350B definition rather than `olimex_pico2bb48`.
