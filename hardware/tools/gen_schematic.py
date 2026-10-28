#!/usr/bin/env python3
"""Generates the first version of the HDDSynthPro KiCad project.

This bootstraps the schematic from the agreed design (docs/pcb.html). Once the
schematic has been edited in KiCad, the .kicad_sch files are the source of
truth: rerunning this overwrites them. check_netlist.py checks either against
src/config.h.

Parts are connected by net labels at the end of a short wire on every pin, not
by drawn wires: labels with the same name on a sheet are one net, and global
labels join nets across sheets. Every symbol except the PGA2350 is copied from
KiCad's own libraries, so pin numbers match KiCad's footprints.

Usage: python3 hardware/tools/gen_schematic.py   (needs KiCad's libraries;
set KICAD_SHARE if they aren't in /Applications/KiCad)
"""
import json
import os
import uuid
from pathlib import Path

from sexpr import Sym as S, dump, find, first, parse

HERE = Path(__file__).resolve().parent
HW = HERE.parent
KICAD_SHARE = Path(os.environ.get("KICAD_SHARE", "/Applications/KiCad/KiCad.app/Contents/SharedSupport"))
PROJECT = "hddsynthpro"
SCH_VERSION = 20250114  # KiCad 9 format; KiCad 10 opens it
DATE = "2026-10-27"
REV = "0.3"
STUB = 2.54
POWER_NETS = {"GND": "power:GND", "+3V3": "power:+3V3"}


def uid(*parts):
    """A stable UUID, so regenerating doesn't churn every line of the files."""
    return str(uuid.uuid5(uuid.NAMESPACE_URL, "hddsynthpro/" + "/".join(parts)))


def font():
    return [S("font"), [S("size"), 1.27, 1.27]]


def effects(justify=None, hide=False):
    e = [S("effects"), font()]
    if justify:
        e.append([S("justify")] + [S(j) for j in justify.split()])
    if hide:
        e.append([S("hide"), S("yes")])
    return e


def at(x, y, a=0):
    return [S("at"), round(x, 4), round(y, 4), a]


# ---------------------------------------------------------------- symbols

_libs = {}


def _lib(lib):
    if lib not in _libs:
        text = (KICAD_SHARE / "symbols" / f"{lib}.kicad_sym").read_text()
        _libs[lib] = {s[1]: s for s in find(parse(text), "symbol")}
    return _libs[lib]


def _flatten(lib, name):
    """A library symbol with any `extends` resolved, as KiCad embeds it."""
    s = _lib(lib)[name]
    ext = first(s, "extends")
    if not ext:
        return s
    parent = _flatten(lib, ext[1])
    child_props = {p[1]: p for p in find(s, "property")}
    child_heads = {x[0]: x for x in s[2:] if isinstance(x, list) and x[0] not in ("property", "extends", "symbol")}
    heads, props, units, tail = [], [], [], []
    for item in parent[2:]:
        if not isinstance(item, list):
            heads.append(item)
        elif item[0] == "property":
            props.append(child_props.pop(item[1], item))
        elif item[0] == "symbol":
            units.append([S("symbol"), name + item[1][len(parent[1]):]] + item[2:])
        elif item[0] == "embedded_fonts":
            tail.append(item)
        else:
            heads.append(child_heads.pop(item[0], item))
    props += child_props.values()
    heads += child_heads.values()
    return [S("symbol"), name] + heads + props + units + tail


def pga2350_symbol():
    """The Pimoroni PGA2350. Pad names (A1-J10) from Pimoroni's schematic.

    Left: power, control, USB, SWD, GP0-3 and GP32-47. Right: GP4-31, the IDE
    side. Bottom: the six ground pads.
    """
    pins = []

    def pin(kind, name, num, x, y, angle):
        pins.append([S("pin"), S(kind), S("line"), at(x, y, angle), [S("length"), 2.54],
                     [S("name"), name, effects()], [S("number"), num, effects()]])

    left = [("power_in", "VBUS", "A3"), ("power_out", "3V3", "B4"), ("input", "3V3_EN", "A4"),
            ("input", "RUN", "A5"), ("input", "BOOTSEL", "A8"), ("passive", "ADC_VREF", "B5"), None,
            ("bidirectional", "USB_DP", "A6"), ("bidirectional", "USB_DM", "A7"), None,
            ("input", "SWCLK", "B6"), ("bidirectional", "SWDIO", "B7"), None,
            ("bidirectional", "GP0", "B3"), ("bidirectional", "GP1", "A2"),
            ("bidirectional", "GP2", "B1"), ("bidirectional", "GP3", "C2"), None]
    gp_pad = PGA_GPIO_PADS
    left += [("bidirectional", f"GP{g}", gp_pad[g]) for g in range(32, 48)]
    y = 38.1
    for p in left:
        if p:
            pin(p[0], p[1], p[2], -17.78, y, 0)
        y -= 2.54
    y = 38.1
    for g in range(4, 32):
        pin("bidirectional", f"GP{g}", gp_pad[g], 17.78, y, 180)
        y -= 2.54
    for i, pad in enumerate(PGA_GND_PADS):
        pin("power_in", "GND", pad, -6.35 + 2.54 * i, -53.34, 90)

    body = [S("symbol"), "PGA2350_0_1",
            [S("rectangle"), [S("start"), -15.24, 40.64], [S("end"), 15.24, -50.8],
             [S("stroke"), [S("width"), 0.254], [S("type"), S("default")]], [S("fill"), [S("type"), S("background")]]]]
    return [S("symbol"), "PGA2350",
            [S("pin_names"), [S("offset"), 1.016]],
            [S("exclude_from_sim"), S("no")], [S("in_bom"), S("yes")], [S("on_board"), S("yes")],
            [S("property"), "Reference", "U", at(-15.24, 41.91), effects("left bottom")],
            [S("property"), "Value", "PGA2350", at(15.24, 41.91), effects("right bottom")],
            [S("property"), "Footprint", "HDDSynthPro:PGA2350", at(0, 0), effects(hide=True)],
            [S("property"), "Datasheet", "https://shop.pimoroni.com/products/pga2350", at(0, 0), effects(hide=True)],
            [S("property"), "Description", "Pimoroni PGA2350 (PIM722): RP2350B, 16 MB flash, 8 MB PSRAM, "
             "3.3 V LDO (VBUS 3.0-5.5 V), 64-pin 2.54 mm PGA", at(0, 0), effects(hide=True)],
            [S("property"), "ki_keywords", "RP2350 RP2350B Pimoroni PGA", at(0, 0), effects(hide=True)],
            body, [S("symbol"), "PGA2350_1_1"] + pins,
            [S("embedded_fonts"), S("no")]]


