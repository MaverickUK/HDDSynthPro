"""Turns the easter eggs for the back of the board into silkscreen footprints:
media/Defrag.png -> HDDSynthPro:EGG_Defrag and media/ParkHeads.png -> HDDSynthPro:EGG_ParkHeads.

Run with KiCad's own Python (it has Pillow and pcbnew):
  "C:/Program Files/KiCad/10.0/bin/python.exe" hardware/tools/make_eggs.py [--png DIR]

Both are clean black-on-white art, so each is only scaled to its width on a PX grid and
thresholded: black prints as silkscreen, so on the board they show in reverse (white, the
CAUTION band solid). The widths keep their finest gaps printable (silkscreen loses gaps
under about 0.15 mm): Defrag's between its blocks are 12 of 1,890 pixels, 0.15 mm at
24 mm; Park Heads' between its letters 15 of 1,418, 0.21 mm at 20 mm. --png writes what
was thresholded, to check by eye.

The footprints are board-only (not in the BOM or the placement file), drawn on F.SilkS:
layout_pcb.py places them and flips them onto the back, so they read from there.
"""
import sys
import uuid
from pathlib import Path

import pcbnew
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from sexpr import Sym as S, dump  # noqa: E402

HW = Path(__file__).resolve().parent.parent
MEDIA = HW.parent / "media"
PX = 0.025         # mm per pixel
THRESHOLD = 128    # luminance under this is ink
EGGS = [  # name, source, width in mm, description
    ("EGG_Defrag", "Defrag.png", 24.0, "Defragmenting C: ... 42% (an easter egg on the back)"),
    ("EGG_ParkHeads", "ParkHeads.png", 20.0, "Caution: park heads before moving (an easter egg on the back)"),
]


def bitmap(src, width_mm):
    """Rows of booleans (True: silkscreen), scaled to width_mm on the PX grid."""
    im = Image.open(src).convert("L")
    w = round(width_mm / PX)
    h = round(im.height * w / im.width)
    im = im.resize((w, h), Image.LANCZOS)
    px = im.load()
    return [[px[x, y] < THRESHOLD for x in range(w)] for y in range(h)]


def polygons(ink):
    """The ink as KiCad polygons, centred on the origin: each row's runs merged by Simplify()."""
    h, w = len(ink), len(ink[0])
    poly = pcbnew.SHAPE_POLY_SET()
    x0, y0 = -w * PX / 2, -h * PX / 2
    for y, row in enumerate(ink):
        x = 0
        while x < w:
            if not row[x]:
                x += 1
                continue
            start = x
            while x < w and row[x]:
                x += 1
            poly.NewOutline()
            for px, py in ((start, y), (x, y), (x, y + 1), (start, y + 1)):
                poly.Append(pcbnew.FromMM(x0 + px * PX), pcbnew.FromMM(y0 + py * PX))
    poly.Simplify()
    poly.Fracture()  # holes (the letters' counters) joined to their outlines, as fp_poly needs
    return poly


def footprint(name, descr, poly, w_mm, h_mm):
    def u(*k):
        return [S("uuid"), str(uuid.uuid5(uuid.NAMESPACE_URL, f"hddsynthpro/{name}/" + "/".join(map(str, k))))]

    fx = [S("effects"), [S("font"), [S("size"), 1, 1], [S("thickness"), 0.15]]]
    fp = [S("footprint"), name, [S("version"), 20241229], [S("generator"), "hddsynthpro_gen"],
          [S("generator_version"), "1.0"], [S("layer"), "F.Cu"],
          [S("descr"), f"{descr}, silkscreen, {w_mm:.1f} x {h_mm:.1f} mm"], [S("tags"), "logo"],
          [S("property"), "Reference", "EGG**", [S("at"), 0, -h_mm / 2 - 1.5, 0], [S("layer"), "F.SilkS"],
           [S("hide"), S("yes")], u("ref"), fx],
          [S("property"), "Value", name, [S("at"), 0, h_mm / 2 + 1.5, 0], [S("layer"), "F.Fab"],
           [S("hide"), S("yes")], u("val"), fx],
          [S("attr"), S("board_only"), S("exclude_from_pos_files"), S("exclude_from_bom")]]
    for i in range(poly.OutlineCount()):
        ol = poly.Outline(i)
        pts = [S("pts")] + [[S("xy"), round(pcbnew.ToMM(ol.CPoint(k).x), 4), round(pcbnew.ToMM(ol.CPoint(k).y), 4)]
                            for k in range(ol.PointCount())]
        fp.append([S("fp_poly"), pts, [S("stroke"), [S("width"), 0], [S("type"), S("solid")]], [S("fill"), S("yes")],
                   [S("layer"), "F.SilkS"], u("poly", i)])
    fp.append([S("embedded_fonts"), S("no")])
    return dump(fp) + "\n"


def main():
    png_dir = Path(sys.argv[sys.argv.index("--png") + 1]) if "--png" in sys.argv else None
    for name, src, width, descr in EGGS:
        ink = bitmap(MEDIA / src, width)
        h, w = len(ink), len(ink[0])
        if png_dir:
            Image.frombytes("L", (w, h), bytes(0 if v else 255 for row in ink for v in row)).save(png_dir / f"{name}.png")
        poly = polygons(ink)
        out = HW / "HDDSynthPro.pretty" / f"{name}.kicad_mod"
        out.write_text(footprint(name, descr, poly, w * PX, h * PX))
        print(f"wrote {out.name}: {w * PX:.1f} x {h * PX:.1f} mm, {poly.OutlineCount()} polygons")


if __name__ == "__main__":
    main()
