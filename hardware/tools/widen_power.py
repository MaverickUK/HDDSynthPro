"""Widens the supply tracks Freerouting left thin, as far as the clearances allow.

Freerouting doesn't keep the Power net class's 0.6 mm on every segment (on rev 0.4 it
routed VB, the 5 V supply, mostly at 0.2 mm, with necks to 0.1 mm). This sets each
segment of the nets below to its target width, runs KiCad's DRC, steps every segment
in a clearance violation down to the next width, and repeats until DRC is clean.

Run with KiCad's own Python, kicad-cli on PATH, after autoroute.py:
  python tools/widen_power.py [board.kicad_pcb]
"""
import math
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import pcbnew

HW = Path(__file__).resolve().parent.parent
TARGETS = {"VB": 0.6, "SD_VDD": 0.4}  # net (without its sheet path) -> width, mm
STEPS = [0.6, 0.5, 0.4, 0.3, 0.25]
MIN_LEN = 0.3  # mm: shorter pieces are pad entries, left as routed


def drc_points(pcb):
    """Points (mm) where DRC finds a clearance or short violation."""
    with tempfile.TemporaryDirectory() as tmp:
        rpt = Path(tmp) / "drc.rpt"
        subprocess.run(["kicad-cli", "pcb", "drc", "--refill-zones", "-o", str(rpt), str(pcb)],
                       check=False, capture_output=True)
        text = rpt.read_text()
    pts = []
    for block in re.split(r"\n(?=\[)", text):
        if block.startswith(("[clearance]", "[shorting_items]", "[tracks_crossing]", "[hole_clearance]")):
            pts += [(float(x), float(y)) for x, y in re.findall(r"@\(([\d.]+) mm, ([\d.]+) mm\)", block)]
    return pts


def near(track, pts):
    s, e = track.GetStart(), track.GetEnd()
    x0, y0, x1, y1 = (pcbnew.ToMM(v) for v in (s.x, s.y, e.x, e.y))
    w = pcbnew.ToMM(track.GetWidth())
    for px, py in pts:
        vx, vy = x1 - x0, y1 - y0
        L = vx * vx + vy * vy
        t = 0.0 if L == 0 else max(0.0, min(1.0, ((px - x0) * vx + (py - y0) * vy) / L))
        if math.hypot(px - (x0 + t * vx), py - (y0 + t * vy)) <= w / 2 + 0.05:
            return True
    return False


def main():
    pcb = Path(sys.argv[1]) if len(sys.argv) > 1 else HW / "hddsynthpro.kicad_pcb"
    board = pcbnew.LoadBoard(str(pcb))
    width = {}  # track uuid -> index into STEPS
    routed = {}  # track uuid -> the width the router gave it
    for t in board.GetTracks():
        net = t.GetNetname().split("/")[-1]
        if t.GetClass() != "PCB_TRACK" or net not in TARGETS or t.IsLocked():
            continue
        if pcbnew.ToMM(t.GetLength()) < MIN_LEN or pcbnew.ToMM(t.GetWidth()) >= TARGETS[net]:
            continue
        width[t.m_Uuid.AsString()] = STEPS.index(TARGETS[net])
        routed[t.m_Uuid.AsString()] = t.GetWidth()
    print(f"{len(width)} segments to widen")
    # DRC names one point per item, so a violation can be blamed on a neighbouring segment;
    # after putting back the segments that fit at no width, check again until DRC is clean.
    while True:
        while True:
            changed = 0
            for t in board.GetTracks():
                k = width.get(t.m_Uuid.AsString())
                if k is not None and k < len(STEPS):
                    t.SetWidth(pcbnew.FromMM(STEPS[k]))
            board.Save(str(pcb))
            pts = drc_points(pcb)
            board = pcbnew.LoadBoard(str(pcb))
            for t in board.GetTracks():
                u = t.m_Uuid.AsString()
                if u in width and width[u] < len(STEPS) and near(t, pts):
                    width[u] += 1
                    changed += 1
            print(f"{len(pts)} violation points, {changed} segments stepped down")
            if not changed:
                break
        # whatever still violates at the narrowest step goes back to how the router left it
        put_back = [t for t in board.GetTracks() if width.get(t.m_Uuid.AsString()) == len(STEPS)]
        for t in put_back:
            t.SetWidth(routed[t.m_Uuid.AsString()])
            del width[t.m_Uuid.AsString()]
        board.Save(str(pcb))
        if not put_back and not pts:
            break
        if not put_back:  # violations left that no widened segment is near: not ours
            print(f"{len(pts)} violation points left near no widened segment")
            break
        board = pcbnew.LoadBoard(str(pcb))
    final = {}
    for t in board.GetTracks():
        if t.m_Uuid.AsString() in routed:
            w = round(pcbnew.ToMM(t.GetWidth()), 2)
            final[w] = final.get(w, 0) + 1
    print("widths now:", dict(sorted(final.items(), reverse=True)))


if __name__ == "__main__":
    main()
