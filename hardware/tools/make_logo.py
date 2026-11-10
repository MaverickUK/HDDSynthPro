"""Turns media/IDESynthBay.png into the board's silkscreen logo:
HDDSynthPro.pretty/LOGO_IDESynthBay.kicad_mod (the library keeps the project's old name).

Run with KiCad's own Python (it has Pillow and pcbnew):
  "C:/Program Files/KiCad/10.0/bin/python.exe" hardware/tools/make_logo.py [height_mm] [--png preview.png]

Silkscreen is one colour, so dark pixels become silk and everything else stays bare: the discs'
grey shading and the shadow under the drive drop out, leaving their black outlines. Silkscreen
prints nothing under about 0.15 mm, so the art is simplified for it:
- "BAY" is drawn in thin vertical bars (6 pixels, 5 apart: 0.11 and 0.09 mm at the board's size),
  so in its rows the bars are closed into solid letters; the faded bars after it drop out with the
  grey, and its red bar (a cursor) is left out, as it would only thicken the B;
- the image is sampled at PX mm per pixel and every stroke widened by a pixel each side, so the
  thin "Synth" prints.
"""
import sys
import uuid
from pathlib import Path

import pcbnew
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from sexpr import Sym as S, dump  # noqa: E402

HW = Path(__file__).resolve().parent.parent
SRC = HW.parent / "media" / "IDESynthBay.png"
NAME = "LOGO_IDESynthBay"
OUT = HW / "HDDSynthPro.pretty" / f"{NAME}.kicad_mod"
PX = 0.04          # mm per pixel
DARK = 128         # luminance below this is silk
BAY_FROM = 660     # source rows from here down are "BAY"
BAY_GAP = 6        # source pixels between its bars, closed


def ink(height_mm):
    """Rows of booleans (True: silkscreen), height_mm tall on the PX grid."""
    im = Image.open(SRC).convert("RGB")
    w, h = im.size
    src = im.load()
    dark = [[False] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            r, g, b = src[x, y]
            red = r - max(g, b) > 80
            dark[y][x] = not red and (0.299 * r + 0.587 * g + 0.114 * b) < DARK
    # "BAY": close the gaps between bars along each row (a gap no wider than BAY_GAP between two dark
    # pixels is filled), which joins each letter's bars into solid strokes.
    for y in range(BAY_FROM, h):
        row = dark[y]
        last = None
        for x in range(w):
            if row[x]:
                if last is not None and 1 < x - last <= BAY_GAP + 1:
                    for k in range(last + 1, x):
                        row[k] = True
                last = x
    mask = Image.new("L", (w, h))
    mask.putdata([255 if v else 0 for row in dark for v in row])
    mask = mask.crop(mask.getbbox())  # to the ink: the image has white margins
    w, h = mask.size
    rows = round(height_mm / PX)
    cols = round(w * rows / h)
    small = mask.resize((cols, rows), Image.LANCZOS).load()
    thin = [[small[x, y] >= 128 for x in range(cols)] for y in range(rows)]
    # Widen strokes by one pixel in each direction.
    return [[any(thin[y + dy][x + dx] for dy in (-1, 0, 1) for dx in (-1, 0, 1)
                 if 0 <= y + dy < rows and 0 <= x + dx < cols) for x in range(cols)] for y in range(rows)]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    height_mm = float(args[0]) if args else 15.0
    fat = ink(height_mm)
    h, w = len(fat), len(fat[0])
    if "--png" in sys.argv:
        out = sys.argv[sys.argv.index("--png") + 1]
        Image.frombytes("L", (w, h), bytes(0 if v else 255 for row in fat for v in row)).save(out)

    poly = pcbnew.SHAPE_POLY_SET()
    x0, y0 = -w * PX / 2, -h * PX / 2
    for y, row in enumerate(fat):
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
    poly.Fracture()

    def u(*k):
        return [S("uuid"), str(uuid.uuid5(uuid.NAMESPACE_URL, "hddsynthpro/logo-isb/" + "/".join(map(str, k))))]

    fx = [S("effects"), [S("font"), [S("size"), 1, 1], [S("thickness"), 0.15]]]
    fp = [S("footprint"), NAME, [S("version"), 20241229], [S("generator"), "hddsynthpro_gen"],
          [S("generator_version"), "1.0"], [S("layer"), "F.Cu"],
          [S("descr"), f"IDE Synth Bay logo, silkscreen, {w * PX:.1f} x {h * PX:.1f} mm, from media/IDESynthBay.png"],
          [S("tags"), "logo"],
          [S("property"), "Reference", "LOGO**", [S("at"), 0, -h * PX / 2 - 1.5, 0], [S("layer"), "F.SilkS"],
           [S("hide"), S("yes")], u("ref"), fx],
          [S("property"), "Value", NAME, [S("at"), 0, h * PX / 2 + 1.5, 0], [S("layer"), "F.Fab"],
           [S("hide"), S("yes")], u("val"), fx],
          [S("attr"), S("board_only"), S("exclude_from_pos_files"), S("exclude_from_bom")]]
    for i in range(poly.OutlineCount()):
        ol = poly.Outline(i)
        pts = [S("pts")] + [[S("xy"), round(pcbnew.ToMM(ol.CPoint(k).x), 4), round(pcbnew.ToMM(ol.CPoint(k).y), 4)]
                            for k in range(ol.PointCount())]
        fp.append([S("fp_poly"), pts, [S("stroke"), [S("width"), 0], [S("type"), S("solid")]], [S("fill"), S("yes")],
                   [S("layer"), "F.SilkS"], u("poly", i)])
    fp.append([S("embedded_fonts"), S("no")])
    OUT.write_text(dump(fp) + "\n")
    print(f"wrote {OUT.name}: {w * PX:.1f} x {h * PX:.1f} mm, {poly.OutlineCount()} polygons")


if __name__ == "__main__":
    main()