def cb3t3245_symbol():
    """TI SN74CB3T3245: 8-bit FET bus switch, 5 V-tolerant I/O, Ioff. Pinout from TI's
    datasheet (SCDS136E), TSSOP-20 (PW): 1 NC, 2-9 A1-A8, 10 GND, 11-18 B8-B1, 19 OE#, 20 VCC.
    """
    pins = []

    def pin(kind, name, num, x, y, angle, hide=False):
        p = [S("pin"), S(kind), S("line"), at(x, y, angle), [S("length"), 2.54]]
        if hide:
            p.append([S("hide"), S("yes")])
        p += [[S("name"), name, effects()], [S("number"), num, effects()]]
        pins.append(p)

    for k in range(1, 9):
        y = 8.89 - 2.54 * (k - 1)
        pin("passive", f"A{k}", str(k + 1), -10.16, y, 0)
        pin("passive", f"B{k}", str(19 - k), 10.16, y, 180)
    pin("input", "~{OE}", "19", -10.16, -12.7, 0)
    pin("power_in", "VCC", "20", 0, 15.24, 270)
    pin("power_in", "GND", "10", 0, -17.78, 90)
    pin("no_connect", "NC", "1", 7.62, -12.7, 180, hide=True)
    body = [S("symbol"), "SN74CB3T3245_0_1",
            [S("rectangle"), [S("start"), -7.62, 12.7], [S("end"), 7.62, -15.24],
             [S("stroke"), [S("width"), 0.254], [S("type"), S("default")]], [S("fill"), [S("type"), S("background")]]]]
    return [S("symbol"), "SN74CB3T3245",
            [S("pin_names"), [S("offset"), 1.016]],
            [S("exclude_from_sim"), S("no")], [S("in_bom"), S("yes")], [S("on_board"), S("yes")],
            [S("property"), "Reference", "U", at(-7.62, 13.97), effects("left bottom")],
            [S("property"), "Value", "SN74CB3T3245", at(7.62, 13.97), effects("right bottom")],
            [S("property"), "Footprint", "Package_SO:TSSOP-20_4.4x6.5mm_P0.65mm", at(0, 0), effects(hide=True)],
            [S("property"), "Datasheet", "https://www.ti.com/lit/ds/symlink/sn74cb3t3245.pdf", at(0, 0), effects(hide=True)],
            [S("property"), "Description", "8-bit FET bus switch, 2.5/3.3 V, 5 V-tolerant level shifter, "
             "Ioff, OE# active low", at(0, 0), effects(hide=True)],
            [S("property"), "ki_keywords", "bus switch level shifter CB3T", at(0, 0), effects(hide=True)],
            body, [S("symbol"), "SN74CB3T3245_1_1"] + pins,
            [S("embedded_fonts"), S("no")]]


# GPIO -> PGA2350 pad, from Pimoroni's schematic (U$1). Top view: row A along
# the top edge, column 1 down the left edge.
PGA_GPIO_PADS = {
    0: "B3", 1: "A2", 2: "B1", 3: "C2", 4: "C1", 5: "D2", 6: "D1", 7: "E2", 8: "E1", 9: "F2",
    10: "F1", 11: "G2", 12: "G1", 13: "H2", 14: "H1", 15: "I2", 16: "I1", 17: "J2", 18: "J3", 19: "I3",
    20: "J4", 21: "I4", 22: "J5", 23: "I5", 24: "J6", 25: "I6", 26: "J7", 27: "I7", 28: "J8", 29: "I8",
    30: "J9", 31: "I9", 32: "I10", 33: "H9", 34: "H10", 35: "G9", 36: "G10", 37: "F9", 38: "F10", 39: "E9",
    40: "E10", 41: "D9", 42: "D10", 43: "C9", 44: "C10", 45: "B10", 46: "A9", 47: "B8",
}
# The schematic shows six GND pins; these are the six pads left over.
PGA_GND_PADS = ["A1", "A10", "B2", "B9", "J1", "J10"]


def _kicad9_property(prop):
    """KiCad 10's libraries put (hide yes) on the property; the KiCad 9 format wants it in effects."""
    hidden = any(isinstance(x, list) and x[:2] == ["hide", "yes"] for x in prop)
    out = [x for x in prop if not (isinstance(x, list) and x and x[0] in ("hide", "show_name", "do_not_autoplace"))]
    if hidden:
        eff = first(out, "effects")
        if not any(isinstance(x, list) and x and x[0] == "hide" for x in eff):
            eff.append([S("hide"), S("yes")])
    return out


def is_hidden(prop):
    eff = first(prop, "effects") or []
    return any(isinstance(x, list) and x[:2] == ["hide", "yes"] for x in list(prop) + list(eff))


def symbol_def(lib_id):
    if lib_id == "HDDSynthPro:PGA2350":
        return pga2350_symbol()
    if lib_id == "HDDSynthPro:SN74CB3T3245":
        return cb3t3245_symbol()
    lib, name = lib_id.split(":")
    sym = _flatten(lib, name)
    return [_kicad9_property(x) if isinstance(x, list) and x and x[0] == "property" else x for x in sym]


def symbol_pins(sym):
    """[(number, x, y, angle, type)] for unit 1, body style 1."""
    out = []
    for unit in find(sym, "symbol"):
        u, b = unit[1].rsplit("_", 2)[-2:]
        if u not in ("0", "1") or b not in ("0", "1"):
            continue
        for p in find(unit, "pin"):
            a = first(p, "at")
            out.append((first(p, "number")[1], float(a[1]), float(a[2]), int(float(a[3])), str(p[1])))
    return out


# ---------------------------------------------------------------- sheets

