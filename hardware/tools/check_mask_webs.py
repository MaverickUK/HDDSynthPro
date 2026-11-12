"""Fails if a board's solder mask has a strip narrower than a fab can make in black.

Between two mask openings closer than MIN_WEB_MM, PCBWay can't make the strip of black mask
(the "bridge") and asks whether to leave it out (11 Nov 2026, the IDE Synth Bay's RP2350B:
0.4 mm pitch, 0.2 mm strips). Fine-pitch parts need a footprint with one opening per row of
pads instead (allow_soldermask_bridges, one F.Mask rectangle over the row), or a board in
green, which allows thinner strips.

Run with KiCad's own Python, which has the pcbnew module:
  /Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 \\
      hardware/tools/check_mask_webs.py hardware/hddsynthpro.kicad_pcb [--min 0.22]

It merges every opening on each mask layer (pads with their mask margin, mask drawings in
footprints and on the board, and vias that aren't tented), then measures the gap between
each pair of openings. Exit status 1 if any gap is under the minimum.
"""
import argparse
import os
import sys

import pcbnew

MIN_WEB_MM = 0.22  # PCBWay's narrowest strip in black mask; green, blue and red allow less


def openings(board, layer):
    """Every mask opening on `layer`, merged, and which parts each one belongs to."""
    err = pcbnew.FromMM(0.005)
    pieces = []  # (polygon set, owner)
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            if pad.IsOnLayer(layer):
                ps = pcbnew.SHAPE_POLY_SET()
                pad.TransformShapeToPolygon(ps, layer, pad.GetSolderMaskExpansion(layer), err, pcbnew.ERROR_INSIDE)
                pieces.append((ps, f"{fp.GetReference()}.{pad.GetNumber() or 'hole'}"))
        for item in fp.GraphicalItems():
            if item.GetLayer() == layer:
                ps = pcbnew.SHAPE_POLY_SET()
                item.TransformShapeToPolygon(ps, layer, 0, err, pcbnew.ERROR_INSIDE)
                pieces.append((ps, fp.GetReference()))
    for item in board.GetDrawings():
        if item.GetLayer() == layer:
            ps = pcbnew.SHAPE_POLY_SET()
            item.TransformShapeToPolygon(ps, layer, 0, err, pcbnew.ERROR_INSIDE)
            pieces.append((ps, "board"))
    for track in board.GetTracks():
        if isinstance(track, pcbnew.PCB_VIA) and track.IsOnLayer(layer):  # only vias left untented
            ps = pcbnew.SHAPE_POLY_SET()
            track.TransformShapeToPolygon(ps, layer, track.GetSolderMaskExpansion(), err, pcbnew.ERROR_INSIDE)
            pieces.append((ps, "via"))
    merged = pcbnew.SHAPE_POLY_SET()
    for ps, _ in pieces:
        merged.Append(ps)
    merged.Simplify()
    outlines = [pcbnew.SHAPE_LINE_CHAIN(merged.COutline(i)) for i in range(merged.OutlineCount())]  # copies: merged goes
    owners = [set() for _ in outlines]
    for ps, owner in pieces:
        c = ps.BBox().Centre()
        for i, o in enumerate(outlines):
            if o.PointInside(c) or o.PointOnEdge(c):
                owners[i].add(owner)
                break
    return outlines, owners


def gap(a, b):
    """The distance between two separate polygons: from a vertex of one to an edge of the other."""
    d = min(a.SquaredDistance(b.CPoint(k)) for k in range(b.PointCount()))
    return min(d, min(b.SquaredDistance(a.CPoint(k)) for k in range(a.PointCount()))) ** 0.5


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("board")
    ap.add_argument("--min", type=float, default=MIN_WEB_MM, help="narrowest strip allowed, mm (default %(default)s)")
    args = ap.parse_args()
    board = pcbnew.LoadBoard(args.board)
    limit = pcbnew.FromMM(args.min)
    bad, narrowest = [], None
    for layer in (pcbnew.F_Mask, pcbnew.B_Mask):
        outlines, owners = openings(board, layer)
        boxes = [o.BBox() for o in outlines]
        for i in range(len(outlines)):
            grown = pcbnew.BOX2I(boxes[i].GetPosition(), boxes[i].GetSize())
            grown.Inflate(limit)
            for j in range(i + 1, len(outlines)):
                if not grown.Intersects(boxes[j]):
                    continue
                g = gap(outlines[i], outlines[j])
                narrowest = g if narrowest is None else min(narrowest, g)
                if g < limit:
                    who = sorted({o.split(".")[0] for o in owners[i] | owners[j]})
                    bad.append((pcbnew.ToMM(g), board.GetLayerName(layer), ", ".join(who)))
    worst = {}
    for g, layer, who in bad:
        k = (layer, who)
        worst[k] = (min(worst.get(k, (g, 0))[0], g), worst.get(k, (g, 0))[1] + 1)
    for (layer, who), (g, n) in sorted(worst.items(), key=lambda kv: kv[1][0]):
        print(f"{layer}: {n} mask strip(s) down to {g:.3f} mm, under {args.min} mm: {who}")
    if bad:
        print(f"narrowest mask strip: {pcbnew.ToMM(narrowest):.3f} mm")
    else:
        print(f"OK: no mask strip under {args.min} mm")
    sys.stdout.flush()
    os._exit(1 if bad else 0)  # pcbnew can crash as Python exits, losing the result


if __name__ == "__main__":
    main()
