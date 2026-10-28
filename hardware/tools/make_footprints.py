#!/usr/bin/env python3
"""Writes the two footprints KiCad's libraries don't have, into HDDSynthPro.pretty.

Usage: python3 hardware/tools/make_footprints.py path/to/RPI-RP2350A-MINIMAL_R4-S1.kicad_pcb

- C_0402_1005Metric_RPi_Wide: Raspberry Pi's wide-pad 0402, for the two capacitors
  either side of the VREG_LX strip in their regulator layout.

- L_Abracon_AOTA-B201610S3R3: the RP2350 core regulator's inductor, copied from
  Raspberry Pi's RP2350 minimal design (MIT licence; Minimal-KiCAD.zip from
  datasheets.raspberrypi.com). Raspberry Pi require this polarity-marked part,
  fitted with its dot as in their layout: pad 1 to the 1.1 V side.
- microSD_TF-PUSH: the generic push-push microSD socket, JLCPCB C393941, from
  JLCPCB's own footprint (2026-10-27). Contacts 1-8 in the standard microSD
  order, 9 card detect, SH the four shell tabs, two 1.2 mm locating holes.
  The card goes in from the +y edge.
"""
import sys
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from sexpr import Sym as S, dump, find, first, parse  # noqa: E402

PRETTY = Path(__file__).resolve().parent.parent / "HDDSynthPro.pretty"


def u(*k):
    return [S("uuid"), str(uuid.uuid5(uuid.NAMESPACE_URL, "hddsynthpro/fp/" + "/".join(map(str, k))))]


def fx(size=1.0, thick=0.15):
    return [S("effects"), [S("font"), [S("size"), size, size], [S("thickness"), thick]]]


def header(name, descr, tags):
    return [S("footprint"), name, [S("version"), 20241229], [S("generator"), "hddsynthpro_gen"],
            [S("generator_version"), "1.0"], [S("layer"), "F.Cu"], [S("descr"), descr], [S("tags"), tags]]


def inductor(pi_board):
    board = parse(Path(pi_board).read_text())
    src = next(fp for fp in find(board, "footprint") if "abracon_aota-b201610s3r3-101-t" in fp[1])
    if first(src, "at")[3:] not in ([], [0], [0.0], ["0"]):
        sys.exit("expected Raspberry Pi's inductor unrotated")
    name = "L_Abracon_AOTA-B201610S3R3"
    fp = header(name, "Abracon AOTA-B201610S3R3-101-T, 3.3 uH, 2016, polarity marked. From Raspberry Pi's "
                      "RP2350 minimal design (MIT licence). Pad 1 to the 1.1 V side.", "inductor RP2350 polarity")
    fp += [[S("property"), "Reference", "REF**", [S("at"), 0, -1.55, 0], [S("layer"), "F.SilkS"], u(name, "ref"), fx(0.8, 0.12)],
           [S("property"), "Value", name, [S("at"), 0, 1.6, 0], [S("layer"), "F.Fab"], u(name, "val"), fx(0.6, 0.09)]]
    for item in src[2:]:
        if not isinstance(item, list) or item[0] in ("layer", "uuid", "at", "property", "path", "sheetname", "sheetfile", "descr", "tags"):
            continue
        if item[0] == "pad":
            item = [x for x in item if not (isinstance(x, list) and x and x[0] in ("net", "pinfunction", "pintype", "uuid"))]
            item.append(u(name, "pad", item[1]))
        elif isinstance(item, list) and any(isinstance(x, list) and x and x[0] == "uuid" for x in item):
            item = [x if not (isinstance(x, list) and x and x[0] == "uuid") else u(name, dump(item)[:60]) for x in item]
        fp.append(item)
    (PRETTY / f"{name}.kicad_mod").write_text(dump(fp) + "\n")
    return name