def rotate(x, y, rot):
    """Rotate a screen offset (y down) counterclockwise on screen, as KiCad rotates symbols."""
    for _ in range(rot // 90 % 4):
        x, y = y, -x
    return x, y


OUTWARD = {0: (-1, 0), 180: (1, 0), 90: (0, 1), 270: (0, -1)}  # lib pin angle -> schematic direction
LABEL_ANGLE = {(-1, 0): 180, (1, 0): 0, (0, -1): 90, (0, 1): 270}


class Project:
    def __init__(self):
        self.root_uuid = uid("root")
        self.sheets = []
        self.pwr = 0
        self.flg = 0

    def sheet(self, name, file, page):
        sh = Sheet(self, name, file, page)
        self.sheets.append(sh)
        return sh


class Sheet:
    def __init__(self, project, name, file, page):
        self.p = project
        self.name, self.file, self.page = name, file, page
        self.sym_uuid = uid("sheet", file)
        self.path = f"/{project.root_uuid}/{self.sym_uuid}"
        self.items = []
        self.labels = []  # (net, x, y, angle)
        self.points = {}  # connection point -> net
        self.lib_ids = set()

    # --- low level
    def _claim(self, x, y, net):
        key = (round(x, 3), round(y, 3))
        prev = self.points.get(key)
        if prev is not None and prev != net:
            raise ValueError(f"{self.file}: {key} is both {prev} and {net}")
        self.points[key] = net

    def _symbol(self, lib_id, ref, value, x, y, rot=0, footprint=None, in_bom=True, fields=()):
        sym = symbol_def(lib_id)
        self.lib_ids.add(lib_id)
        props = {p[1]: p for p in find(sym, "property")}
        node = [S("symbol"), [S("lib_id"), lib_id], at(x, y, rot), [S("unit"), 1],
                [S("exclude_from_sim"), S("no")], [S("in_bom"), S("yes" if in_bom else "no")],
                [S("on_board"), S("yes")], [S("dnp"), S("no")], [S("uuid"), uid(self.file, ref)]]
        values = {"Reference": ref, "Value": value}
        if footprint is not None:
            values["Footprint"] = footprint
        for key in ("Reference", "Value", "Footprint", "Datasheet", "Description"):
            lp = props.get(key)
            text = values.get(key, lp[2] if lp else "")
            if lp:
                la = first(lp, "at")
                px, py = rotate(float(la[1]), -float(la[2]), rot)
                pa = int(float(la[3]))
                eff = [x for x in first(lp, "effects") if not (isinstance(x, list) and x and x[0] == "hide")]
                if is_hidden(lp) or key in ("Footprint", "Datasheet", "Description"):
                    eff.append([S("hide"), S("yes")])
            else:
                px, py, pa, eff = 0, 0, 0, effects(hide=True)
            node.append([S("property"), key, text, at(x + px, y + py, pa), eff])
        for key, text in fields:
            node.append([S("property"), key, text, at(x, y), effects(hide=True)])
        for num in dict.fromkeys(n for n, *_ in symbol_pins(sym)):
            node.append([S("pin"), num, [S("uuid"), uid(self.file, ref, "pin", num)]])
        node.append([S("instances"), [S("project"), PROJECT,
                                      [S("path"), self.path, [S("reference"), ref], [S("unit"), 1]]]])
        self.items.append(node)
        return sym

    def _wire(self, x1, y1, x2, y2):
        self.items.append([S("wire"), [S("pts"), [S("xy"), round(x1, 4), round(y1, 4)], [S("xy"), round(x2, 4), round(y2, 4)]],
                           [S("stroke"), [S("width"), 0], [S("type"), S("default")]],
                           [S("uuid"), uid(self.file, "wire", f"{x1},{y1},{x2},{y2}")]])

    def _power(self, net, x, y, d):
        """A power symbol pointing away from the pin (GND's arrow points down, +3V3's up)."""
        lib_id = POWER_NETS[net]
        self.p.pwr += 1
        if net == "GND":
            rot = {(0, 1): 0, (1, 0): 90, (0, -1): 180, (-1, 0): 270}[d]
        else:
            rot = {(0, -1): 0, (-1, 0): 90, (0, 1): 180, (1, 0): 270}[d]
        self._symbol(lib_id, f"#PWR{self.p.pwr:03d}", net, x, y, rot=rot, in_bom=False)

    def attach(self, x, y, d, net):
        """A short wire out of a pin at (x, y) in direction d, ending in a label or power symbol."""
        qx, qy = x + d[0] * STUB, y + d[1] * STUB
        self._claim(qx, qy, net)
        self._wire(x, y, qx, qy)
        if net in POWER_NETS:
            self._power(net, qx, qy, d)
        else:
            self.labels.append((net, qx, qy, LABEL_ANGLE[d]))

    # --- high level
    def part(self, lib_id, ref, value, x, y, nets, footprint="", fields=(), in_bom=True):
        """Place a symbol and give every pin a net (None = not connected)."""
        if ref in LCSC:
            fields = list(fields) + [("LCSC", LCSC[ref])]
        fields = list(fields) + FT_FIELDS.get(footprint.split(":")[-1], [])
        sym = self._symbol(lib_id, ref, value, x, y, footprint=footprint, in_bom=in_bom, fields=fields)
        done = {}
        for num, px, py, angle, kind in symbol_pins(sym):
            if num not in nets:
                if kind == "no_connect":
                    continue
                raise KeyError(f"{ref} pin {num} has no net")
            net = nets[num]
            ex, ey = x + px, y - py
            key = (round(ex, 3), round(ey, 3))
            if key in done:  # stacked pins share one connection point
                if done[key] != net:
                    raise ValueError(f"{ref} pin {num}: stacked pins on different nets")
                continue
            done[key] = net
            self._claim(ex, ey, net if net else f"NC:{ref}.{num}")
            if net is None:
                self.items.append([S("no_connect"), [S("at"), round(ex, 4), round(ey, 4)],
                                   [S("uuid"), uid(self.file, ref, "nc", num)]])
            else:
                self.attach(ex, ey, OUTWARD[angle], net)
        unknown = set(nets) - {n for n, *_ in symbol_pins(sym)}
        if unknown:
            raise KeyError(f"{ref}: no pins {sorted(unknown)}")

    def r(self, ref, value, x, y, top, bottom, footprint="Resistor_SMD:R_0603_1608Metric"):
        self.part("Device:R", ref, value, x, y, {"1": top, "2": bottom}, footprint)

    def c(self, ref, value, x, y, top="+3V3", bottom="GND", footprint="Capacitor_SMD:C_0603_1608Metric"):
        self.part("Device:C", ref, value, x, y, {"1": top, "2": bottom}, footprint)

    def flag(self, net, x, y):
        """A PWR_FLAG: tells ERC the net is powered from a connector."""
        self.p.flg += 1
        self._symbol("power:PWR_FLAG", f"#FLG{self.p.flg:02d}", "PWR_FLAG", x, y, in_bom=False)
        self._claim(x, y, net)
        self.attach(x, y, (0, 1), net)

    def note(self, text, x, y, size=1.27):
        self.items.append([S("text"), text, [S("exclude_from_sim"), S("no")], at(x, y),
                           [S("effects"), [S("font"), [S("size"), size, size]], [S("justify"), S("left"), S("top")]],
                           [S("uuid"), uid(self.file, "text", text[:40])]])


# Corrections for the KiCad Fabrication Toolkit's JLCPCB placement file, by footprint, so its
# output matches JLCPCB's own footprints (fitted 2026-10-27; tools/jlcpcb.py's JLC_FIX holds the
# same differences). The toolkit centres each part on its pads and adds its own rotation table;
# these fields cover where that table, or the pad centre, is wrong for JLCPCB's part.
FT_FIELDS = {
    "QFN-80-1EP_10x10mm_P0.4mm_EP3.4x3.4mm_ThermalVias": [("FT Rotation Offset", "-90")],
    "SOT-23-6": [("FT Rotation Offset", "90")],
    "JST_XH_B2B-XH-A_1x02_P2.50mm_Vertical": [("FT Rotation Offset", "180")],
    "PinSocket_1x04_P2.54mm_Vertical": [("FT Rotation Offset", "-90")],
    "PinHeader_1x03_P2.54mm_Vertical": [("FT Rotation Offset", "-90")],
    "IDC-Header_2x20_P2.54mm_Vertical": [("FT Rotation Offset", "-90")],
    "SOT-223-3_TabPin2": [("FT Position Offset", "-0.18,0")],
    "USB_C_Receptacle_HRO_TYPE-C-31-M-12": [("FT Position Offset", "0,0.111")],
    "microSD_TF-PUSH": [("FT Position Offset", "0,0.15")],
    "L_Abracon_AOTA-B201610S3R3": [("FT Rotation Offset", "180")],  # JLCPCB marks the dot at the other end
}

# ---------------------------------------------------------------- the design

# JLCPCB/LCSC part numbers, written into each part's "LCSC" field so KiCad's BOM export and
# the Fabrication Toolkit carry them. Checked in stock at JLCPCB on 2026-10-27.
def _refs(prefix, *nums):
    return [f"{prefix}{n}" for n in nums]


LCSC = {
    **{r: "C15850" for r in _refs("C", 1, 2, 6, 29)},              # 10uF 0805
    **{r: "C14663" for r in _refs("C", 3, 4, 5, 7, 8)},            # 100nF 0603
    **{r: "C23733" for r in _refs("C", 9, 10, 11)},                # 4.7uF 0402
    **{r: "C1525" for r in _refs("C", *range(12, 27))},            # 100nF 0402
    **{r: "C1548" for r in _refs("C", 27, 28)},                    # 15pF 0402
    "D1": "C2480", "D2": "C2480", "D5": "C2480", "D3": "C2297", "D4": "C84256",
    "F1": "C19193043",
    "J1": "C165948", "J2": "C158012", "J3": "C2718488", "J4": "C393941", "J5": "C19193815", "J6": "C2937625",
    "L1": "C42411119",
    "R1": "C23186", "R2": "C23186",                                # 5.1k 0603
    **{r: "C21190" for r in _refs("R", 3, 4, 6, 20)},              # 1k 0603
    "R5": "C23179",                                                # 470R 0603
    **{r: "C25804" for r in _refs("R", 7, 8, 9, 10, 11, 12, 13)},  # 10k 0603
    "R18": "C25744", "R19": "C11702",                              # 10k, 1k 0402 (by the flash)
    "R14": "C23140", "R15": "C23140",                              # 33R 0603
    "R16": "C25105", "R17": "C11702",                              # 33R, 1k 0402
    "R21": "C25100", "R22": "C25100",                              # 27R 0402 (no basic 27R in 0402)
    "SW1": "C318884", "SW2": "C318884",
    "U1": "C42415655", **{r: "C15298" for r in _refs("U", 2, 3, 5, 6)}, "U4": "C2687116", "U7": "C97521",
    "U8": "C6186", "Y1": "C20625731",
}



R0603 = "Resistor_SMD:R_0603_1608Metric"
C0603 = "Capacitor_SMD:C_0603_1608Metric"
C0805 = "Capacitor_SMD:C_0805_2012Metric"
C0402 = "Capacitor_SMD:C_0402_1005Metric"
R0402 = "Resistor_SMD:R_0402_1005Metric"


def power_sheet(sh):
    sh.note("USB-C (power and flashing), the 5 V header and 5 V from the HDD Synth on the link socket,\n"
            "diode-ORed into VB; an AMS1117 makes 3V3 from it.\n"
            "Never connect the IDE adapter's 5 V (Molex) to this board: the boards share ground only.", 20, 20, 1.6)
    usb = {"A1": "GND", "A12": "GND", "B1": "GND", "B12": "GND", "SH": "GND",
           "A4": "VBUS_USB", "A9": "VBUS_USB", "B4": "VBUS_USB", "B9": "VBUS_USB",
           "A5": "CC1", "B5": "CC2", "A6": "USB_DP", "B6": "USB_DP", "A7": "USB_DM", "B7": "USB_DM",
           "A8": None, "B8": None}
    sh.part("Connector:USB_C_Receptacle_USB2.0_16P", "J1", "USB-C", 50.8, 81.28, usb,
            "Connector_USB:USB_C_Receptacle_HRO_TYPE-C-31-M-12", fields=[("MPN", "HRO TYPE-C-31-M-12")])
    sh.r("R1", "5.1k", 101.6, 76.2, "CC1", "GND")
    sh.r("R2", "5.1k", 116.84, 76.2, "CC2", "GND")
    sh.note("CC pull-downs: without them a\nC-to-C cable gives no power.", 96.52, 91.44)
    sh.part("Power_Protection:USBLC6-2SC6", "U4", "USBLC6-2SC6", 160.02, 78.74,
            {"1": "USB_DP", "6": "USB_DP", "3": "USB_DM", "4": "USB_DM", "2": "GND", "5": "VBUS_USB"},
            "Package_TO_SOT_SMD:SOT-23-6")
    sh.note("ESD protection, close to J1. The 27 R series\nresistors sit by the RP2350 (RP2350 sheet).", 144.78, 93.98)

    sh.part("Device:D_Schottky", "D1", "SS14", 213.36, 60.96, {"2": "VBUS_USB", "1": "VB"}, "Diode_SMD:D_SMA")
    sh.part("Connector_Generic:Conn_01x02", "J2", "5V in", 50.8, 137.16, {"1": "5V_EXT_IN", "2": "GND"},
            "Connector_JST:JST_XH_B2B-XH-A_1x02_P2.50mm_Vertical")
    sh.part("Device:Polyfuse", "F1", "500mA", 99.06, 137.16, {"1": "5V_EXT_IN", "2": "5V_EXT"},
            "Fuse:Fuse_1206_3216Metric")
    sh.part("Device:D_Schottky", "D2", "SS14", 213.36, 81.28, {"2": "5V_EXT", "1": "VB"}, "Diode_SMD:D_SMA")
    sh.part("Device:D_Schottky", "D5", "SS14", 213.36, 101.6, {"2": "5V_LINK", "1": "VB"}, "Diode_SMD:D_SMA")
    sh.note("D1/D2/D5: diode-OR of USB, the 5 V header and the link socket's\n"
            "pin 1 (5 V from the HDD Synth); also reverse-polarity protection.\nVB is about 4.6 V.", 200.66, 111.76)
    sh.c("C1", "10uF", 254.0, 71.12, "VB", "GND", C0805)
    sh.flag("VB", 271.78, 63.5)
    sh.flag("GND", 287.02, 63.5)

    sh.part("Regulator_Linear:AMS1117-3.3", "U8", "AMS1117-3.3", 254.0, 137.16, {"3": "VB", "2": "+3V3", "1": "GND"},
            "Package_TO_SOT_SMD:SOT-223-3_TabPin2")
    sh.c("C29", "10uF", 287.02, 137.16, "+3V3", "GND", C0805)
    sh.note("3V3 for everything: ~1 V dropout, so 3.3 V holds down to\nabout 4.4 V on VB. The board draws well under 300 mA.", 228.6, 152.4)

    sh.part("Switch:SW_Push", "SW1", "BOOTSEL", 63.5, 200.66, {"1": "BOOTSEL", "2": "GND"},
            "Button_Switch_SMD:SW_Push_1P1T_XKB_TS-1187A")
    sh.part("Switch:SW_Push", "SW2", "RESET", 63.5, 223.52, {"1": "RUN_BTN", "2": "GND"},
            "Button_Switch_SMD:SW_Push_1P1T_XKB_TS-1187A")
    sh.note("Hold BOOTSEL and press RESET for the UF2 drive.\nBoth go through 1k on the RP2350 sheet.", 45.72, 238.76)
    sh.part("Connector_Generic:Conn_01x03", "J6", "SWD", 147.32, 210.82,
            {"1": "SWCLK", "2": "GND", "3": "SWDIO"},
            "Connector_PinHeader_2.54mm:PinHeader_1x03_P2.54mm_Vertical")
    sh.note("Optional SWD: 0.1-inch header in the Raspberry Pi debug connector's order;\n"
            "the Debug Probe's JST-to-header cable fits it.", 129.54, 223.52)


def mcu_sheet(sh):
    sh.note("RP2350B with Raspberry Pi's minimal-design support circuit (\"Hardware design with RP2350\"):\n"
            "core regulator, 12 MHz crystal, 16 MB QSPI flash, USB. GPIO numbers as in src/config.h;\n"
            "GP3, GP31, GP40 and GP41 are new on this board.", 20, 20, 1.6)
    power = {"IOVDD": "+3V3", "ADC_AVDD": "+3V3", "USB_OTP_VDD": "+3V3", "QSPI_IOVDD": "+3V3", "VREG_VIN": "+3V3",
             "DVDD": "+1V1", "VREG_FB": "+1V1", "VREG_AVDD": "VREG_AVDD", "VREG_LX": "VREG_LX",
             "VREG_PGND": "GND", "GND": "GND", "XIN": "XIN", "XOUT": "XOUT", "RUN": "RUN",
             "SWCLK": "SWCLK", "SWDIO": "SWDIO", "USB_DP": "USB_DP_MCU", "USB_DM": "USB_DM_MCU",
             "~{QSPI_SS}": "QSPI_SS", "QSPI_SCLK": "QSPI_SCLK", "QSPI_SD0": "QSPI_SD0", "QSPI_SD1": "QSPI_SD1",
             "QSPI_SD2": "QSPI_SD2", "QSPI_SD3": "QSPI_SD3"}
    gp = {g: None for g in range(48)}
    gp.update({0: "LINK_TX", 1: "LINK_RX", 3: "DMACK_N", 25: "LED", 26: "DIOR_N", 27: "DIOW_N", 28: "IORDY",
               29: "INTRQ", 30: "RESET_N", 31: "DMARQ", 34: "SD_CLK", 35: "SD_CMD", 36: "SD_DAT0", 37: "SD_DAT1",
               38: "SD_DAT2", 39: "SD_DAT3", 40: "SD_CD", 41: "BUS_EN_N", 23: "CS0_N", 24: "CS1_N"})
    gp.update({4 + i: f"DD{i}" for i in range(16)})
    gp.update({20 + i: f"DA{i}" for i in range(3)})
    pins = {}
    sym = symbol_def("MCU_RaspberryPi:RP2350B")
    for unit in find(sym, "symbol"):
        for p in find(unit, "pin"):
            name, num = first(p, "name")[1], first(p, "number")[1]
            pins[num] = gp[int(name[4:].split("/")[0])] if name.startswith("GPIO") else power[name]
    sh.part("MCU_RaspberryPi:RP2350B", "U1", "RP2350B", 205.74, 152.4, pins,
            "Package_DFN_QFN:QFN-80-1EP_10x10mm_P0.4mm_EP3.4x3.4mm_ThermalVias")

    # Core regulator, as in Raspberry Pi's layout: the polarity-marked inductor (pad 1 to 1.1 V),
    # 4.7 uF on VREG_VIN and on the 1.1 V output, and an RC filter (33 R, 4.7 uF) on VREG_AVDD.
    sh.note("Core regulator: Raspberry Pi require this inductor (polarity dot), and their layout.", 30.48, 45.72)
    sh.part("Device:L", "L1", "3.3uH", 40.64, 66.04, {"1": "+1V1", "2": "VREG_LX"},
            "HDDSynthPro:L_Abracon_AOTA-B201610S3R3", fields=[("MPN", "Abracon AOTA-B201610S3R3-101-T")])
    sh.c("C9", "4.7uF", 55.88, 66.04, "+3V3", "GND", "HDDSynthPro:C_0402_1005Metric_RPi_Wide")
    sh.c("C10", "4.7uF", 71.12, 66.04, "+1V1", "GND", "HDDSynthPro:C_0402_1005Metric_RPi_Wide")
    sh.r("R16", "33", 86.36, 66.04, "+3V3", "VREG_AVDD", R0402)
    sh.c("C11", "4.7uF", 101.6, 66.04, "VREG_AVDD", "GND", C0402)
    sh.flag("+1V1", 116.84, 58.42)
    sh.flag("VREG_AVDD", 129.54, 58.42)

    # A 100 nF on every supply pin: IOVDD x8, DVDD x3, ADC_AVDD, USB_OTP_VDD, QSPI_IOVDD.
    sh.note("100 nF on every supply pin, each by its pin.", 30.48, 236.22)
    for i in range(14):
        sh.c(f"C{12 + i}", "100nF", 40.64 + 12.7 * i, 254.0, "+1V1" if i in (8, 9, 10) else "+3V3", "GND", C0402)

    # Crystal, exactly as Raspberry Pi specify: ABM8-272-T3, 15 pF each side, 1k from XOUT.
    sh.note("12 MHz crystal: Raspberry Pi's part and values (ABM8-272-T3, 15 pF, 1k).", 30.48, 172.72)
    sh.part("Device:Crystal_GND24", "Y1", "12MHz", 63.5, 196.85, {"1": "XIN", "3": "XTAL_OUT", "2": "GND", "4": "GND"},
            "Crystal:Crystal_SMD_3225-4Pin_3.2x2.5mm", fields=[("MPN", "Abracon ABM8-272-T3")])
    sh.c("C27", "15pF", 40.64, 203.2, "XIN", "GND", C0402)
    sh.c("C28", "15pF", 86.36, 203.2, "XTAL_OUT", "GND", C0402)
    sh.r("R17", "1k", 101.6, 190.5, "XOUT", "XTAL_OUT", R0402)

    # QSPI flash, BOOTSEL and RUN.
    sh.part("Memory_Flash:W25Q128JVS", "U7", "W25Q128JVSIQ", 76.2, 127.0,
            {"1": "QSPI_SS", "2": "QSPI_SD1", "3": "QSPI_SD2", "4": "GND", "5": "QSPI_SD0", "6": "QSPI_SCLK",
             "7": "QSPI_SD3", "8": "+3V3"}, "Package_SO:SOIC-8_5.3x5.3mm_P1.27mm")
    sh.c("C26", "100nF", 101.6, 116.84, "+3V3", "GND", C0402)
    sh.r("R18", "10k", 116.84, 116.84, "+3V3", "QSPI_SS", R0402)
    sh.r("R19", "1k", 132.08, 116.84, "QSPI_SS", "BOOTSEL", R0402)
    sh.r("R20", "1k", 132.08, 142.24, "RUN", "RUN_BTN")
    sh.note("Flash CS pulled up (10k). BOOTSEL pulls QSPI_SS low through 1k;\n"
            "RESET pulls RUN low through 1k (RUN has its own pull-up).", 99.06, 157.48)

    # USB: 27 R series resistors by the RP2350.
    sh.r("R21", "27", 297.18, 203.2, "USB_DP", "USB_DP_MCU", R0402)
    sh.r("R22", "27", 312.42, 203.2, "USB_DM", "USB_DM_MCU", R0402)
    sh.note("USB: 27 R series, next to the RP2350.", 287.02, 218.44)

    sh.c("C6", "10uF", 330.2, 203.2, "+3V3", "GND", C0805)
    sh.note("Bulk on 3V3.", 322.58, 218.44)

    sh.note("Activity link to the HDD Synth (docs/activity-link.md): UART0, 115200 8N1, 3.3 V logic.\n"
            "Pin order matches the synth's J6. Pin 1 takes 5 V from the synth, through D5 (Power and USB sheet),\n"
            "so one supply runs both boards. The synth's J6 pin 1 must carry 5 V, not its 3.3 V.", 279.4, 63.5)
    sh.r("R3", "1k", 289.56, 88.9, "LINK_TX", "LINK_TX_EXT")
    sh.r("R4", "1k", 309.88, 88.9, "LINK_RX", "LINK_RX_EXT")
    sh.part("Connector_Generic:Conn_01x04", "J3", "Link", 373.38, 88.9,
            {"1": "5V_LINK", "2": "GND", "3": "LINK_TX_EXT", "4": "LINK_RX_EXT"},
            "Connector_PinSocket_2.54mm:PinSocket_1x04_P2.54mm_Vertical")

    sh.note("Activity LED (GP25, core 0's updateLed()) and an optional power LED.", 279.4, 134.62)
    sh.r("R5", "470", 289.56, 152.4, "LED", "LED_A")
    sh.part("Device:LED", "D3", "Activity", 289.56, 172.72, {"2": "LED_A", "1": "GND"}, "LED_SMD:LED_0805_2012Metric")
    sh.r("R6", "1k", 340.36, 152.4, "+3V3", "PWR_LED_A")
    sh.part("Device:LED", "D4", "Power", 340.36, 172.72, {"2": "PWR_LED_A", "1": "GND"}, "LED_SMD:LED_0805_2012Metric")


IDE_PINS = {  # signal -> 40-pin IDE header pin
    **{f"DD{i}": p for i, p in enumerate([17, 15, 13, 11, 9, 7, 5, 3, 4, 6, 8, 10, 12, 14, 16, 18])},
    "DA0": 35, "DA1": 33, "DA2": 36, "CS0_N": 37, "CS1_N": 38, "DIOR_N": 25, "DIOW_N": 23,
    "IORDY": 27, "INTRQ": 31, "RESET_N": 1, "DMARQ": 21, "DMACK_N": 29,
}
IDE_GND = [2, 19, 22, 24, 26, 30, 40]
IDE_NC = [20, 28, 32, 34, 39]


def ide_sheet(sh):
    sh.note("IDE bus through four SN74CB3T3245 FET bus switches: A side to the RP2350, B side to the IDE header.\n"
            "OE# low = connected. They cap what the RP2350 sees at about 3.3 V and isolate it when unpowered (Ioff).\n"
            "No pull-ups or pull-downs on the bus: the host provides them. IORDY stays open drain.", 20, 20, 1.6)
    # Each switch takes eight consecutive IDE header signals, in the header's pin order from
    # B8 (nearest the header's pin 1) to B1, so the B side, facing the header, runs straight.
    # Channel -> net, or (A-side net, B-side net) where a resistor sits on the A side.
    switches = [
        ("U2", 120.65, 88.9, {8: "RESET_N", 7: "DD7", 6: "DD8", 5: "DD6", 4: "DD9", 3: "DD5", 2: "DD10", 1: "DD4"}),
        ("U3", 120.65, 157.48, {8: "DD11", 7: "DD3", 6: "DD12", 5: "DD2", 4: "DD13", 3: "DD1", 2: "DD14", 1: "DD0"}),
        ("U5", 205.74, 88.9, {8: "DD15", 7: "DMARQ", 6: ("DIOW_N_SW", "DIOW_N"), 5: ("DIOR_N_SW", "DIOR_N"),
                              4: "IORDY", 3: "DMACK_N", 2: "INTRQ", 1: "DA1"}),
        ("U6", 205.74, 157.48, {8: "DA0", 7: "DA2", 6: "CS0_N", 5: "CS1_N"}),
    ]
    for ref, x, y, channels in switches:
        nets = {"20": "+3V3", "10": "GND", "19": "BUS_EN_N"}
        for k in range(1, 9):
            sig = channels.get(k)
            a, b = sig if isinstance(sig, tuple) else (sig, sig)
            nets[str(k + 1)] = a
            nets[str(19 - k)] = None if b is None else "IDE_" + b
        sh.part("HDDSynthPro:SN74CB3T3245", ref, "SN74CB3T3245PWR", x, y, nets,
                "Package_SO:TSSOP-20_4.4x6.5mm_P0.65mm")
    sh.note("Channels follow the IDE header's pin order (see the comment in gen_schematic.py).\n"
            "U6 channels 1-4 are spare. GP31 (DMARQ) and GP3 (DMACK#) are reserved for DMA.", 104.14, 200.66)

    sh.r("R14", "33", 66.04, 182.88, "DIOR_N", "DIOR_N_SW")
    sh.r("R15", "33", 81.28, 182.88, "DIOW_N", "DIOW_N_SW")
    sh.note("33 R damping on the strobes,\nclose to the RP2350.", 58.42, 205.74)

    sh.r("R13", "10k", 66.04, 99.06, "+3V3", "BUS_EN_N")
    sh.part("Jumper:SolderJumper_2_Open", "JP1", "BUS_EN", 71.12, 124.46, {"1": "BUS_EN_N", "2": "GND"},
            "Jumper:SolderJumper-2_P1.3mm_Open_RoundedPad1.0x1.5mm", in_bom=False)
    sh.note("BUS_EN# (GP41) is high, so the bus is disconnected,\nuntil firmware drives it low. Close JP1 to run\n"
            "firmware that doesn't drive GP41.", 45.72, 134.62)
    sh.c("C4", "100nF", 50.8, 68.58)
    sh.c("C5", "100nF", 66.04, 68.58)
    sh.c("C7", "100nF", 81.28, 68.58)
    sh.c("C8", "100nF", 96.52, 68.58)
    sh.note("Next to the VCC pins of U2, U3, U5 and U6.", 45.72, 83.82)

    j5 = {str(p): "IDE_" + s for s, p in IDE_PINS.items()}
    j5.update({str(p): "GND" for p in IDE_GND})
    j5.update({str(p): None for p in IDE_NC})
    sh.part("Connector_Generic:Conn_02x20_Odd_Even", "J5", "IDE", 289.56, 152.4, j5,
            "Connector_IDC:IDC-Header_2x20_P2.54mm_Vertical")
    sh.note("IDE 40-pin, shrouded and keyed (pin 20 removed).\nNot connected: 20 key, 28 CSEL, 32 IOCS16#,\n"
            "34 PDIAG#, 39 DASP#.", 274.32, 190.5)


def sd_sheet(sh):
    sh.note("microSD: SPI now, 4-bit SDIO later. CLK, CMD and DAT0 on SPI0's SCK, TX and RX; DAT3 is the SPI\n"
            "chip select; DAT0-3 on consecutive GPIOs (36-39), as PIO SDIO needs.", 20, 20, 1.6)
    sh.part("Connector:Micro_SD_Card_Det1", "J4", "microSD", 228.6, 111.76,
            {"1": "SD_DAT2", "2": "SD_DAT3", "3": "SD_CMD", "4": "+3V3", "5": "SD_CLK", "6": "GND",
             "7": "SD_DAT0", "8": "SD_DAT1", "9": "SD_CD", "SH": "GND"},
            "HDDSynthPro:microSD_TF-PUSH", fields=[("MPN", "TF PUSH (generic push-push)")])
    sh.note("Card detect (GP40): pin 9, pulled up by R12. On these sockets it usually\n"
            "meets the grounded shell when a card is in (reads low): check on the first board.", 205.74, 132.08)
    for i, (ref, net) in enumerate([("R7", "SD_CMD"), ("R8", "SD_DAT0"), ("R9", "SD_DAT1"),
                                    ("R10", "SD_DAT2"), ("R11", "SD_DAT3"), ("R12", "SD_CD")]):
        sh.r(ref, "10k", 63.5 + 17.78 * i, 99.06, "+3V3", net)
    sh.note("Pull-ups, close to the socket. CLK needs none.", 58.42, 116.84)
    sh.c("C2", "10uF", 81.28, 149.86, "+3V3", "GND", C0805)
    sh.c("C3", "100nF", 99.06, 149.86)
    sh.note("At the socket's VDD.", 76.2, 162.56)


# ---------------------------------------------------------------- output

def title_block(title, comment=""):
    tb = [S("title_block"), [S("title"), title], [S("date"), DATE], [S("rev"), REV], [S("company"), "HDDSynthPro"],
          [S("comment"), 1, "Generated by hardware/tools/gen_schematic.py from docs/pcb.html"]]
    if comment:
        tb.append([S("comment"), 2, comment])
    return tb


def label_nodes(sheet, global_nets):
    out = []
    for net, x, y, angle in sheet.labels:
        left = angle in (0, 90)
        if net in global_nets:
            out.append([S("global_label"), net, [S("shape"), S("bidirectional")], at(x, y, angle),
                        [S("fields_autoplaced"), S("yes")], effects("left" if left else "right"),
                        [S("uuid"), uid(sheet.file, "label", net, f"{x},{y}")],
                        [S("property"), "Intersheetrefs", "${INTERSHEET_REFS}", at(x, y, angle),
                         effects("left" if left else "right", hide=True)]])
        else:
            out.append([S("label"), net, at(x, y, angle), [S("fields_autoplaced"), S("yes")],
                        effects("left bottom" if left else "right bottom"),
                        [S("uuid"), uid(sheet.file, "label", net, f"{x},{y}")]])
    return out


def lib_symbols(lib_ids):
    out = [S("lib_symbols")]
    for lib_id in sorted(lib_ids):
        sym = symbol_def(lib_id)
        out.append([S("symbol"), lib_id] + sym[2:])
    return out


def write_sheet(path, file_uuid, title, body, lib_ids, extra=(), comment=""):
    doc = [S("kicad_sch"), [S("version"), SCH_VERSION], [S("generator"), "hddsynthpro_gen"],
           [S("generator_version"), "1.0"], [S("uuid"), file_uuid], [S("paper"), "A3"],
           title_block(title, comment), lib_symbols(lib_ids)] + body + list(extra) + [[S("embedded_fonts"), S("no")]]
    path.write_text(dump(doc) + "\n")


def root_sheet(project):
    items = []
    x = 30.48
    for sh in project.sheets:
        w, h = 60.96, 25.4
        items.append([S("sheet"), [S("at"), x, 60.96], [S("size"), w, h],
                      [S("exclude_from_sim"), S("no")], [S("in_bom"), S("yes")], [S("on_board"), S("yes")],
                      [S("dnp"), S("no")], [S("fields_autoplaced"), S("yes")],
                      [S("stroke"), [S("width"), 0.1524], [S("type"), S("solid")]],
                      [S("fill"), [S("color"), 0, 0, 0, 0.0]],
                      [S("uuid"), sh.sym_uuid],
                      [S("property"), "Sheetname", sh.name, at(x, 60.2484), effects("left bottom")],
                      [S("property"), "Sheetfile", sh.file, at(x, 60.96 + h + 0.5866), effects("left top")],
                      [S("instances"), [S("project"), PROJECT,
                                        [S("path"), f"/{project.root_uuid}", [S("page"), str(sh.page)]]]]])
        x += w + 30.48
    note = ("HDDSynthPro: an RP2350B that acts as an IDE hard disk.\n"
            "Pages: power and USB; the RP2350 with the activity link and LEDs; the IDE bus switches and header; the microSD socket.\n"
            "Nets cross pages through global labels. The GPIO map must agree with src/config.h and docs/wiring.md:\n"
            "run hardware/tools/check_netlist.py after editing.")
    items.append([S("text"), note, [S("exclude_from_sim"), S("no")], at(30.48, 30.48),
                  [S("effects"), [S("font"), [S("size"), 2, 2]], [S("justify"), S("left"), S("top")]],
                  [S("uuid"), uid("root", "note")]])
    write_sheet(HW / f"{PROJECT}.kicad_sch", project.root_uuid, "HDDSynthPro", items, set(),
                extra=[[S("sheet_instances"), [S("path"), "/", [S("page"), "1"]]]])


def write_library():
    lib = [S("kicad_symbol_lib"), [S("version"), 20241209], [S("generator"), "hddsynthpro_gen"],
           [S("generator_version"), "1.0"], pga2350_symbol(), cb3t3245_symbol()]
    (HW / "HDDSynthPro.kicad_sym").write_text(dump(lib) + "\n")


def write_footprint():
    """PGA2350: 64 through-hole pads on a 2.54 mm grid, two rings, top view."""
    rows = "ABCDEFGHIJ"
    used = set(PGA_GPIO_PADS.values()) | set(PGA_GND_PADS) | {"A3", "A4", "A5", "A6", "A7", "A8", "B4", "B5", "B6", "B7"}
    assert len(used) == 64, len(used)

    def u(*k):
        return [S("uuid"), uid("fp", *k)]

    def line_rect(layer, half, width):
        return [S("fp_rect"), [S("start"), -half, -half], [S("end"), half, half],
                [S("stroke"), [S("width"), width], [S("type"), S("solid")]], [S("fill"), S("no")],
                [S("layer"), layer], u("rect", layer)]

    fx = lambda size, thick: [S("effects"), [S("font"), [S("size"), size, size], [S("thickness"), thick]]]
    fp = [S("footprint"), "PGA2350", [S("version"), 20241229], [S("generator"), "hddsynthpro_gen"],
          [S("generator_version"), "1.0"], [S("layer"), "F.Cu"],
          [S("descr"), "Pimoroni PGA2350 (RP2350B module), 64 pins, 2.54 mm grid, 25.4 mm square. "
                       "https://shop.pimoroni.com/products/pga2350"],
          [S("tags"), "PGA2350 RP2350 Pimoroni PGA"],
          [S("property"), "Reference", "REF**", [S("at"), 0, -14.2, 0], [S("layer"), "F.SilkS"], u("ref"), fx(1, 0.15)],
          [S("property"), "Value", "PGA2350", [S("at"), 0, 14.2, 0], [S("layer"), "F.Fab"], u("val"), fx(1, 0.15)],
          [S("attr"), S("through_hole")],
          line_rect("F.Fab", 12.7, 0.1), line_rect("F.SilkS", 12.82, 0.12), line_rect("F.CrtYd", 13.2, 0.05),
          [S("fp_poly"), [S("pts"), [S("xy"), -13.7, -13.7], [S("xy"), -12.5, -13.7], [S("xy"), -13.7, -12.5]],
           [S("stroke"), [S("width"), 0.12], [S("type"), S("solid")]], [S("fill"), S("yes")],
           [S("layer"), "F.SilkS"], u("pin1")],
          [S("fp_text"), S("user"), "${REFERENCE}", [S("at"), 0, 0, 0], [S("layer"), "F.Fab"], u("reftext"), fx(1, 0.15)]]
    for r, row in enumerate(rows):
        for col in range(1, 11):
            name = f"{row}{col}"
            if name not in used:
                continue
            x, y = round((col - 5.5) * 2.54, 4), round((r + 0.5 - 5) * 2.54, 4)
            shape = S("rect") if name == "A1" else S("circle")
            fp.append([S("pad"), name, S("thru_hole"), shape, [S("at"), x, y], [S("size"), 1.7, 1.7],
                       [S("drill"), 1.0], [S("layers"), "*.Cu", "*.Mask"], [S("remove_unused_layers"), S("no")],
                       u("pad", name)])
    fp.append([S("embedded_fonts"), S("no")])
    d = HW / "HDDSynthPro.pretty"
    d.mkdir(exist_ok=True)
    (d / "PGA2350.kicad_mod").write_text(dump(fp) + "\n")


def write_project(project):
    # Update the project in place: KiCad keeps the board's net classes and rules in it too.
    path = HW / f"{PROJECT}.kicad_pro"
    pro = json.loads(path.read_text()) if path.exists() else {"boards": [], "text_variables": {}}
    pro["meta"] = {"filename": f"{PROJECT}.kicad_pro", "version": pro.get("meta", {}).get("version", 3)}
    pro["sheets"] = [[project.root_uuid, "Root"]] + [[sh.sym_uuid, sh.name] for sh in project.sheets]
    path.write_text(json.dumps(pro, indent=2) + "\n")
    (HW / "sym-lib-table").write_text(
        '(sym_lib_table\n\t(version 7)\n\t(lib (name "HDDSynthPro")(type "KiCad")(uri "${KIPRJMOD}/HDDSynthPro.kicad_sym")'
        '(options "")(descr "HDDSynthPro parts"))\n)\n')
    (HW / "fp-lib-table").write_text(
        '(fp_lib_table\n\t(version 7)\n\t(lib (name "HDDSynthPro")(type "KiCad")(uri "${KIPRJMOD}/HDDSynthPro.pretty")'
        '(options "")(descr "HDDSynthPro footprints"))\n)\n')


def main():
    project = Project()
    builders = [("Power and USB", "power.kicad_sch", power_sheet), ("RP2350", "mcu.kicad_sch", mcu_sheet),
                ("IDE bus", "ide.kicad_sch", ide_sheet), ("microSD", "sd.kicad_sch", sd_sheet)]
    for page, (name, file, build) in enumerate(builders, start=2):
        build(project.sheet(name, file, page))

    sheets_of = {}
    for sh in project.sheets:
        for net, *_ in sh.labels:
            sheets_of.setdefault(net, set()).add(sh.file)
    global_nets = {n for n, s in sheets_of.items() if len(s) > 1}
    for sh in project.sheets:
        counts = {}
        for net, *_ in sh.labels:
            counts[net] = counts.get(net, 0) + 1
        lonely = [n for n, c in counts.items() if c == 1 and n not in global_nets]
        if lonely:
            raise ValueError(f"{sh.file}: nets with one connection: {lonely}")

    for sh in project.sheets:
        write_sheet(HW / sh.file, uid("file", sh.file), f"HDDSynthPro: {sh.name}",
                    sh.items + label_nodes(sh, global_nets), sh.lib_ids)
    root_sheet(project)
    write_library()
    write_footprint()
    write_project(project)
    print(f"wrote {len(project.sheets) + 1} sheets, {len(global_nets)} global nets")


if __name__ == "__main__":
    main()
