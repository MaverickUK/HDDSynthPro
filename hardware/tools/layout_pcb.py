"""Creates the first version of the HDDSynthPro board: footprints placed, nets assigned, planes poured.

Run with KiCad's own Python, which has the pcbnew module:
  /Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 \
      hardware/tools/layout_pcb.py [output.kicad_pcb]
  or, on a board that's already routed, redo only the logo and labels:
      hardware/tools/layout_pcb.py --silkscreen hardware/hddsynthpro.kicad_pcb
  then fill the planes:  kicad-cli pcb drc --refill-zones --save-board hardware/hddsynthpro.kicad_pcb

It reads the netlist from the schematic (via kicad-cli), so every footprint is
linked to its symbol and "Update PCB from Schematic" works afterwards. Like
gen_schematic.py, this only bootstraps: once the board has been edited in
KiCad, rerunning it overwrites the edits. Traces aren't routed here.

Layout: the 40-pin IDE header on the left edge, vertical. The four bus switches
sit beside it, their IDE side facing the header and their PGA2350 side facing
the module. The PGA2350 is in the middle; USB-C, power and the link along the top
edge; the microSD socket on the right edge with its pull-ups beside it.
Four layers: signals, GND plane, 3V3 plane, signals.
"""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import pcbnew

sys.path.insert(0, str(Path(__file__).resolve().parent))
from sexpr import find, first, parse  # noqa: E402

HW = Path(__file__).resolve().parent.parent
KICAD_SHARE = Path(os.environ.get("KICAD_SHARE", "/Applications/KiCad/KiCad.app/Contents/SharedSupport"))
OUT = HW / "hddsynthpro.kicad_pcb"

# Board outline, mm. KiCad's y axis points down.
X0, Y0, W, H = 100.0, 100.0, 85.0, 76.0

