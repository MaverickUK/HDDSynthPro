"""Autoroutes the board with Freerouting, then pours ground on the outer layers.

Run with KiCad's own Python (it has the pcbnew module), with the Freerouting CLI
(https://github.com/freerouting/freerouting/releases): FREEROUTING is the
freerouting-cli executable, or the .jar with JAVA pointing at a Java 25 runtime
(the 2.5.0 native CLI crashed on this board, the jar didn't):
  FREEROUTING=/path/to/freerouting-2.5.0.jar JAVA=/path/to/java \
  /Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 \
      hardware/tools/autoroute.py [board.kicad_pcb]
then refill the zones and check:
  kicad-cli pcb drc --refill-zones --save-board --schematic-parity hardware/hddsynthpro.kicad_pcb

The inner layers are the GND and 3V3 planes, so they're handed to Freerouting
as power layers: it connects to them with vias but never routes on them.
Before routing, each surface-mount GND and +3V3 pad gets a locked stub and via to
its plane; after, vias the router left dangling are removed. Existing traces are
kept, so hand-routed parts survive a rerun.
"""
import math
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import pcbnew

HW = Path(__file__).resolve().parent.parent
PASSES = os.environ.get("FREEROUTING_PASSES", "100")
PLANE_NETS = ("GND", "+3V3")
VIA_D, VIA_DRILL, STUB_W, CLEAR = 0.6, 0.3, 0.25, 0.2  # mm; CLEAR has margin over the 0.15 rule


def _mm(v):
    return pcbnew.ToMM(v)


def _rect(item):
    b = item.GetBoundingBox()
    return (_mm(b.GetLeft()), _mm(b.GetTop()), _mm(b.GetRight()), _mm(b.GetBottom()))


def _dist(x, y, o):
    """Distance from (x, y) to an obstacle: a rectangle (x0, y0, x1, y1), or a track
    ("seg", x0, y0, x1, y1, half_width) measured to its centreline less its half-width."""
    if o[0] == "seg":
        _, x0, y0, x1, y1, hw = o
        vx, vy = x1 - x0, y1 - y0
        L = vx * vx + vy * vy
        s = 0.0 if L == 0 else max(0.0, min(1.0, ((x - x0) * vx + (y - y0) * vy) / L))
        return max(0.0, math.hypot(x - (x0 + s * vx), y - (y0 + s * vy)) - hw)
    dx = max(o[0] - x, 0.0, x - o[2])
    dy = max(o[1] - y, 0.0, y - o[3])
    return math.hypot(dx, dy)


def _seg(t):
    s, e = t.GetStart(), t.GetEnd()
    return ("seg", _mm(s.x), _mm(s.y), _mm(e.x), _mm(e.y), _mm(t.GetWidth()) / 2)


# Pads joined to their planes some other way: the RP2350B (its 3V3 ring and exposed pad) and
# the regulator capacitors inside Raspberry Pi's copper fills.
FANOUT_SKIP = {"U1", "C9", "C10"}


