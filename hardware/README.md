# HDDSynthPro hardware

The KiCad project for the HDDSynthPro board: a Pimoroni PGA2350 (RP2350B) that acts as an IDE drive. The design and the reasons for it are in [docs/pcb.html](../docs/pcb.html); open it in a browser.

Status: **not reviewed**. The schematic is complete and the board is placed and autorouted: DRC finds 0 violations, 0 unconnected items and 0 differences from the schematic. Review the routing (below) before ordering.

## Files

| File | What |
|---|---|
| `hddsynthpro.kicad_pro` | The project: open this in KiCad (9 or 10) |
| `hddsynthpro.kicad_sch` | Root sheet, with one sheet per block |
| `power.kicad_sch` | USB-C, the 5 V header, diode-OR, ESD, BOOTSEL/RESET, SWD |
| `mcu.kicad_sch` | The PGA2350, the activity link (J3) and the LEDs |
| `ide.kicad_sch` | The four SN74CB3T3245 bus switches and the 40-pin IDE header |
| `sd.kicad_sch` | The microSD socket and its pull-ups |
| `hddsynthpro.kicad_pcb` | The board: 85 × 76 mm, 4 layers (signals, GND plane, 3V3 plane, signals), four M3 holes |
| `HDDSynthPro.kicad_sym`, `HDDSynthPro.pretty/` | The PGA2350's symbol and footprint, the SN74CB3T3245's symbol (pinout from TI's datasheet), and the logo (everything else is from KiCad's own libraries) |
| `tools/gen_schematic.py` | Generated the first version of the schematic |
| `tools/layout_pcb.py` | Placed the parts on the first version of the board (run with KiCad's own Python) |
| `tools/make_logo.py` | Made the silkscreen logo footprint from `media/HDDSynthProWide3.png` |
| `tools/autoroute.py` | Routed it with Freerouting, after a via from every surface-mount GND/3V3 pad to its plane |
| `tools/jlcpcb.py` | Writes `fab/bom-jlcpcb.csv` and `fab/cpl-jlcpcb.csv` for JLCPCB's assembly service, with a JLCPCB part number on every line |
| `tools/check_netlist.py` | Checks the schematic against `src/config.h` |

Parts connect through net labels, not drawn wires: a label on a pin's short wire joins every other label of the same name on that sheet, and a global label (the arrow-shaped ones) joins its name across all sheets. GND and +3V3 are power symbols, which are global too.

## Checking it

```sh
cd hardware
kicad-cli sch erc hddsynthpro.kicad_sch      # electrical rules: 0 violations
python3 tools/check_netlist.py               # GPIO map against src/config.h
```

`check_netlist.py` exports the netlist with `kicad-cli` and follows each IDE signal from its GPIO, through the 33 Ω resistor where there is one and the bus switch, to the IDE header, and checks it lands on the ATA pin. It also checks the microSD socket, the link, the LED, the bus-switch enable and the IDE grounds. Run it after every schematic edit: CLAUDE.md requires the pin map to agree everywhere.

## The board

The 40-pin IDE header runs down the left edge, pin 1 at the top. The four bus switches (SN74CB3T3245, 8 channels each) sit in a column beside it, IDE side (B, pins 11-18) facing it, each with its 100 nF below it. Each takes eight consecutive header signals in pin order, so those traces run straight: U2 RESET# and DD4-DD10, U3 DD0-DD3 and DD11-DD14, U5 DD15, the strobes, IORDY, INTRQ, DMA and DA1, U6 DA0, DA2 and the chip selects (four spare channels). The PGA2350 is in the middle, its bus pins on its left and bottom edges facing the switches. USB-C, the 5 V input, the link socket, the buttons and the SWD header are along the top edge; the microSD socket is on the right edge, card in from the right, with its pull-ups beside it.

Design rules are set for a 4-layer board at JLCPCB-class fabs: 0.2 mm tracks and 0.15 mm clearance by default, 0.6 mm for the `Power` net class (VB, VBUS_USB, 5V_EXT), 0.6/0.3 mm vias. GND and +3V3 are the inner planes; every surface-mount pad on them has its own via.

```sh
kicad-cli pcb drc --schematic-parity hddsynthpro.kicad_pcb   # 0 violations, 0 unconnected, 0 parity issues
```

Two project rules differ from KiCad's defaults: the minimum track is 0.1 mm, because Freerouting necks a few traces to 0.12 mm between pads (fabs make 0.09 mm on 4-layer boards), and one thermal spoke is enough on the outer GND pours, because through-hole GND pins also join the inner plane.

### Routing

Autorouted with Freerouting 2.5.0 (the Java build; the native CLI crashed). Before routing, `autoroute.py` gives every surface-mount GND and +3V3 pad a short locked stub and via to its plane; the inner layers are passed to Freerouting as planes, so it never routes on them; afterwards both outer layers get a GND pour.

What to look at when reviewing it:
- **The IDE side is short and direct**: header to switch B pins, all on the top layer.
- **The module side is autorouter-shaped**: some PGA2350-to-switch runs, the strobes through R14/R15 and some SD lines take long detours, partly on the bottom layer. Mode 0 timing doesn't mind, but shorter runs, especially for DIOR#/DIOW#/IORDY, would be better.
- **Ground return**: signals that change layer don't have a GND via beside them; the inner GND plane is under everything, which covers most of it.

To route again after moving parts (it keeps existing traces, so delete the ones you want rerouted): get the Freerouting jar and a Java 25 runtime (Eclipse Temurin), then

```sh
FREEROUTING=freerouting-2.5.0.jar JAVA=.../bin/java \
  /Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 tools/autoroute.py
kicad-cli pcb drc --refill-zones --save-board --schematic-parity hddsynthpro.kicad_pcb
```

The logo (19.4 × 16 mm, bottom of the board) is the board-only footprint `LOGO_HDDSynthPro`: dark pixels of `media/HDDSynthProWide3.png` as silkscreen, strokes widened to about 0.15 mm so they print. `tools/layout_pcb.py --silkscreen hddsynthpro.kicad_pcb` redoes the logo and labels on the routed board without touching the traces.

## Editing

After the first edit in KiCad, the `.kicad_sch` and `.kicad_pcb` files are the source of truth. Running `gen_schematic.py` or `layout_pcb.py` again **overwrites** them (the layout loses its routing), so use them only to start over.

## Ordering assembled boards from JLCPCB

`python3 tools/jlcpcb.py` writes the BOM and placement files. Upload them instead of a BOM exported from KiCad: every line already has its JLCPCB part number. Every part was in stock at JLCPCB on 27 Oct 2026 (the bus switches, SN74CB3T3245PWR C15298, had 4,118; the 2x20 header, C19193815, 746). Left off, to fit by hand: the PGA2350 (U1, from Pimoroni). J2, J3, J5 and J6 are through-hole: JLCPCB fits them at extra cost, or move them to `HAND_FIT` in the script and solder them yourself.

- **Check rotations in JLCPCB's placement preview.** Its idea of 0° differs from KiCad's for some packages; the usual suspects are the TSSOP switches, the SOT-23-6, the diodes and LEDs, and the USB-C socket. Fix any that are wrong in the preview.

## Before ordering a board

- **PGA2350 ground pads.** Pimoroni's schematic shows six GND pins but doesn't name their pads. A1, A10, B2, B9, J1 and J10 are the six pads left over once every other signal is placed. Check them against a module, or Pimoroni's own footprint if they publish one.
- **The footprint.** It's drawn from Pimoroni's pinout and mechanical drawing: 64 pads on a 2.54 mm grid, A1 at the top left, seen from the top. Print it 1:1 and lay a module on it.
- **Card detect.** Which way the microSD socket's switch closes depends on the part; the firmware will need to know.
- **New pins.** The board adds GP41 (`BUS_EN#`, the bus switches' enable), GP40 (card detect), GP31 and GP3 (DMARQ and DMACK#, reserved). The firmware doesn't use them yet. Until it drives GP41 low, close solder jumper JP1, or the IDE bus stays disconnected.
