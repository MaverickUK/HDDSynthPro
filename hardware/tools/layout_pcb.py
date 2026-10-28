"""Creates the first version of the HDDSynthPro board: footprints placed, nets assigned, planes poured.

Run with KiCad's own Python, which has the pcbnew module:
  /Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 \
      hardware/tools/layout_pcb.py [output.kicad_pcb]
  or, on a board that's already routed, redo only the logo and labels, or copy the
  schematic's fields (LCSC numbers, Fabrication Toolkit offsets) onto it:
      hardware/tools/layout_pcb.py --silkscreen hardware/hddsynthpro.kicad_pcb
      hardware/tools/layout_pcb.py --fields hardware/hddsynthpro.kicad_pcb
  then fill the planes:  kicad-cli pcb drc --refill-zones --save-board hardware/hddsynthpro.kicad_pcb

It reads the netlist from the schematic (via kicad-cli), so every footprint is
linked to its symbol and "Update PCB from Schematic" works afterwards. Like
gen_schematic.py, this only bootstraps: once the board has been edited in
KiCad, rerunning it overwrites the edits. Traces aren't routed here.

Layout: the HDD Synth Mk5's outline and holes. The 40-pin IDE header on the left,
vertical, just inside the left-hand holes. The four bus switches sit beside it,
their IDE side facing the header and their RP2350B side facing the chip. The
RP2350B is in the middle; USB-C, power and the link along the top edge; the
microSD socket on the right edge with its pull-ups beside it.
Four layers: signals, GND plane, 3V3 plane, signals.
"""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import pcbnew

sys.path.insert(0, str(Path(__file__).resolve().parent))
from sexpr import find, first, parse  # noqa: E402

HW = Path(__file__).resolve().parent.parent
KICAD_SHARE = Path(os.environ.get("KICAD_SHARE", "/Applications/KiCad/KiCad.app/Contents/SharedSupport"))
OUT = HW / "hddsynthpro.kicad_pcb"

# Board outline, mm. KiCad's y axis points down. The HDD Synth Mk5's outline (100.33 x 69.88 mm,
# 2.54 mm corner radius) and mounting holes, moved so its top left corner is at (X0, Y0), so the
# two boards stack on standoffs.
X0, Y0, W, H = 100.0, 100.0, 100.33, 69.88
CORNER_R = 2.54