def wide_0402(pi_board):
    """Raspberry Pi's 'wide' 0402 capacitor: pads 0.47 x 0.55 at +-0.515 mm. Their regulator
    layout needs these beside the VREG_LX strip; KiCad's 0402 pads come 0.08 mm closer."""
    board = parse(Path(pi_board).read_text())
    src = next(fp for fp in find(board, "footprint") if fp[1].endswith("generic_capc1005x50_wide"))
    name = "C_0402_1005Metric_RPi_Wide"
    fp = header(name, "0402 capacitor with Raspberry Pi's wide pad layout (RP2350 minimal design, MIT licence).",
                "capacitor 0402 RP2350")
    fp += [[S("property"), "Reference", "REF**", [S("at"), 0, -1.0, 0], [S("layer"), "F.SilkS"], u(name, "ref"), fx(0.6, 0.1)],
           [S("property"), "Value", name, [S("at"), 0, 1.0, 0], [S("layer"), "F.Fab"], u(name, "val"), fx(0.5, 0.08)],
           [S("attr"), S("smd")]]
    for item in src[2:]:
        if not isinstance(item, list) or item[0] in ("layer", "uuid", "at", "property", "path", "sheetname", "sheetfile",
                                                     "descr", "tags", "attr"):
            continue
        if item[0] == "pad":
            item = [x for x in item if not (isinstance(x, list) and x and x[0] in ("net", "pinfunction", "pintype", "uuid"))]
            item.append(u(name, "pad", item[1]))
        elif any(isinstance(x, list) and x and x[0] == "uuid" for x in item):
            item = [x if not (isinstance(x, list) and x and x[0] == "uuid") else u(name, dump(item)[:60]) for x in item]
        fp.append(item)
    (PRETTY / f"{name}.kicad_mod").write_text(dump(fp) + "\n")
    return name


def tf_push():
    name = "microSD_TF-PUSH"
    fp = header(name, "Generic push-push microSD socket 'TF PUSH' (JLCPCB C393941), from JLCPCB's footprint. "
                      "Card enters from the +y edge.", "microSD TF push")
    fp += [[S("property"), "Reference", "REF**", [S("at"), 0, -7.3, 0], [S("layer"), "F.SilkS"], u(name, "ref"), fx()],
           [S("property"), "Value", name, [S("at"), 0, 2.0, 0], [S("layer"), "F.Fab"], u(name, "val"), fx()],
           [S("attr"), S("smd")]]

    def rect(layer, x0, y0, x1, y1, w, tag):
        return [S("fp_rect"), [S("start"), x0, y0], [S("end"), x1, y1], [S("stroke"), [S("width"), w], [S("type"), S("solid")]],
                [S("fill"), S("no")], [S("layer"), layer], u(name, tag)]

    def line(layer, x0, y0, x1, y1, tag):
        return [S("fp_line"), [S("start"), x0, y0], [S("end"), x1, y1], [S("stroke"), [S("width"), 0.12], [S("type"), S("solid")]],
                [S("layer"), layer], u(name, tag)]

    fp.append(rect("F.Fab", -8.07, -4.81, 8.07, 9.69, 0.1, "fab"))
    fp.append(rect("F.CrtYd", -8.6, -6.35, 8.6, 9.95, 0.05, "crt"))
    # Silkscreen along both sides, clear of the shell tabs.
    for i, (x0, y0, x1, y1) in enumerate([(-8.25, -3.3, -8.25, 3.9), (8.25, -3.3, 8.25, 3.9),
                                         (-8.25, 6.7, -8.25, 9.2), (8.25, 6.7, 8.25, 9.2)]):
        fp.append(line("F.SilkS", x0, y0, x1, y1, f"silk{i}"))
    fp.append([S("fp_text"), S("user"), "${REFERENCE}", [S("at"), 0, 3.0, 0], [S("layer"), "F.Fab"], u(name, "rt"), fx()])

    def pad(num, x, y, w, h):
        return [S("pad"), num, S("smd"), S("rect"), [S("at"), x, y], [S("size"), w, h],
                [S("layers"), "F.Cu", "F.Paste", "F.Mask"], u(name, "pad", num, x, y)]

    for n in range(1, 9):
        fp.append(pad(str(n), round(2.25 - 1.1 * (n - 1), 3), -5.3, 0.7, 1.6))
    fp.append(pad("9", -6.55, -5.3, 0.7, 1.6))
    for x, y, w, h in [(-7.75, -4.3, 1.2, 1.5), (-7.75, 5.3, 1.2, 2.2), (7.75, 5.3, 1.2, 2.2), (6.85, -4.3, 1.6, 1.5)]:
        fp.append(pad("SH", x, y, w, h))
    for x in (-4.95, 3.05):
        fp.append([S("pad"), "", S("np_thru_hole"), S("circle"), [S("at"), x, 5.7], [S("size"), 1.2, 1.2], [S("drill"), 1.2],
                   [S("layers"), "*.Cu", "*.Mask"], u(name, "hole", x)])
    fp.append([S("embedded_fonts"), S("no")])
    (PRETTY / f"{name}.kicad_mod").write_text(dump(fp) + "\n")
    return name


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    print("wrote", inductor(sys.argv[1]), wide_0402(sys.argv[1]), "and", tf_push())


if __name__ == "__main__":
    main()