# ref -> (x, y, rotation in degrees counterclockwise). Footprint origins: pin 1 for the
# IDE header and the pin socket, the centre for everything else.
PLACE = {
    # IDE side
    "J5": (105.5, 114.0, 0),       # 2x20 header, pin 1 at the top, key facing the edge
    # Bus switches, B side (pins 11-18) facing the header, each beside the header pins it serves.
    "U2": (118.0, 119.0, 180),     # header pins 1-9
    "U3": (118.0, 129.5, 180),     # 10-17
    "U5": (118.0, 141.5, 180),     # 18-33
    "U6": (118.0, 151.5, 180),     # 35-38
    "C4": (118.0, 124.2, 0),       # decoupling, below each switch
    "C5": (118.0, 134.7, 0),
    "C7": (118.0, 146.7, 0),
    "C8": (118.0, 156.7, 0),
    "R13": (115.0, 160.5, 90),     # BUS_EN# pull-up
    "JP1": (117.6, 160.5, 90),     # BUS_EN# to GND
    "R14": (132.0, 153.0, 0),      # DIOR# 33R
    "R15": (132.0, 156.0, 0),      # DIOW# 33R
    # Module
    "U1": (143.0, 133.0, 0),
    "C6": (137.5, 116.8, 0),       # 3V3 bulk, near pad B4
    # Top edge: link, buttons, SWD, USB-C, power
    "J3": (116.0, 104.0, 90),      # link socket, pins along the edge, pin 1 on the left
    "R3": (117.27, 108.8, 90),
    "R4": (119.81, 108.8, 90),
    "J6": (127.46, 104.0, 90),     # SWD, 1x3 0.1" header, pins along the edge
    "SW1": (128.0, 111.5, 0),      # BOOTSEL
    "SW2": (134.0, 111.5, 0),      # RESET
    "J1": (147.0, 103.1, 180),     # USB-C, mouth on the top edge
    "U4": (147.0, 111.0, 0),
    "R1": (141.2, 110.5, 90),
    "R2": (152.8, 110.5, 90),
    "D1": (158.5, 104.0, 0),
    "C1": (160.0, 110.5, 0),
    "J2": (166.0, 103.6, 0),       # 5 V in
    "F1": (167.5, 110.5, 0),
    "D2": (174.0, 110.5, 0),
    # LEDs, top right
    "R5": (163.0, 117.0, 0),
    "D3": (167.0, 117.0, 0),
    "R6": (172.0, 117.0, 0),
    "D4": (176.0, 117.0, 0),
    # microSD on the right edge, card in from the right
    "J4": (X0 + W - 8.93, 140.0, 90),
    "R7": (161.0, 126.5, 0),
    "R8": (161.0, 128.7, 0),
    "R9": (161.0, 130.9, 0),
    "R10": (161.0, 133.1, 0),
    "R11": (161.0, 135.3, 0),
    "R12": (161.0, 137.5, 0),
    "C3": (161.0, 140.0, 0),
    "C2": (161.0, 143.0, 0),
}
# Reference designators moved off pads and neighbours: ref -> (x, y), or None to hide.
REF_TEXT = {
    **{r: (158.0, PLACE[r][1]) for r in ("R7", "R8", "R9", "R10", "R11", "R12", "C3", "C2")},
    "R3": (117.27, 111.6), "R4": (119.81, 111.6), "U4": (147.0, 113.6), "J6": (135.6, 103.4), "J1": (153.6, 101.6),
    "J2": (172.6, 103.6), "F1": (167.5, 112.6), "R13": (114.3, 163.8), "JP1": (118.3, 163.8),
    **{u: (PLACE[u][0], PLACE[u][1]) for u in ("U2", "U3", "U5", "U6")},  # on the chip body
    **{c: (114.9, PLACE[c][1]) for c in ("C4", "C5", "C7", "C8")},
    "H1": None, "H2": None, "H3": None, "H4": None,
}
HOLES = [(104.0, 104.0), (181.0, 104.0), (104.0, 172.0), (181.0, 172.0)]
LOGO = (129.0, 166.8)  # centre of LOGO_HDDSynthPro (19.4 x 16 mm, from tools/make_logo.py)
SILK = [  # text, x, y, size
    ("PGA2350 IDE drive  rev 0.1", 158.0, 164.0, 1.0),
    ("IDE 40-pin", 113.0, 170.5, 1.0),
    ("NC", 116.0, 101.3, 0.8), ("GND", 118.54, 101.3, 0.8), ("TX", 121.08, 101.3, 0.8), ("RX", 123.62, 101.3, 0.8),
    ("+5V GND", 167.25, 108.5, 0.8),
    ("CLK", 127.46, 101.3, 0.8), ("GND", 130.0, 101.3, 0.8), ("DIO", 132.54, 101.3, 0.8),
    ("BOOT", 128.0, 114.3, 0.8),
    ("RST", 134.0, 114.3, 0.8),
    ("JP1: close for firmware without BUS_EN#", 158.0, 167.5, 0.8),
]
# Net-name patterns for the Power class (local nets carry their sheet path).
POWER_PATTERNS = ["VB", "*/VBUS_USB", "*/5V_EXT", "*/5V_EXT_IN"]  # GND and +3V3 are planes


def mm(v):
    return pcbnew.FromMM(v)


def netlist():
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "board.net"
        subprocess.run(["kicad-cli", "sch", "export", "netlist", "--format", "kicadsexpr", "-o", str(out),
                        str(HW / "hddsynthpro.kicad_sch")], check=True, capture_output=True)
        root = parse(out.read_text())
    comps = {}
    for c in find(first(root, "components"), "comp"):
        ref = first(c, "ref")[1]
        sheet = first(first(c, "sheetpath"), "tstamps")[1]
        tstamps = first(c, "tstamps")
        fields = {f[1][1]: (f[2] if len(f) > 2 else "") for f in find(first(c, "fields"), "field")}
        fields.pop("Footprint", None)
        props = {first(p, "name")[1] for p in find(c, "property")}
        comps[ref] = {"value": first(c, "value")[1], "footprint": first(c, "footprint")[1],
                      "path": sheet + tstamps[1], "fields": fields, "in_bom": "exclude_from_bom" not in props}
    nets = {}
    for n in find(first(root, "nets"), "net"):
        name = first(n, "name")[1]
        nets[name] = [(first(x, "ref")[1], first(x, "pin")[1]) for x in find(n, "node")]
    return comps, nets