# ref -> (x, y, rotation in degrees counterclockwise). Footprint origins: pin 1 for the
# IDE header and the pin socket, the centre for everything else.
PLACE = {
    # IDE side
    "J5": (117.5, 114.0, 0),       # 2x20 header, pin 1 at the top, key facing the edge
    # Bus switches, B side (pins 11-18) facing the header, each beside the header pins it serves.
    "U2": (130.0, 119.0, 180),     # header pins 1-9
    "U3": (130.0, 129.5, 180),     # 10-17
    "U5": (130.0, 141.5, 180),     # 18-33
    "U6": (130.0, 151.5, 180),     # 35-38
    "C4": (130.0, 124.2, 0),       # decoupling, below each switch
    "C5": (130.0, 134.7, 0),
    "C7": (130.0, 146.7, 0),
    "C8": (130.0, 156.7, 0),
    "R13": (127.0, 160.5, 90),     # BUS_EN# pull-up
    "JP1": (129.6, 160.5, 90),     # BUS_EN# to GND
    "R14": (142.0, 148.5, 0),      # DIOR# 33R
    "R15": (142.0, 150.7, 0),      # DIOW# 33R
    # RP2350B, its data side (pins 1-20) facing the switches. Its regulator cluster is placed
    # by core_copper(), relative to the VREG_LX pin, as in Raspberry Pi's layout.
    "U1": (151.0, 132.0, 0),
    "R20": (169.0, 147.6, 0),      # RUN to RESET button
    "Y1": (151.0, 143.5, 0),       # crystal below XIN/XOUT
    "R17": (152.6, 139.6, 90),     # XOUT 1k
    "C27": (147.6, 144.35, 0),     # XIN 15 pF
    "C28": (154.4, 142.65, 0),     # crystal output 15 pF
    # 100 nF on the supply pins: corners and the gaps between signal groups
    "C24": (134.2, 125.0, 90), "C25": (135.8, 125.0, 90), "C19": (137.4, 125.0, 90), "C20": (139.0, 125.0, 90),
    "C12": (140.6, 125.0, 90),    # left of the QSPI fan-out
    "C13": (144.6, 138.4, 0), "C14": (144.6, 139.6, 0), "C15": (148.4, 141.2, 0),
    "C16": (157.4, 138.4, 0), "C21": (157.4, 139.6, 0),
    "C17": (157.8, 130.0, 90), "C22": (159.0, 130.0, 90),
    "C18": (157.6, 126.4, 90), "C23": (158.8, 126.4, 90),
    "C6": (170.0, 121.0, 0),       # 3V3 bulk
    "U8": (183.0, 124.0, 0),       # AMS1117 3.3 V regulator
    "C29": (176.0, 122.0, 0),      # its output capacitor
    # Top edge: link, buttons, SWD, USB-C, power
    "J3": (127.0, 104.0, 90),      # link socket, pins along the edge, pin 1 on the left
    "R3": (128.27, 108.8, 90),
    "R4": (130.81, 108.8, 90),
    "D5": (136.0, 110.0, 180),     # link 5 V into VB, anode towards J3's pin 1
    "J6": (138.46, 104.0, 90),     # SWD, 1x3 0.1" header, pins along the edge
    "SW1": (165.0, 152.0, 0),      # BOOTSEL, bottom right, clear of the flash and the logo
    "SW2": (173.5, 152.0, 0),      # RESET
    "J1": (158.0, 103.64, 180),    # USB-C, mouth on the top edge (3.64 mm in, as on the HDD Synth Mk5)
    "U4": (158.0, 112.6, 90),      # ESD, turned so D+ and D- run straight through it to J1
    "R1": (161.4, 110.2, 0),       # CC1 pull-down, beside the CC1 pin
    "R2": (154.4, 110.2, 180),     # CC2 pull-down, beside the CC2 pin
    "D1": (169.5, 104.0, 0),
    "C1": (171.0, 110.5, 0),
    "J2": (177.0, 103.6, 0),       # 5 V in
    "F1": (178.5, 110.5, 0),
    "D2": (185.0, 110.5, 0),
    # LEDs, top right
    "R5": (174.0, 117.0, 0),
    "D3": (178.0, 117.0, 0),
    "R6": (183.0, 117.0, 0),
    "D4": (187.0, 117.0, 0),
    # microSD on the right edge, card in from the right
    "J4": (X0 + W - 9.69, 140.0, 90),  # TF PUSH: its front edge on the board edge
    "R7": (172.0, 126.5, 0),
    "R8": (172.0, 128.7, 0),
    "R9": (172.0, 130.9, 0),
    "R10": (172.0, 133.1, 0),
    "R11": (172.0, 135.3, 0),
    "R12": (172.0, 137.5, 0),
    "C3": (172.0, 140.0, 0),
    "C2": (172.0, 143.0, 0),
}
# Reference designators moved off pads and neighbours: ref -> (x, y), or None to hide.
REF_TEXT = {
    **{r: (169.0, PLACE[r][1]) for r in ("R7", "R8", "R9", "R10", "R11", "R12", "C3", "C2")},
    "R3": (128.27, 111.6), "R4": (130.81, 111.6), "U4": (161.9, 113.4), "J6": (146.6, 103.4), "J1": (164.6, 101.6),
    "R14": (138.6, 148.5), "R15": (138.6, 150.7),
    "J2": (183.6, 103.6), "F1": (178.5, 112.6), "R13": (126.3, 163.8), "JP1": (130.3, 163.8),
    **{u: (PLACE[u][0], PLACE[u][1]) for u in ("U2", "U3", "U5", "U6")},  # on the chip body
    **{c: (126.9, PLACE[c][1]) for c in ("C4", "C5", "C7", "C8")},
    "H1": None, "H2": None, "H3": None, "H4": None, "SW1": None, "SW2": None, "R20": None,
    "R18": None, "R19": None, "R1": (161.4, 111.9), "R2": (154.4, 111.9),
    # The small parts round the RP2350 have no room for labels; they're on the fab layer.
    **{r: None for r in ["L1", "R16", "R17", "R21", "R22"] + [f"C{n}" for n in range(9, 29) if n not in (12,)]},
    "C12": None,
}
# Raspberry Pi's core-regulator cluster (RP2350 minimal design), relative to the VREG_LX pad:
# ref -> (dx, dy, rotation). Same pin order and pitch on the RP2350B as on their RP2350A.
CORE = {
    "C9": (0.0, -1.16, 0),     # VREG_VIN 4.7 uF
    "C10": (0.0, -2.1, 0),     # 1.1 V output 4.7 uF
    "L1": (0.0, -3.75, 0),     # inductor, pad 1 (1.1 V) on the left: Raspberry Pi's polarity
    "C11": (2.2, -1.6, 270),   # VREG_AVDD 4.7 uF
    "R16": (2.2, -3.45, 270),  # VREG_AVDD 33R
    "R21": (-3.1, -3.95, 270), # USB D+ 27R
    "R22": (-2.1, -3.95, 270), # USB D- 27R
    # Raspberry Pi's flash corner: the same QSPI pins sit at the same place relative to LX.
    "U7": (-8.95, -9.27, 0),   # W25Q128JV
    "R18": (-9.0, -14.35, 270),  # CS pull-up, 10k
    "R19": (-13.1, -15.25, 0),   # QSPI_SS to BOOTSEL, 1k
    "C26": (-5.81, -13.72, 180),  # flash 100 nF
}
# Their small copper fills on F.Cu, relative to the VREG_LX pad: net -> (priority, outline).
CORE_ZONES = [
    ("+3V3", 7, [(-0.5, 0.75), (-0.5, -0.6), (-0.75, -0.85), (-0.75, -1.4), (-0.3, -1.4), (-0.3, 0.75)]),
    ("GND", 4, [(0.3, 0.3), (0.3, -1.4), (0.45, -1.4), (0.45, -1.85), (0.3, -1.85), (0.3, -2.35), (1.1, -2.35),
                (1.4, -2.05), (1.4, -1.3), (0.5, -0.4), (0.5, 0.3)]),
    ("VREG_LX", 5, [(-0.1, 0.3), (-0.1, -0.5), (-0.15, -0.55), (-0.15, -2.7), (0.35, -3.2), (0.35, -4.6), (1.05, -4.6),
                    (1.05, -2.9), (0.75, -2.6), (0.3, -2.6), (0.15, -2.45), (0.15, -0.55), (0.1, -0.5), (0.1, 0.3)]),
    ("+1V1", 6, [(-0.35, -1.85), (-0.35, -4.6), (-1.05, -4.6), (-1.05, -2.75), (-0.75, -2.45), (-0.75, -2.3),
                 (-0.95, -2.3), (-1.3, -2.65), (-1.5, -2.65), (-1.8, -2.35), (-1.8, -1.75), (-1.5, -1.45), (-1.0, -1.45),
                 (-1.0, -1.65), (-0.45, -1.65), (-0.45, -1.85)]),
]
# Their tracks, relative to the VREG_LX pad: net -> (width, points).
CORE_TRACKS = [
    ("VREG_AVDD", 0.15, [(0.8, 0.0), (0.8, -0.15), (1.64, -0.99), (1.64, -1.52), (2.2, -2.08), (2.2, -2.97)]),
    ("+1V1", 0.15, [(-0.8, 0.0), (-0.825, -0.025), (-0.825, -0.675), (-1.075, -0.925), (-1.075, -1.675)]),
    ("USB_DP_MCU", 0.15, [(-1.6, 0.0), (-1.6, -0.55), (-3.1, -2.05), (-3.1, -3.47)]),
    ("USB_DM_MCU", 0.15, [(-1.2, 0.0), (-1.225, -0.025), (-1.225, -0.651), (-2.1, -1.526), (-2.1, -3.47)]),
]
CORE_TRACKS += [
    # Raspberry Pi's QSPI fan-out. Their FLASH_SS and QSPI_SS are one net here (no 0R between them).
    # Their QSPI_SS route from the chip would clip the RP2350B's pin 76, which the RP2350A
    # doesn't have, so only its flash end is kept and the router takes it from the chip.
    ("QSPI_SS", 0.15, [(-13.61, -13.85), (-13.61, -15.25)]),
    ("QSPI_SS", 0.15, [(-12.55, -11.18), (-12.59, -11.22), (-12.59, -13.85), (-10.71, -13.85), (-10.7, -13.84),
                       (-9.0, -13.84)]),
    ("QSPI_SS", 0.15, [(-13.61, -13.85), (-12.59, -13.85)]),
    ("QSPI_SD1", 0.15, [(-4.4, 0.0), (-4.4, -0.41), (-5.94, -1.95), (-9.15, -1.95), (-14.0, -6.8), (-14.0, -8.95),
                        (-13.04, -9.91), (-12.55, -9.91)]),
    ("QSPI_SD2", 0.15, [(-4.0, 0.0), (-4.0, -0.45), (-10.4, -6.85), (-10.4, -7.95), (-11.09, -8.64), (-12.55, -8.64)]),
    ("QSPI_SD0", 0.15, [(-3.6, 0.0), (-3.6, -0.55), (-6.0, -2.95), (-6.0, -6.72), (-5.35, -7.37)]),
    ("QSPI_SCLK", 0.15, [(-3.2, 0.0), (-3.2, -0.65), (-5.6, -3.05), (-5.6, -5.35), (-4.2, -6.75), (-4.2, -8.05),
                         (-4.79, -8.64), (-5.35, -8.64)]),
    ("QSPI_SD3", 0.15, [(-2.8, 0.0), (-2.8, -0.75), (-5.2, -3.15), (-5.2, -5.05), (-3.8, -6.45), (-3.8, -8.95),
                        (-4.76, -9.91), (-5.35, -9.91)]),
    # VREG_AVDD capacitor's ground out to a via on its right; the 33R's 3V3 end up to a via
    ("GND", 0.2, [(2.2, -1.12), (2.85, -1.12)]),
    ("+3V3", 0.2, [(2.2, -3.93), (2.2, -4.7)]),
    # the flash's GND pin, boxed in by the QSPI fan-out, to its own via
    ("GND", 0.25, [(-12.55, -7.37), (-11.7, -6.45)]),
]
CORE_VIAS = [("+1V1", -1.5, -2.35), ("+1V1", -1.5, -1.75), ("GND", 1.1, -1.45), ("GND", 1.1, -2.05),
             ("GND", 2.85, -1.12), ("+3V3", 2.2, -4.7), ("GND", -11.7, -6.45)]