def fanout(board):
    """A short stub and a via from every surface-mount GND and +3V3 pad to the inner planes.

    Freerouting won't drop these itself on this board. Each via goes where it clears every
    other net's copper (pads as their bounding boxes, which is conservative), preferring
    short stubs pointing away from the part. The stubs and vias are locked, so the
    router keeps them and routes around them.
    """
    edge = board.GetBoardEdgesBoundingBox()
    ex0, ey0, ex1, ey1 = _mm(edge.GetLeft()), _mm(edge.GetTop()), _mm(edge.GetRight()), _mm(edge.GetBottom())
    obstacles = [(_rect(p), p.GetNetname()) for p in board.GetPads()]
    obstacles += [(_seg(t) if t.GetClass() == "PCB_TRACK" else _rect(t), t.GetNetname())
                  for t in board.GetTracks()]  # fixed copper already there
    obstacles += [(_rect(z), z.GetNetname()) for z in board.Zones() if z.GetLayer() in (pcbnew.F_Cu, pcbnew.B_Cu)
                  and z.GetBoundingBox().GetWidth() < pcbnew.FromMM(20)]
    vias = []  # holes keep apart whatever their net
    added = 0
    for fp in board.GetFootprints():
        cx, cy = _mm(fp.GetPosition().x), _mm(fp.GetPosition().y)
        if fp.GetReference() in FANOUT_SKIP:
            continue
        for pad in fp.Pads():
            net = pad.GetNetname()
            if net not in PLANE_NETS or pad.GetAttribute() != pcbnew.PAD_ATTRIB_SMD:
                continue
            px, py = _mm(pad.GetPosition().x), _mm(pad.GetPosition().y)
            if any(o[0] == "seg" and n == net and (math.hypot(o[1] - px, o[2] - py) < 0.05 or math.hypot(o[3] - px, o[4] - py) < 0.05)
                   for o, n in obstacles):
                continue  # a fixed track already takes this pad to its via
            r = _rect(pad)
            half = max(r[2] - r[0], r[3] - r[1]) / 2
            out = math.atan2(py - cy, px - cx) if (px, py) != (cx, cy) else 0.0
            best = None
            for k in range(16):
                a = k * math.pi / 8
                turn = abs((a - out + math.pi) % (2 * math.pi) - math.pi)
                for step in range(9):
                    d = half + 0.5 + 0.25 * step
                    vx, vy = px + d * math.cos(a), py + d * math.sin(a)
                    if not (ex0 + 0.8 < vx < ex1 - 0.8 and ey0 + 0.8 < vy < ey1 - 0.8):
                        continue
                    if any(n != net and _dist(vx, vy, o) < VIA_D / 2 + CLEAR for o, n in obstacles):
                        continue
                    if any(math.hypot(vx - ox, vy - oy) < VIA_D + CLEAR for ox, oy in vias):
                        continue
                    steps = max(2, int(d / 0.1))
                    if any(n != net and _dist(px + (vx - px) * i / steps, py + (vy - py) * i / steps, o)
                           < STUB_W / 2 + CLEAR for i in range(steps + 1) for o, n in obstacles):
                        continue
                    score = d + 0.6 * turn
                    if best is None or score < best[0]:
                        best = (score, vx, vy)
                    break
            if best is None:
                print(f"fanout: no room for a via at {fp.GetReference()} pad {pad.GetNumber()}")
                continue
            _, vx, vy = best
            netinfo = board.FindNet(net)
            track = pcbnew.PCB_TRACK(board)
            track.SetStart(pad.GetPosition())
            track.SetEnd(pcbnew.VECTOR2I_MM(vx, vy))
            track.SetWidth(pcbnew.FromMM(STUB_W))
            track.SetLayer(pcbnew.F_Cu)
            track.SetNet(netinfo)
            track.SetLocked(True)
            board.Add(track)
            via = pcbnew.PCB_VIA(board)
            via.SetPosition(pcbnew.VECTOR2I_MM(vx, vy))
            via.SetWidth(pcbnew.FromMM(VIA_D))
            via.SetDrill(pcbnew.FromMM(VIA_DRILL))
            via.SetNet(netinfo)
            via.SetLocked(True)
            board.Add(via)
            vias.append((vx, vy))
            obstacles.append(((vx - VIA_D / 2, vy - VIA_D / 2, vx + VIA_D / 2, vy + VIA_D / 2), net))
            obstacles.append((("seg", px, py, vx, vy, STUB_W / 2), net))
            added += 1
    return added


def remove_dangling_tracks(board):
    """Unlocked track ends the router left going nowhere, removed until none are left."""
    removed = 0
    while True:
        board.BuildConnectivity()
        conn = board.GetConnectivity()
        dangling = [t for t in board.GetTracks()
                    if t.GetClass() == "PCB_TRACK" and not t.IsLocked() and conn.TestTrackEndpointDangling(t, False)]
        if not dangling:
            return removed
        for t in dangling:
            board.Remove(t)
        removed += len(dangling)