def load_footprint(fpid):
    lib, name = fpid.split(":")
    path = HW / f"{lib}.pretty" if lib == "HDDSynthPro" else KICAD_SHARE / "footprints" / f"{lib}.pretty"
    fp = pcbnew.FootprintLoad(str(path), name)
    if fp is None:
        raise FileNotFoundError(fpid)
    fp.SetFPID(pcbnew.LIB_ID(lib, name))
    return fp


def outline(board):
    rect = pcbnew.PCB_SHAPE(board)
    rect.SetShape(pcbnew.SHAPE_T_RECT)
    rect.SetStart(pcbnew.VECTOR2I(mm(X0), mm(Y0)))
    rect.SetEnd(pcbnew.VECTOR2I(mm(X0 + W), mm(Y0 + H)))
    rect.SetLayer(pcbnew.Edge_Cuts)
    rect.SetWidth(mm(0.1))
    board.Add(rect)


def plane(board, net, layer, name):
    z = pcbnew.ZONE(board)
    z.SetLayer(layer)
    z.SetNet(net)
    z.SetZoneName(name)
    z.SetLocalClearance(mm(0.3))
    z.SetMinThickness(mm(0.2))
    z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
    ol = z.Outline()
    ol.NewOutline()
    m = 0.4
    for x, y in [(X0 + m, Y0 + m), (X0 + W - m, Y0 + m), (X0 + W - m, Y0 + H - m), (X0 + m, Y0 + H - m)]:
        ol.Append(mm(x), mm(y))
    board.Add(z)
    return z


def design_rules(project_file):
    """Net classes and limits for a 4-layer board at JLCPCB-class fabs, with margin.

    KiCad keeps these in the project file, not the board, so they're written there.
    """
    pro = json.loads(project_file.read_text()) if project_file.exists() else {}
    base = {"bus_width": 12, "diff_pair_gap": 0.25, "diff_pair_via_gap": 0.25, "diff_pair_width": 0.2,
            "line_style": 0, "microvia_diameter": 0.3, "microvia_drill": 0.1, "pcb_color": "rgba(0, 0, 0, 0.000)",
            "schematic_color": "rgba(0, 0, 0, 0.000)", "tuning_profile": "", "wire_width": 6}
    pro["net_settings"] = {
        "classes": [
            {**base, "name": "Default", "priority": 2147483647, "clearance": 0.15, "track_width": 0.2,
             "via_diameter": 0.6, "via_drill": 0.3},
            {**base, "name": "Power", "priority": 0, "clearance": 0.15, "track_width": 0.6,
             "via_diameter": 0.8, "via_drill": 0.4},
        ],
        "meta": {"version": 5}, "net_colors": None, "netclass_assignments": None,
        "netclass_patterns": [{"netclass": "Power", "pattern": p} for p in POWER_PATTERNS],
    }
    rules = pro.setdefault("board", {}).setdefault("design_settings", {}).setdefault("rules", {})
    rules.update({"min_clearance": 0.15, "min_track_width": 0.1, "min_via_diameter": 0.5,
                  "min_via_annular_width": 0.1, "min_through_hole_diameter": 0.3, "min_hole_to_hole": 0.25,
                  "min_copper_edge_clearance": 0.3,
                  "min_resolved_spokes": 1})  # THT GND pins also join the inner plane
    project_file.write_text(json.dumps(pro, indent=2) + "\n")