# The RP2350B's supply pins (pad centres, relative to the chip's centre) and the via under the
# chip each joins: (net, pads, via). Neighbouring pins on the same net share a via.
SUPPLY_VIAS = [
    ("+3V3", [(-4.95, -2.2)], (-3.7, -2.2)),               # IOVDD 5
    ("+1V1", [(-4.95, -0.2)], (-3.7, -0.2)),               # DVDD 10
    ("+3V3", [(-4.95, 1.8)], (-3.7, 1.8)),                 # IOVDD 15
    ("+3V3", [(-2.6, 4.95)], (-2.6, 3.7)),                 # IOVDD 24
    ("+3V3", [(-0.6, 4.95)], (-0.6, 3.7)),                 # IOVDD 29
    ("+1V1", [(0.6, 4.95)], (0.6, 3.7)),                   # DVDD 32
    ("+3V3", [(4.95, 3.8)], (3.7, 3.6)),                   # IOVDD 41
    ("+3V3", [(4.95, 0.2)], (3.7, 0.7)),                   # IOVDD 50
    ("+1V1", [(4.95, -0.2)], (3.7, -0.7)),                 # DVDD 51
    ("+3V3", [(4.95, -3.4), (4.95, -3.8)], (3.7, -3.6)),   # ADC_AVDD 59, IOVDD 60
    ("+3V3", [(2.6, -4.95)], (2.6, -3.7)),                 # VREG_VIN 64
    ("+3V3", [(1.0, -4.95), (0.6, -4.95)], (0.8, -3.7)),   # USB_OTP_VDD 68, QSPI_IOVDD 69
    ("+3V3", [(-2.2, -4.95)], (-2.2, -3.7)),               # IOVDD 76
]
# 1V1 on B.Cu under the RP2350B (chip-relative): from Raspberry Pi's two 1V1 vias (LX-relative
# (-1.5, -2.35) and (-1.5, -1.75)) between the 3V3 vias to the DVDD 51, 10 and 32 vias.
CORE_1V1_BOTTOM = [
    [(1.5, -7.3), (1.5, -6.7), (1.7, -6.5), (1.7, -3.0), (3.7, -1.0), (3.7, -0.7)],   # to DVDD 51
    [(1.7, -3.0), (-2.0, -3.0), (-3.0, -2.0), (-3.0, -0.2), (-3.7, -0.2)],            # to DVDD 10
    [(-3.0, -0.2), (-3.0, 2.6), (-0.2, 2.6), (0.6, 3.4), (0.6, 3.7)],                 # to DVDD 32
]
# USB, board coordinates: from R21/R22 (pad 1) straight through U4 (pins 1-6 D+, 3-4 D-) to the
# connector's outer D+/D- pads (B6, B7), and the inner pads (A6, A7), which sit swapped between
# them, by a stub and via each and a hop across on the bottom layer. (net, layer, points)
USB_TRACKS = [
    ("USB_DP", "F.Cu", [(150.9, 122.625), (150.9, 121.9), (155.0, 117.8), (157.05, 115.75), (157.05, 109.0),
                        (157.25, 108.8), (157.25, 107.685)]),
    ("USB_DM", "F.Cu", [(151.9, 122.625), (151.9, 121.9), (156.0, 117.8), (158.95, 114.85), (158.95, 109.0),
                        (158.75, 108.8), (158.75, 107.685)]),
    ("USB_DM", "F.Cu", [(157.75, 107.685), (157.75, 109.15)]),   # A7
    ("USB_DP", "F.Cu", [(158.25, 107.685), (158.25, 110.0)]),    # A6
    ("USB_DM", "B.Cu", [(157.75, 109.15), (158.95, 109.6)]),
    ("USB_DP", "B.Cu", [(158.25, 110.0), (157.05, 110.3)]),
    # VBUS: the right-hand pads (A4/B9) under R1 to D1's anode; the left-hand pads (A9/B4) over on
    # the bottom layer, above the D+/D- hops; U4's VBUS pin down a via and round underneath.
    # (the connector's pads reach down to y 108.41, so the run below them is at 108.95)
    ("VBUS_USB", "F.Cu", [(160.45, 107.685), (160.45, 108.95), (171.5, 108.95), (171.5, 104.0)]),
    ("VBUS_USB", "F.Cu", [(155.55, 107.685), (155.55, 108.95)]),
    ("VBUS_USB", "B.Cu", [(155.55, 108.95), (156.0, 108.5), (160.0, 108.5), (160.45, 108.95)]),
    ("VBUS_USB", "F.Cu", [(158.0, 111.46), (158.0, 110.6)]),
    ("VBUS_USB", "B.Cu", [(158.0, 110.6), (159.6, 112.2), (163.6, 112.2), (163.6, 108.95)]),
    ("GND", "F.Cu", [(158.0, 113.74), (158.0, 114.75)]),   # U4's GND, boxed in by D+ and D-
]
USB_VIAS = [("USB_DM", 157.75, 109.15), ("USB_DM", 158.95, 109.6), ("USB_DP", 158.25, 110.0), ("USB_DP", 157.05, 110.3),
            ("VBUS_USB", 155.55, 108.95), ("VBUS_USB", 160.45, 108.95), ("VBUS_USB", 158.0, 110.6),
            ("VBUS_USB", 163.6, 108.95), ("GND", 158.0, 114.75)]
