"""Turns media/HDDSynthProWide3.png into a silkscreen logo footprint, HDDSynthPro:LOGO_HDDSynthPro.

Run with KiCad's own Python (macOS: it uses `sips` to scale the image):
  /Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 \
      hardware/tools/make_logo.py [height_mm]

Silkscreen is one colour, so dark pixels become silk and everything else stays bare
(the grey disc and the light bars behind "PRO" drop out). The image is sampled at
0.05 mm per pixel and its dark strokes widened by one pixel each side, so the thin
arcs come out about 0.15 mm, which fabs can print.
"""
import struct
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path

import pcbnew

sys.path.insert(0, str(Path(__file__).resolve().parent))
from sexpr import Sym as S, dump  # noqa: E402

HW = Path(__file__).resolve().parent.parent
SRC = HW.parent / "media" / "HDDSynthProWide3.png"
OUT = HW / "HDDSynthPro.pretty" / "LOGO_HDDSynthPro.kicad_mod"
PX = 0.05      # mm per pixel
DARK = 128     # luminance below this is silk


def read_bmp(path):
    """Rows of luminance (0-255), top row first, from an uncompressed 24- or 32-bit BMP."""
    data = path.read_bytes()
    offset, = struct.unpack_from("<I", data, 10)
    width, height, _, bpp, compression = struct.unpack_from("<iiHHI", data, 18)
    if compression not in (0, 3) or bpp not in (24, 32):
        raise ValueError(f"unsupported BMP: {bpp} bpp, compression {compression}")
    step = bpp // 8
    stride = (width * step + 3) & ~3
    rows = []
    for r in range(abs(height)):
        start = offset + r * stride
        row = [int(0.114 * data[start + c * step] + 0.587 * data[start + c * step + 1]
                   + 0.299 * data[start + c * step + 2]) for c in range(width)]
        rows.append(row)
    return rows if height < 0 else rows[::-1]


def main():
    height_mm = float(sys.argv[1]) if len(sys.argv) > 1 else 16.0
    with tempfile.TemporaryDirectory() as tmp:
        bmp = Path(tmp) / "logo.bmp"
        rows_px = round(height_mm / PX)
        subprocess.run(["sips", "--resampleHeight", str(rows_px), "-s", "format", "bmp", str(SRC), "--out", str(bmp)],
                       check=True, capture_output=True)
        lum = read_bmp(bmp)
    h, w = len(lum), len(lum[0])
    dark = [[v < DARK for v in row] for row in lum]
    # Widen strokes by one pixel in each direction.
    fat = [[any(dark[y + dy][x + dx] for dy in (-1, 0, 1) for dx in (-1, 0, 1)
                if 0 <= y + dy < h and 0 <= x + dx < w) for x in range(w)] for y in range(h)]

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
        return [S("uuid"), str(uuid.uuid5(uuid.NAMESPACE_URL, "hddsynthpro/logo/" + "/".join(map(str, k))))]

    fx = [S("effects"), [S("font"), [S("size"), 1, 1], [S("thickness"), 0.15]]]
    fp = [S("footprint"), "LOGO_HDDSynthPro", [S("version"), 20241229], [S("generator"), "hddsynthpro_gen"],
          [S("generator_version"), "1.0"], [S("layer"), "F.Cu"],
          [S("descr"), f"HDD Synth Pro logo, silkscreen, {w * PX:.1f} x {h * PX:.1f} mm, from media/HDDSynthProWide3.png"],
          [S("tags"), "logo"],
          [S("property"), "Reference", "LOGO**", [S("at"), 0, -h * PX / 2 - 1.5, 0], [S("layer"), "F.SilkS"],
           [S("hide"), S("yes")], u("ref"), fx],
          [S("property"), "Value", "LOGO_HDDSynthPro", [S("at"), 0, h * PX / 2 + 1.5, 0], [S("layer"), "F.Fab"],
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
