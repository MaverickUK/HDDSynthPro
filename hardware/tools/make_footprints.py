#!/usr/bin/env python3
"""Writes the footprints KiCad's libraries don't have, into HDDSynthPro.pretty.

Usage: python3 hardware/tools/make_footprints.py [path/to/RPI-RP2350A-MINIMAL_R4-S1.kicad_pcb]
(without Raspberry Pi's board, only the footprints drawn here: the microSD socket, the Molex header and
the two IDE headers with pin 20 unplated)

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
- Molex_8981_1x04_P5.08mm_Vertical: a PC disk-power header (rev 0.4), Molex 8981 style:
  four pins on 5.08 mm, the housing's two chamfered corners drawn on the +y side
  (the keyed side). Pin 1 is +12 V. Check the drill and outline against the part
  chosen before ordering: KiCad has no footprint for it.
- QFN-80-1EP_10x10mm_P0.4mm_EP3.4x3.4mm_ThermalVias_OneMaskOpening (the RP2350B) and
  USB_C_Receptacle_HRO_TYPE-C-31-M-12_OneMaskOpening: KiCad's, with one solder mask
  opening per row of fine-pitch pads. Their pads are 0.2 mm apart, too close for a strip
  of black mask between them (PCBWay, 11 Nov 2026).
"""
import os
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


def molex_8981():
    name = "Molex_8981_1x04_P5.08mm_Vertical"
    fp = header(name, "PC disk-drive power header, Molex 8981 style (e.g. Molex 15-24-4041), 4 pins, 5.08 mm, "
                      "vertical. Pin 1 +12 V, 2-3 GND, 4 +5 V. Chamfered corners on the +y side.",
                "Molex 8981 disk power 5.08mm")
    fp += [[S("property"), "Reference", "REF**", [S("at"), 7.62, -5.0, 0], [S("layer"), "F.SilkS"], u(name, "ref"), fx()],
           [S("property"), "Value", name, [S("at"), 7.62, 5.6, 0], [S("layer"), "F.Fab"], u(name, "val"), fx(0.8, 0.12)],
           [S("attr"), S("through_hole")]]
    # Housing about 21.3 x 7.6 mm round the pins (pin 1 at the origin), its +y corners chamfered.
    x0, x1, y0, y1, c = -2.97, 18.21, -3.8, 3.8, 1.6

    def outline(layer, grow, w):
        pts = [(x0 - grow, y0 - grow), (x1 + grow, y0 - grow), (x1 + grow, y1 + grow - c),
               (x1 + grow - c, y1 + grow), (x0 - grow + c, y1 + grow), (x0 - grow, y1 + grow - c)]
        return [S("fp_poly"), [S("pts")] + [[S("xy"), round(x, 3), round(y, 3)] for x, y in pts],
                [S("stroke"), [S("width"), w], [S("type"), S("solid")]], [S("fill"), S("no")],
                [S("layer"), layer], u(name, layer)]

    fp += [outline("F.Fab", 0, 0.1), outline("F.SilkS", 0.11, 0.12), outline("F.CrtYd", 0.5, 0.05)]
    fp.append([S("fp_text"), S("user"), "${REFERENCE}", [S("at"), 7.62, 0, 0], [S("layer"), "F.Fab"], u(name, "rt"), fx()])
    for n in range(4):
        fp.append([S("pad"), str(n + 1), S("thru_hole"), S("rect" if n == 0 else "circle"), [S("at"), round(5.08 * n, 2), 0],
                   [S("size"), 3.0, 3.0], [S("drill"), 1.8], [S("layers"), "*.Cu", "*.Mask"],
                   [S("remove_unused_layers"), S("no")], u(name, "pad", n + 1)])
    fp.append([S("embedded_fonts"), S("no")])
    (PRETTY / f"{name}.kicad_mod").write_text(dump(fp) + "\n")
    return name


KICAD_FOOTPRINTS = Path(os.environ.get("KICAD_SHARE", "/Applications/KiCad/KiCad.app/Contents/SharedSupport"
                                        if sys.platform == "darwin" else "C:/Program Files/KiCad/10.0/share/kicad")) / "footprints"


def keyed(lib, name, ring):
    """KiCad's header footprint with pin 20, IDE's key, as an unplated hole (rev 0.4).

    The headers JLCPCB stocks have a pin 20, so the hole stays for it, but nothing solders
    it: after assembly it pushes out of the plastic with pliers, for keyed cables, instead of
    being cut flush. A silkscreen ring marks it. `ring` is the ring's radius, clear of the
    neighbouring pads.
    """
    src = parse((KICAD_FOOTPRINTS / f"{lib}.pretty" / f"{name}.kicad_mod").read_text(encoding="utf-8"))
    new = name + "_Pin20Unplated"
    fp = [S("footprint"), new]
    for item in src[2:]:
        if isinstance(item, list) and item and item[0] == "descr":
            item = [S("descr"), item[1] + ". Pin 20 (IDE's key) an unplated hole, ringed: pull the pin out after assembly"]
        elif isinstance(item, list) and item and item[0] == "pad" and item[1] == "20":
            at, drill = first(item, "at"), first(item, "drill")[1]
            item = [S("pad"), "20", S("np_thru_hole"), S("circle"), at, [S("size"), drill, drill], [S("drill"), drill],
                    [S("layers"), "*.Cu", "*.Mask"], u(new, "pad20")]
            fp.append(item)
            cx, cy = float(at[1]), float(at[2])
            fp.append([S("fp_circle"), [S("center"), cx, cy], [S("end"), round(cx + ring, 3), cy],
                       [S("stroke"), [S("width"), 0.12], [S("type"), S("solid")]], [S("fill"), S("no")],
                       [S("layer"), "F.SilkS"], u(new, "ring20")])
            continue
        fp.append(item)
    (PRETTY / f"{new}.kicad_mod").write_text(dump(fp) + "\n")
    return new