# The Mk5's M3 holes, relative to its top left corner: 9.76 mm in from the left, 13.97 from the right.
HOLES = [(X0 + 9.76, Y0 + 4.09), (X0 + 86.36, Y0 + 4.09), (X0 + 9.76, Y0 + 65.81), (X0 + 86.36, Y0 + 65.81)]
LOGO = (151.0, 161.2)  # centre of LOGO_HDDSynthPro (19.4 x 16 mm, from tools/make_logo.py)
SILK = [  # text, x, y, size, rotation
    ("RP2350B IDE drive  rev 0.3", 172.0, 160.0, 1.0, 0),
    ("IDE 40-pin", 111.0, 135.0, 1.0, 90),
    ("5V", 127.0, 101.3, 0.8, 0), ("GND", 129.54, 101.3, 0.8, 0), ("TX", 132.08, 101.3, 0.8, 0),
    ("RX", 134.62, 101.3, 0.8, 0),
    ("+5V GND", 178.25, 108.5, 0.8, 0),
    ("CLK", 138.46, 101.3, 0.8, 0), ("GND", 141.0, 101.3, 0.8, 0), ("DIO", 143.54, 101.3, 0.8, 0),
    ("BOOT", 165.0, 156.0, 0.8, 0),
    ("RST", 173.5, 156.0, 0.8, 0),
    ("JP1: close for firmware\nwithout BUS_EN#", 172.0, 164.5, 0.8, 0),
]
# Net-name patterns for the Power class (local nets carry their sheet path).
POWER_PATTERNS = ["VB", "5V_LINK", "*/VBUS_USB", "*/5V_EXT", "*/5V_EXT_IN"]  # GND and +3V3 are planes