def remove_dangling_vias(board):
    """Vias the router left with nothing, or with tracks on one layer only."""
    tracks = [t for t in board.GetTracks() if t.GetClass() == "PCB_TRACK"]
    removed = 0
    for via in [t for t in board.GetTracks() if t.GetClass() == "PCB_VIA"]:
        if via.IsLocked():
            continue
        pos, rad = via.GetPosition(), via.GetWidth(pcbnew.F_Cu) / 2
        layers = {t.GetLayer() for t in tracks if t.GetNetCode() == via.GetNetCode() and
                  any((e - pos).EuclideanNorm() <= rad for e in (t.GetStart(), t.GetEnd()))}
        planes = via.GetNetname() in PLANE_NETS
        if not layers or (len(layers) < 2 and not planes):
            board.Remove(via)
            removed += 1
    return removed


def outer_ground(board):
    """GND pours on F.Cu and B.Cu: short return paths and less to etch."""
    gnd = board.FindNet("GND")
    box = board.GetBoardEdgesBoundingBox()
    m = pcbnew.FromMM(0.4)
    for layer in (pcbnew.F_Cu, pcbnew.B_Cu):
        if any(z.GetLayer() == layer and z.GetNetname() == "GND" for z in board.Zones()):
            continue
        z = pcbnew.ZONE(board)
        z.SetLayer(layer)
        z.SetNet(gnd)
        z.SetZoneName("GND pour")
        z.SetLocalClearance(pcbnew.FromMM(0.3))
        z.SetMinThickness(pcbnew.FromMM(0.2))
        z.SetAssignedPriority(0)
        z.SetPadConnection(pcbnew.ZONE_CONNECTION_THT_THERMAL)  # solid to SMD pads, spokes to THT
        ol = z.Outline()
        ol.NewOutline()
        for x, y in [(box.GetLeft() + m, box.GetTop() + m), (box.GetRight() - m, box.GetTop() + m),
                     (box.GetRight() - m, box.GetBottom() - m), (box.GetLeft() + m, box.GetBottom() - m)]:
            ol.Append(x, y)
        board.Add(z)


def main():
    pcb = Path(sys.argv[1]) if len(sys.argv) > 1 else HW / "hddsynthpro.kicad_pcb"
    router = os.environ.get("FREEROUTING")
    if not router:
        sys.exit("set FREEROUTING to the freerouting-cli executable")
    board = pcbnew.LoadBoard(str(pcb))
    print(f"fanout: {fanout(board)} plane vias")
    with tempfile.TemporaryDirectory() as tmp:
        dsn, ses = Path(tmp) / "board.dsn", Path(tmp) / "board.ses"
        if not pcbnew.ExportSpecctraDSN(board, str(dsn)):
            sys.exit("DSN export failed")
        inner = [board.GetLayerName(pcbnew.In1_Cu), board.GetLayerName(pcbnew.In2_Cu)]
        text = dsn.read_text()
        for name in inner:
            text, n = re.subn(r"(\(layer %s\s+\(type )signal" % re.escape(name), r"\1power", text)
            if n != 1:
                sys.exit(f"layer {name} not found in the DSN")
        dsn.write_text(text)
        cmd = [router]
        if router.endswith(".jar"):
            cmd = [os.environ.get("JAVA", "java"), "-Djava.awt.headless=true", "-jar", router, "--gui.enabled=false"]
        subprocess.run(cmd + ["-de", str(dsn), "-do", str(ses), "-mp", PASSES], check=True)
        if not pcbnew.ImportSpecctraSES(board, str(ses)):
            sys.exit("SES import failed")
    print(f"removed {remove_dangling_tracks(board)} dangling track pieces")
    # KiCad 10's Python bindings break after removing items; save and reload before going on.
    board.Save(str(pcb))
    board = pcbnew.LoadBoard(str(pcb))
    print(f"removed {remove_dangling_vias(board)} dangling vias")
    outer_ground(board)
    board.Save(str(pcb))
    tracks = sum(1 for t in board.GetTracks() if t.GetClass() == "PCB_TRACK")
    vias = sum(1 for t in board.GetTracks() if t.GetClass() == "PCB_VIA")
    print(f"routed {pcb.name}: {tracks} track segments, {vias} vias")


if __name__ == "__main__":
    main()
