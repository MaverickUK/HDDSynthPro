#!/usr/bin/env python3
"""Writes the BOM and placement (CPL) files for JLCPCB's assembly service.

Usage: python3 hardware/tools/jlcpcb.py   -> hardware/fab/bom-jlcpcb.csv, hardware/fab/cpl-jlcpcb.csv

Every assembled part carries its JLCPCB/LCSC part number from PARTS below, so
JLCPCB's BOM tool has nothing to guess. Parts in HAND_FIT are left off both files:
JLCPCB doesn't stock them, or they're optional.

Part numbers were checked against JLCPCB's parts library on 2026-10-27. Stock
changes: re-check in JLCPCB's BOM tool before ordering.
"""
import csv
import math
import subprocess
import sys
import tempfile
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from sexpr import find, first, parse  # noqa: E402

HW = Path(__file__).resolve().parent.parent
FAB = HW / "fab"

# ref -> (JLCPCB part #, note). "basic" parts carry no per-part setup fee.
PARTS = {
    **{r: ("C15850", "10uF 0805 X5R, basic") for r in ("C1", "C2", "C6")},
    **{r: ("C14663", "100nF 0603 X7R, basic") for r in ("C3", "C4", "C5", "C7", "C8")},
    **{r: ("C2480", "SS14, basic") for r in ("D1", "D2")},
    "D3": ("C2297", "KT-0805G green LED, basic"),
    "D4": ("C84256", "NCD0805R1 red LED, basic"),
    "F1": ("C19193043", "1206L050/33NR polyfuse 500mA"),
    **{r: ("C23186", "5.1k 0603, basic") for r in ("R1", "R2")},
    **{r: ("C21190", "1k 0603, basic") for r in ("R3", "R4", "R6")},
    "R5": ("C23179", "470R 0603, basic"),
    **{r: ("C25804", "10k 0603, basic") for r in ("R7", "R8", "R9", "R10", "R11", "R12", "R13")},
    **{r: ("C23140", "33R 0603, basic") for r in ("R14", "R15")},
    "U4": ("C2687116", "USBLC6-2SC6"),
    "J1": ("C5184243", "GCT USB4105-GF-A-120 (1.2 mm shell stakes), same footprint as USB4105-GF-A"),
    "J4": ("C114218", "Hirose DM3AT-SF-PEJM5"),
    **{r: ("C231329", "Omron B3U-1000P") for r in ("SW1", "SW2")},
    **{r: ("C15298", "SN74CB3T3245PWR") for r in ("U2", "U3", "U5", "U6")},
    # Through-hole: JLCPCB fits these at extra cost, or move them to HAND_FIT.
    "J2": ("C158012", "JST B2B-XH-A(LF)(SN), through-hole"),
    "J3": ("C2718488", "1x4 2.54 mm female header, through-hole"),
    "J5": ("C19193815", "2x20 2.54 mm box header, through-hole"),
    "J6": ("C2937625", "1x3 2.54 mm pin header (SWD, optional), through-hole"),
}
# Parts whose KiCad footprint differs from JLCPCB's: KiCad puts their origin on pin 1,
# JLCPCB on the middle of the part, and JLCPCB draws them turned. ref -> rotation to send,
# from JLCPCB's own footprints (2026-10-27): the 1x4 socket and 1x3 header have pin 1 on
# the left and run right (as on this board: 0); the 2x20 header has pin 1 bottom-left,
# pin 2 above it, running right (this board runs it down the edge: 270). The position
# sent is the centre of the part's pads, worked out from the board.
JLC_ROTATION = {"J2": 180, "J3": 0, "J5": 270, "J6": 0}  # J2: JLCPCB has pin 1 on the right
# JLCPCB draws these packages with pin 1 bottom-left and the pins along the bottom; KiCad
# with pin 1 top-left and the pins down the side. So JLCPCB's rotation is KiCad's minus 90.
JLC_ROTATION_OFFSET = {**{u: -90 for u in ("U2", "U3", "U5", "U6")}, "U4": -90}  # TSSOP-20, SOT-23-6
# Not on JLCPCB's order: fitted by hand.
HAND_FIT = {
    "U1": "PGA2350: from Pimoroni, soldered by hand",
}