def mm(v):
    return pcbnew.FromMM(v)


def netlist():
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "board.net"
        subprocess.run(["kicad-cli", "sch", "export", "netlist", "--format", "kicadsexpr", "-o", str(out),
                        str(HW / "hddsynthpro.kicad_sch")], check=True, capture_output=True)
        root = parse(out.read_text())
    comps = {}
    for c in find(first(root, "components"), "comp"):
        ref = first(c, "ref")[1]
        sheet = first(first(c, "sheetpath"), "tstamps")[1]
        tstamps = first(c, "tstamps")
        fields = {f[1][1]: (f[2] if len(f) > 2 else "") for f in find(first(c, "fields"), "field")}
        fields.pop("Footprint", None)
        props = {first(p, "name")[1] for p in find(c, "property")}
        comps[ref] = {"value": first(c, "value")[1], "footprint": first(c, "footprint")[1],
                      "path": sheet + tstamps[1], "fields": fields, "in_bom": "exclude_from_bom" not in props}
    nets = {}
    for n in find(first(root, "nets"), "net"):
        name = first(n, "name")[1]
        nets[name] = [(first(x, "ref")[1], first(x, "pin")[1]) for x in find(n, "node")]
    return comps, nets


def load_footprint(fpid):
    lib, name = fpid.split(":")
    path = HW / f"{lib}.pretty" if lib == "HDDSynthPro" else KICAD_SHARE / "footprints" / f"{lib}.pretty"
    fp = pcbnew.FootprintLoad(str(path), name)
    if fp is None:
        raise FileNotFoundError(fpid)
    fp.SetFPID(pcbnew.LIB_ID(lib, name))
    return fp


def outline(board):
    """A rectangle with rounded corners: four edges and four quarter-circle arcs."""
    r, x1, y1 = CORNER_R, X0 + W, Y0 + H
    k = r * (1 - 0.5 ** 0.5)  # from a corner to its arc's midpoint, along each axis

    def shape(kind, *pts):
        s = pcbnew.PCB_SHAPE(board)
        s.SetShape(kind)
        v = [pcbnew.VECTOR2I(mm(x), mm(y)) for x, y in pts]
        if kind == pcbnew.SHAPE_T_ARC:
            s.SetArcGeometry(*v)
        else:
            s.SetStart(v[0])
            s.SetEnd(v[1])
        s.SetLayer(pcbnew.Edge_Cuts)
        s.SetWidth(mm(0.1))
        board.Add(s)

    shape(pcbnew.SHAPE_T_SEGMENT, (X0 + r, Y0), (x1 - r, Y0))
    shape(pcbnew.SHAPE_T_SEGMENT, (x1, Y0 + r), (x1, y1 - r))
    shape(pcbnew.SHAPE_T_SEGMENT, (x1 - r, y1), (X0 + r, y1))
    shape(pcbnew.SHAPE_T_SEGMENT, (X0, y1 - r), (X0, Y0 + r))
    shape(pcbnew.SHAPE_T_ARC, (x1 - r, Y0), (x1 - k, Y0 + k), (x1, Y0 + r))
    shape(pcbnew.SHAPE_T_ARC, (x1, y1 - r), (x1 - k, y1 - k), (x1 - r, y1))
    shape(pcbnew.SHAPE_T_ARC, (X0 + r, y1), (X0 + k, y1 - k), (X0, y1 - r))
    shape(pcbnew.SHAPE_T_ARC, (X0, Y0 + r), (X0 + k, Y0 + k), (X0 + r, Y0))


