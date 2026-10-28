#!/usr/bin/env python3
"""Writes the BOM and placement (CPL) files for JLCPCB's assembly service.

Usage: python3 hardware/tools/jlcpcb.py   -> hardware/fab/bom-jlcpcb.csv, hardware/fab/cpl-jlcpcb.csv

Part numbers come from each symbol's "LCSC" field (set in tools/gen_schematic.py),
so the BOM always matches the schematic. Parts in HAND_FIT are left off both files.

JLCPCB places parts using its own footprints, which for some packages are turned,
or centred differently, from KiCad's. JLC_FIX holds the difference per footprint,
found by fitting JLCPCB's footprint pads onto KiCad's (2026-10-27): the KiCad pad
at p is JLCPCB's pad at R(theta) p + t, in the footprints' own frames. So a part
KiCad places at Q turned rho goes to JLCPCB at Q + R(rho) t, turned rho + theta.
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

# footprint -> (theta degrees, tx mm, ty mm). Footprints not listed match JLCPCB's.
JLC_FIX = {
    "TSSOP-20_4.4x6.5mm_P0.65mm": (270, 0.0, 0.0),             # bus switches
    "SOT-23-6": (270, 0.0, 0.0),                               # USBLC6
    "SOIC-8_5.3x5.3mm_P1.27mm": (270, 0.0, 0.0),               # flash
    "SOT-223-3_TabPin2": (180, -0.18, 0.0),                    # AMS1117
    "USB_C_Receptacle_HRO_TYPE-C-31-M-12": (0, 0.0, -1.571),
    "JST_XH_B2B-XH-A_1x02_P2.50mm_Vertical": (180, 1.25, 0.0),  # KiCad's origin is pin 1
    "PinSocket_1x04_P2.54mm_Vertical": (270, 0.0, 3.81),
    "PinHeader_1x03_P2.54mm_Vertical": (270, 0.0, 2.54),
    "IDC-Header_2x20_P2.54mm_Vertical": (270, 1.27, 24.13),
    # Electrically symmetric, but JLCPCB's footprint marks the polarity dot by its pad 2 and
    # Raspberry Pi's (ours) by pad 1, the 1.1 V end: turned 180, JLCPCB fits the dot where they want it.
    "L_Abracon_AOTA-B201610S3R3": (180, 0.0, 0.0),
}
# Not on JLCPCB's order: fitted by hand. ref -> why.
HAND_FIT = {}


def netlist():
    """ref -> (value, footprint, LCSC part #) for every part on the BOM."""
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "board.net"
        subprocess.run(["kicad-cli", "sch", "export", "netlist", "--format", "kicadsexpr", "-o", str(out),
                        str(HW / "hddsynthpro.kicad_sch")], check=True, capture_output=True)
        root = parse(out.read_text())
    parts = {}
    for c in find(first(root, "components"), "comp"):
        if "exclude_from_bom" in {first(p, "name")[1] for p in find(c, "property")}:
            continue  # JP1, a solder jumper, isn't a part to buy
        fields = {f[1][1]: (f[2] if len(f) > 2 else "") for f in find(first(c, "fields"), "field")}
        parts[first(c, "ref")[1]] = (first(c, "value")[1], first(c, "footprint")[1].split(":")[1], fields.get("LCSC", ""))
    return parts


def footprints():
    """ref -> (x, y, rotation, side) of each footprint's origin on the board (mm, y down)."""
    root = parse((HW / "hddsynthpro.kicad_pcb").read_text())
    out = {}
    for fp in find(root, "footprint"):
        ref = next((p[2] for p in find(fp, "property") if p[1] == "Reference"), None)
        at = first(fp, "at")
        out[ref] = (float(at[1]), float(at[2]), float(at[3]) if len(at) > 3 else 0.0,
                    "Bottom" if first(fp, "layer")[1] == "B.Cu" else "Top")
    return out


def turn(x, y, deg):
    """Turn (x, y) counterclockwise on screen (y down), as KiCad turns footprints."""
    a = math.radians(deg)
    return x * math.cos(a) + y * math.sin(a), -x * math.sin(a) + y * math.cos(a)


def ref_key(ref):
    head = ref.rstrip("0123456789")
    return head, int(ref[len(head):] or 0)


def main():
    parts = {r: p for r, p in netlist().items() if r not in HAND_FIT}
    missing = sorted((r for r, p in parts.items() if not p[2]), key=ref_key)
    if missing:
        sys.exit(f"no LCSC field on {missing}: add them to LCSC in tools/gen_schematic.py")
    FAB.mkdir(exist_ok=True)

    groups = defaultdict(list)
    for ref, (value, footprint, lcsc) in parts.items():
        groups[(lcsc, value, footprint)].append(ref)
    with (FAB / "bom-jlcpcb.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Comment", "Designator", "Footprint", "JLCPCB Part #"])
        for (lcsc, value, footprint), refs in sorted(groups.items(), key=lambda g: ref_key(min(g[1], key=ref_key))):
            w.writerow([value, ",".join(sorted(refs, key=ref_key)), footprint, lcsc])

    pos = footprints()
    with (FAB / "cpl-jlcpcb.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
        for ref in sorted(parts, key=ref_key):
            x, y, rot, side = pos[ref]
            theta, tx, ty = JLC_FIX.get(parts[ref][1], (0, 0.0, 0.0))
            dx, dy = turn(tx, ty, rot)
            # JLCPCB's y points up, so the board's y is negated, as in KiCad's position files.
            w.writerow([ref, f"{x + dx:.3f}mm", f"{-(y + dy):.3f}mm", side, f"{(rot + theta) % 360:.0f}"])

    print(f"wrote fab/bom-jlcpcb.csv ({len(groups)} lines) and fab/cpl-jlcpcb.csv ({len(parts)} parts)")
    for ref, why in HAND_FIT.items():
        print(f"  not ordered: {ref}: {why}")


if __name__ == "__main__":
    main()