def netlist():
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "board.net"
        subprocess.run(["kicad-cli", "sch", "export", "netlist", "--format", "kicadsexpr", "-o", str(out),
                        str(HW / "hddsynthpro.kicad_sch")], check=True, capture_output=True)
        root = parse(out.read_text())
    # Parts marked "exclude from BOM" (JP1, a solder jumper) aren't parts to buy.
    return {first(c, "ref")[1]: (first(c, "value")[1], first(c, "footprint")[1].split(":")[1])
            for c in find(first(root, "components"), "comp")
            if "exclude_from_bom" not in {first(p, "name")[1] for p in find(c, "property")}}


def placements():
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "pos.csv"
        subprocess.run(["kicad-cli", "pcb", "export", "pos", "--format", "csv", "--units", "mm", "--side", "both",
                        "-o", str(out), str(HW / "hddsynthpro.kicad_pcb")], check=True, capture_output=True)
        with out.open() as f:
            return {row["Ref"]: row for row in csv.DictReader(f)}


def pad_centres(refs):
    """ref -> centre of its pads, in KiCad board coordinates (mm, y down)."""
    root = parse((HW / "hddsynthpro.kicad_pcb").read_text())
    out = {}
    for fp in find(root, "footprint"):
        ref = next((p[2] for p in find(fp, "property") if p[1] == "Reference"), None)
        if ref not in refs:
            continue
        at = first(fp, "at")
        fx, fy = float(at[1]), float(at[2])
        rot = math.radians(float(at[3]) if len(at) > 3 else 0.0)
        pads = [first(p, "at") for p in find(fp, "pad")]
        lx = sum(float(a[1]) for a in pads) / len(pads)
        ly = sum(float(a[2]) for a in pads) / len(pads)
        # Pad positions are in the footprint's own (unrotated) frame; KiCad turns it counterclockwise.
        out[ref] = (fx + lx * math.cos(rot) + ly * math.sin(rot), fy - lx * math.sin(rot) + ly * math.cos(rot))
    return out


def ref_key(ref):
    head = ref.rstrip("0123456789")
    return head, int(ref[len(head):] or 0)


def main():
    comps = netlist()
    missing = set(comps) - set(PARTS) - set(HAND_FIT)
    if missing:
        sys.exit(f"no JLCPCB part for {sorted(missing)}")
    FAB.mkdir(exist_ok=True)

    groups = defaultdict(list)
    for ref, (lcsc, _) in PARTS.items():
        value, footprint = comps[ref]
        groups[(lcsc, value, footprint)].append(ref)
    with (FAB / "bom-jlcpcb.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Comment", "Designator", "Footprint", "JLCPCB Part #"])
        for (lcsc, value, footprint), refs in sorted(groups.items(), key=lambda g: ref_key(min(g[1], key=ref_key))):
            w.writerow([value, ",".join(sorted(refs, key=ref_key)), footprint, lcsc])

    pos = placements()
    centres = pad_centres(set(JLC_ROTATION))
    with (FAB / "cpl-jlcpcb.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
        for ref in sorted(PARTS, key=ref_key):
            p = pos[ref]
            x, y, rot = float(p["PosX"]), float(p["PosY"]), float(p["Rot"])
            if ref in JLC_ROTATION:
                # kicad-cli's position file has y pointing up, so its y is the board's, negated.
                x, y, rot = centres[ref][0], -centres[ref][1], JLC_ROTATION[ref]
            rot = (rot + JLC_ROTATION_OFFSET.get(ref, 0)) % 360
            w.writerow([ref, f"{x:.3f}mm", f"{y:.3f}mm", "Top" if p["Side"] == "top" else "Bottom", f"{rot:.0f}"])

    print(f"wrote fab/bom-jlcpcb.csv ({len(groups)} lines) and fab/cpl-jlcpcb.csv ({len(PARTS)} parts)")
    for ref, why in HAND_FIT.items():
        print(f"  not ordered: {ref}: {why}")


if __name__ == "__main__":
    main()