def plane(board, net, layer, name):
    z = pcbnew.ZONE(board)
    z.SetLayer(layer)
    z.SetNet(net)
    z.SetZoneName(name)
    z.SetLocalClearance(mm(0.3))
    z.SetMinThickness(mm(0.2))
    z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
    ol = z.Outline()
    ol.NewOutline()
    m = 0.4
    for x, y in [(X0 + m, Y0 + m), (X0 + W - m, Y0 + m), (X0 + W - m, Y0 + H - m), (X0 + m, Y0 + H - m)]:
        ol.Append(mm(x), mm(y))
    board.Add(z)
    return z


def design_rules(project_file):
    """Net classes and limits for a 4-layer board at JLCPCB-class fabs, with margin.

    KiCad keeps these in the project file, not the board, so they're written there.
    """
    pro = json.loads(project_file.read_text()) if project_file.exists() else {}
    base = {"bus_width": 12, "diff_pair_gap": 0.25, "diff_pair_via_gap": 0.25, "diff_pair_width": 0.2,
            "line_style": 0, "microvia_diameter": 0.3, "microvia_drill": 0.1, "pcb_color": "rgba(0, 0, 0, 0.000)",
            "schematic_color": "rgba(0, 0, 0, 0.000)", "tuning_profile": "", "wire_width": 6}
    pro["net_settings"] = {
        "classes": [
            {**base, "name": "Default", "priority": 2147483647, "clearance": 0.15, "track_width": 0.2,
             "via_diameter": 0.6, "via_drill": 0.3},
            {**base, "name": "Power", "priority": 0, "clearance": 0.15, "track_width": 0.6,
             "via_diameter": 0.8, "via_drill": 0.4},
        ],
        "meta": {"version": 5}, "net_colors": None, "netclass_assignments": None,
        "netclass_patterns": [{"netclass": "Power", "pattern": p} for p in POWER_PATTERNS],
    }
    rules = pro.setdefault("board", {}).setdefault("design_settings", {}).setdefault("rules", {})
    rules.update({"min_clearance": 0.12, "min_track_width": 0.1, "min_via_diameter": 0.5,
                  "min_via_annular_width": 0.1, "min_through_hole_diameter": 0.2, "min_hole_to_hole": 0.25,
                  "min_copper_edge_clearance": 0.3,
                  "min_resolved_spokes": 1})  # THT GND pins also join the inner plane
    project_file.write_text(json.dumps(pro, indent=2) + "\n")


def lx_pad():
    """Where the RP2350B's VREG_LX pin (63) lands: (3.0, -4.95) from the QFN-80's centre, unrotated."""
    x, y, rot = PLACE["U1"]
    if rot != 0:
        raise ValueError("core_copper() assumes the RP2350B unrotated")
    return x + 3.0, y - 4.95