MASK_WEB_MIN = 0.22  # mm: the narrowest solder mask strip PCBWay makes in black (11 Nov 2026)


def _mask_box(pad):
    """A pad's outline (x0, y0, x1, y1) on the mask: its copper, as the board adds no margin."""
    at, size = first(pad, "at"), first(pad, "size")
    x, y, w, h = float(at[1]), float(at[2]), float(size[1]), float(size[2])
    if len(at) > 3 and round(float(at[3])) % 180 == 90:
        w, h = h, w
    return [x - w / 2, y - h / 2, x + w / 2, y + h / 2]


def one_mask_opening(lib, name):
    """KiCad's footprint with one solder mask opening per row of pads too close for a strip.

    Between pads closer than MASK_WEB_MIN, a fab can't make the strip of mask (the "bridge")
    in black, and asks what to do. Here each such row gets one opening, an F.Mask rectangle
    over the row, so the Gerbers never ask for a strip that thin, whatever the colour. The
    footprint allows mask bridges (KiCad's attribute), so DRC doesn't flag pads of different
    nets sharing an opening. The copper and the paste are KiCad's, unchanged.
    """
    src = parse((KICAD_FOOTPRINTS / f"{lib}.pretty" / f"{name}.kicad_mod").read_text(encoding="utf-8"))
    pads = [p for p in find(src, "pad") if '"F.Mask"' in dump(first(p, "layers")) and p[2] == "smd"]
    boxes = [_mask_box(p) for p in pads]
    # Group pads whose mask strip would be too thin (or that overlap, as the USB-C's A1/B12 do).
    group = list(range(len(boxes)))

    def root(i):
        while group[i] != i:
            i = group[i]
        return i

    for i, a in enumerate(boxes):
        for j in range(i + 1, len(boxes)):
            b = boxes[j]
            dx, dy = max(b[0] - a[2], a[0] - b[2], 0), max(b[1] - a[3], a[1] - b[3], 0)
            if (dx * dx + dy * dy) ** 0.5 < MASK_WEB_MIN:
                group[root(j)] = root(i)
    rows = {}
    for i, b in enumerate(boxes):
        r = rows.setdefault(root(i), [b[0], b[1], b[2], b[3], 0])
        r[:4] = [min(r[0], b[0]), min(r[1], b[1]), max(r[2], b[2]), max(r[3], b[3])]
        r[4] += 1
    rows = sorted((r for r in rows.values() if r[4] > 1), key=lambda r: (round(r[1], 3), round(r[0], 3)))

    new = name + "_OneMaskOpening"
    fp = [S("footprint"), new]
    for item in src[2:]:
        if isinstance(item, list) and item and item[0] == "descr":
            item = [S("descr"), item[1] + f". One solder mask opening per row of pads closer than {MASK_WEB_MIN} mm"
                    " (no mask strips between them)"]
        elif isinstance(item, list) and item and item[0] == "attr":
            item = item + [S("allow_soldermask_bridges")]
        fp.append(item)
        if isinstance(item, list) and item and item[0] == "attr":
            for k, (x0, y0, x1, y1, _) in enumerate(rows):
                fp.append([S("fp_rect"), [S("start"), round(x0, 4), round(y0, 4)], [S("end"), round(x1, 4), round(y1, 4)],
                           [S("stroke"), [S("width"), 0], [S("type"), S("solid")]], [S("fill"), S("yes")],
                           [S("layer"), "F.Mask"], u(new, "mask", k)])
    (PRETTY / f"{new}.kicad_mod").write_text(dump(fp) + "\n")
    return new


def main():
    drawn = [tf_push(), molex_8981(), keyed("Connector_IDC", "IDC-Header_2x20_P2.54mm_Vertical", 1.15),
             keyed("Connector_PinHeader_2.00mm", "PinHeader_2x22_P2.00mm_Vertical", 0.95),
             one_mask_opening("Package_DFN_QFN", "QFN-80-1EP_10x10mm_P0.4mm_EP3.4x3.4mm_ThermalVias"),
             one_mask_opening("Connector_USB", "USB_C_Receptacle_HRO_TYPE-C-31-M-12")]
    if len(sys.argv) == 1:
        print("wrote", ", ".join(drawn))
        return
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    print("wrote", inductor(sys.argv[1]), wide_0402(sys.argv[1]), ", ".join(drawn))


if __name__ == "__main__":
    main()