def silkscreen(board, logo):
    """The board's own silkscreen: the logo and the labels. Replaces any already there.

    `logo` is the loaded LOGO_HDDSynthPro footprint: load it before LoadBoard(), as
    FootprintLoad() fails in KiCad 10's bindings once a board has been loaded.
    """
    for d in list(board.GetDrawings()):
        if d.GetClass() == "PCB_TEXT" and d.GetLayer() == pcbnew.F_SilkS:
            board.Remove(d)
    old = board.FindFootprintByReference("LOGO1")
    if old:
        board.Remove(old)
    logo.SetReference("LOGO1")
    logo.SetPosition(pcbnew.VECTOR2I(mm(LOGO[0]), mm(LOGO[1])))
    board.Add(logo)
    for text, x, y, size in SILK:
        t = pcbnew.PCB_TEXT(board)
        t.SetText(text)
        t.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y)))
        t.SetLayer(pcbnew.F_SilkS)
        t.SetTextSize(pcbnew.VECTOR2I(mm(size), mm(size)))
        t.SetTextThickness(mm(size * 0.15))
        board.Add(t)


def main():
    if len(sys.argv) > 2 and sys.argv[1] == "--silkscreen":
        # Redo only the logo and labels on an existing (routed) board.
        logo = load_footprint("HDDSynthPro:LOGO_HDDSynthPro")
        board = pcbnew.LoadBoard(sys.argv[2])
        silkscreen(board, logo)
        board.Save(sys.argv[2])
        print(f"silkscreen updated on {sys.argv[2]}")
        return
    comps, nets = netlist()
    missing = set(comps) - set(PLACE)
    if missing:
        raise KeyError(f"no position for {sorted(missing)}")

    board = pcbnew.CreateEmptyBoard()
    board.SetCopperLayerCount(4)
    board.SetLayerName(pcbnew.In1_Cu, "GND")
    board.SetLayerName(pcbnew.In2_Cu, "3V3")
    outline(board)

    net_items = {}
    for name in nets:
        item = pcbnew.NETINFO_ITEM(board, name)
        board.Add(item)
        net_items[name] = item
    pad_net = {key: name for name, nodes in nets.items() for key in nodes}

    for ref, c in sorted(comps.items()):
        fp = load_footprint(c["footprint"])
        fp.SetReference(ref)
        fp.SetValue(c["value"])
        fp.SetPath(pcbnew.KIID_PATH(c["path"]))
        for name, text in c["fields"].items():  # as "Update PCB from Schematic" would
            fp.SetField(name, text)
            fp.GetField(name).SetVisible(False)
        fp.SetExcludedFromBOM(not c["in_bom"])
        x, y, rot = PLACE[ref]
        fp.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y)))
        fp.SetOrientationDegrees(rot)
        board.Add(fp)
        for pad in fp.Pads():
            name = pad_net.get((ref, pad.GetNumber()))
            if name:
                pad.SetNet(net_items[name])

    for i, (x, y) in enumerate(HOLES, start=1):
        fp = load_footprint("MountingHole:MountingHole_3.2mm_M3")
        fp.SetReference(f"H{i}")
        fp.SetValue("M3")
        fp.SetBoardOnly(True)
        fp.SetExcludedFromBOM(True)
        fp.SetPosition(pcbnew.VECTOR2I(mm(x), mm(y)))
        board.Add(fp)

    for fp in board.GetFootprints():
        ref = fp.GetReference()
        if ref in REF_TEXT and REF_TEXT[ref] is None:
            fp.Reference().SetVisible(False)
        elif ref in REF_TEXT:
            fp.Reference().SetPosition(pcbnew.VECTOR2I(mm(REF_TEXT[ref][0]), mm(REF_TEXT[ref][1])))
            fp.Reference().SetTextAngleDegrees(0)

    silkscreen(board, load_footprint("HDDSynthPro:LOGO_HDDSynthPro"))

    # The zone filler crashes outside the GUI; fill the planes with kicad-cli (see the docstring).
    plane(board, net_items["GND"], pcbnew.In1_Cu, "GND plane")
    plane(board, net_items["+3V3"], pcbnew.In2_Cu, "3V3 plane")

    out = Path(sys.argv[1]) if len(sys.argv) > 1 else OUT
    board.Save(str(out))
    design_rules(out.with_suffix(".kicad_pro"))
    print(f"wrote {out}: {len(comps)} parts, {len(nets)} nets, {len(HOLES)} holes")


if __name__ == "__main__":
    main()