def core_copper(board):
    """Raspberry Pi's regulator copper, and a 3V3 ring under the RP2350B joining its supply pins.

    The ring is Raspberry Pi's approach too: between the pad ring and the exposed pad, so every
    3V3 pin (and the VREG_VIN fill) joins it on the top layer; four vias take it to the 3V3 plane.
    """
    def net(name):
        for n, item in board.GetNetsByName().items():
            if str(n) == name or str(n).endswith("/" + name):
                return item
        raise KeyError(name)

    lx, ly = lx_pad()

    def zone(netname, prio, pts, layer=pcbnew.F_Cu):
        z = pcbnew.ZONE(board)
        z.SetLayer(layer)
        z.SetNet(net(netname))
        z.SetAssignedPriority(prio)
        z.SetLocalClearance(mm(0.15))
        z.SetMinThickness(mm(0.12))
        z.SetPadConnection(pcbnew.ZONE_CONNECTION_FULL)
        z.SetLocked(True)
        ol = z.Outline()
        ol.NewOutline()
        for x, y in pts:
            ol.Append(mm(x), mm(y))
        board.Add(z)

    def via(netname, x, y):
        v = pcbnew.PCB_VIA(board)
        v.SetPosition(pcbnew.VECTOR2I_MM(x, y))
        v.SetWidth(mm(0.5))
        v.SetDrill(mm(0.3))
        v.SetNet(net(netname))
        v.SetLocked(True)
        board.Add(v)

    # Raspberry Pi's cluster is laid out to 0.125 mm in places; hardware/hddsynthpro.kicad_dru
    # allows that inside this area only.
    area = pcbnew.ZONE(board)
    area.SetIsRuleArea(True)
    area.SetLayer(pcbnew.F_Cu)
    for flag in ("Tracks", "Vias", "Pads", "Footprints", "ZoneFills"):
        getattr(area, f"SetDoNotAllow{flag}")(False)
    area.SetZoneName("RP2350 core")
    ol = area.Outline()
    ol.NewOutline()
    for dx, dy in [(-3.8, -4.8), (2.8, -4.8), (2.8, 0.5), (-3.8, 0.5)]:
        ol.Append(mm(lx + dx), mm(ly + dy))
    board.Add(area)

    for netname, prio, pts in CORE_ZONES:
        zone(netname, prio, [(lx + dx, ly + dy) for dx, dy in pts])
    for netname, width, pts in CORE_TRACKS:
        for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
            tr = pcbnew.PCB_TRACK(board)
            tr.SetStart(pcbnew.VECTOR2I_MM(lx + x0, ly + y0))
            tr.SetEnd(pcbnew.VECTOR2I_MM(lx + x1, ly + y1))
            tr.SetWidth(mm(width))
            tr.SetLayer(pcbnew.F_Cu)
            tr.SetNet(net(netname))
            tr.SetLocked(True)
            board.Add(tr)
    for netname, dx, dy in CORE_VIAS:
        via(netname, lx + dx, ly + dy)

    # Each supply pin: a short stub inward, under the chip, to its own via (3V3 drops to the
    # 3V3 plane; 1V1 to the bottom layer, where the router joins the DVDD pins to the regulator,
    # as Raspberry Pi do). The outward lanes stay free for signals.
    cx, cy, _ = PLACE["U1"]

    def track(netname, pts, width=0.2):
        for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
            tr = pcbnew.PCB_TRACK(board)
            tr.SetStart(pcbnew.VECTOR2I_MM(cx + x0, cy + y0))
            tr.SetEnd(pcbnew.VECTOR2I_MM(cx + x1, cy + y1))
            tr.SetWidth(mm(width))
            tr.SetLayer(pcbnew.F_Cu)
            tr.SetNet(net(netname))
            tr.SetLocked(True)
            board.Add(tr)

    # 1V1 on the bottom layer, under the chip: Raspberry Pi's 1V1 vias to the three DVDD vias.
    for pts in CORE_1V1_BOTTOM:
        for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
            tr = pcbnew.PCB_TRACK(board)
            tr.SetStart(pcbnew.VECTOR2I_MM(cx + x0, cy + y0))
            tr.SetEnd(pcbnew.VECTOR2I_MM(cx + x1, cy + y1))
            tr.SetWidth(mm(0.4))
            tr.SetLayer(pcbnew.B_Cu)
            tr.SetNet(net("+1V1"))
            tr.SetLocked(True)
            board.Add(tr)
    # USB D+/D- from the 27R resistors to U4's inputs (board coordinates), laid before the
    # router so nothing else takes the space.
    for netname, layer, pts in USB_TRACKS:
        for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
            tr = pcbnew.PCB_TRACK(board)
            tr.SetStart(pcbnew.VECTOR2I_MM(x0, y0))
            tr.SetEnd(pcbnew.VECTOR2I_MM(x1, y1))
            tr.SetWidth(mm(0.5 if netname == "VBUS_USB" and (x0, y0) != (158.0, 111.46) else 0.2))
            tr.SetLayer(pcbnew.F_Cu if layer == "F.Cu" else pcbnew.B_Cu)
            tr.SetNet(net(netname))
            tr.SetLocked(True)
            board.Add(tr)
    for netname, x, y in USB_VIAS:
        via(netname, x, y)

    for netname, pads, (vx, vy) in SUPPLY_VIAS:
        for px, py in pads:
            # straight in past the pad's inner end (4.55 from centre), then across to the via
            k = 4.3 / 4.95
            track(netname, [(px, py), (px * k if abs(px) > 4 else px, py * k if abs(py) > 4 else py), (vx, vy)])
        v = pcbnew.PCB_VIA(board)
        v.SetPosition(pcbnew.VECTOR2I_MM(cx + vx, cy + vy))
        v.SetWidth(mm(0.5))
        v.SetDrill(mm(0.3))
        v.SetNet(net(netname))
        v.SetLocked(True)
        board.Add(v)


def silkscreen(board, logo):
    """The board's own silkscreen: the logo and the labels. Replaces any already there.

    `logo` is the loaded LOGO_HDDSynthPro footprint: load it before LoadBoard(), as
    FootprintLoad() fails in KiCad 10's bindings once a board has been loaded.
    """
    for d in list(board.GetDrawings()):
        if d.GetClass() == "PCB_TEXT" and d.GetLayer() == pcbnew.F_SilkS:
            board.Remove(d)
    old = board.FindFootprintByReference("LOGO1")
    if old:
        board.Remove(old)
    logo.SetReference("LOGO1")
    logo.SetPosition(pcbnew.VECTOR2I(mm(LOGO[0]), mm(LOGO[1])))
    board.Add(logo)
    for text, x, y, size, rot in SILK:
        t = pcbnew.PCB_TEXT(board)
        t.SetText(text)
        t.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y)))
        t.SetTextAngleDegrees(rot)
        t.SetLayer(pcbnew.F_SilkS)
        t.SetTextSize(pcbnew.VECTOR2I(mm(size), mm(size)))
        t.SetTextThickness(mm(size * 0.15))
        board.Add(t)


def main():
    if len(sys.argv) > 2 and sys.argv[1] == "--fields":
        # Copy the schematic's fields (LCSC, FT offsets, ...) onto an existing (routed) board.
        comps, _ = netlist()
        board = pcbnew.LoadBoard(sys.argv[2])
        for ref, c in comps.items():
            fp = board.FindFootprintByReference(ref)
            if fp is None:
                continue
            for name, text in c["fields"].items():
                fp.SetField(name, text)
                fp.GetField(name).SetVisible(False)
        board.Save(sys.argv[2])
        print(f"fields updated on {sys.argv[2]}")
        return
    if len(sys.argv) > 2 and sys.argv[1] == "--silkscreen":
        # Redo only the logo and labels on an existing (routed) board.
        logo = load_footprint("HDDSynthPro:LOGO_HDDSynthPro")
        board = pcbnew.LoadBoard(sys.argv[2])
        silkscreen(board, logo)
        board.Save(sys.argv[2])
        print(f"silkscreen updated on {sys.argv[2]}")
        return
    comps, nets = netlist()
    lx, ly = lx_pad()
    PLACE.update({ref: (lx + dx, ly + dy, rot) for ref, (dx, dy, rot) in CORE.items()})
    missing = set(comps) - set(PLACE)
    if missing:
        raise KeyError(f"no position for {sorted(missing)}")

    board = pcbnew.CreateEmptyBoard()
    board.SetCopperLayerCount(4)
    board.SetLayerName(pcbnew.In1_Cu, "GND")
    board.SetLayerName(pcbnew.In2_Cu, "3V3")
    outline(board)

    net_items = {}
    for name in nets:
        item = pcbnew.NETINFO_ITEM(board, name)
        board.Add(item)
        net_items[name] = item
    pad_net = {key: name for name, nodes in nets.items() for key in nodes}

    for ref, c in sorted(comps.items()):
        fp = load_footprint(c["footprint"])
        fp.SetReference(ref)
        fp.SetValue(c["value"])
        fp.SetPath(pcbnew.KIID_PATH(c["path"]))
        for name, text in c["fields"].items():  # as "Update PCB from Schematic" would
            fp.SetField(name, text)
            fp.GetField(name).SetVisible(False)
        fp.SetExcludedFromBOM(not c["in_bom"])
        x, y, rot = PLACE[ref]
        fp.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y)))
        fp.SetOrientationDegrees(rot)
        board.Add(fp)
        for pad in fp.Pads():
            name = pad_net.get((ref, pad.GetNumber()))
            if name:
                pad.SetNet(net_items[name])

    for i, (x, y) in enumerate(HOLES, start=1):
        fp = load_footprint("MountingHole:MountingHole_3.2mm_M3")
        fp.SetReference(f"H{i}")
        fp.SetValue("M3")
        fp.SetBoardOnly(True)
        fp.SetExcludedFromBOM(True)
        fp.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y)))
        board.Add(fp)

    for fp in board.GetFootprints():
        ref = fp.GetReference()
        if ref in REF_TEXT and REF_TEXT[ref] is None:
            fp.Reference().SetVisible(False)
        elif ref in REF_TEXT:
            fp.Reference().SetPosition(pcbnew.VECTOR2I(mm(REF_TEXT[ref][0]), mm(REF_TEXT[ref][1])))
            fp.Reference().SetTextAngleDegrees(0)

    silkscreen(board, load_footprint("HDDSynthPro:LOGO_HDDSynthPro"))

    # The zone filler crashes outside the GUI; fill the planes with kicad-cli (see the docstring).
    core_copper(board)
    plane(board, net_items["GND"], pcbnew.In1_Cu, "GND plane")
    plane(board, net_items["+3V3"], pcbnew.In2_Cu, "3V3 plane")

    out = Path(sys.argv[1]) if len(sys.argv) > 1 else OUT
    board.Save(str(out))
    design_rules(out.with_suffix(".kicad_pro"))
    print(f"wrote {out}: {len(comps)} parts, {len(nets)} nets, {len(HOLES)} holes")


if __name__ == "__main__":
    main()
