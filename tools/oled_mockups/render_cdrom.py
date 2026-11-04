#!/usr/bin/env python3
"""Mock-ups of the HDD Synth's CD-ROM screens (docs/hddsynth-cdrom-display-brief.md).

Draws each screen on a 128x64 one-bit canvas with the same primitives and 5x7
font as the synth's own `Canvas` (HDDSynth_Native, copied unchanged into
FDDSynthPro), following FDDSynthPro's `screens.cpp` (its title bar, big digits,
spinning disk, head marker and chooser), and writes them as PNGs the way
FDDSynthPro's docs/oled previews are made: 4x, white on black, grey border.

These are design mock-ups, not the synth's code: the screens belong in
HDDSynth_Native, written in C++ against its Canvas. Standard library only.

    python tools/oled_mockups/render_cdrom.py [docs/oled-cdrom]
"""
import math
import os
import re
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
WIDTH, HEIGHT = 128, 64
CHAR_W = 6
TITLE_H = 10
SCALE, BORDER = 4, 2


def load_font():
    """The glyph bytes from font5x7.h (FDDSynthPro's copy of Adafruit GFX's)."""
    text = open(os.path.join(HERE, "font5x7.h"), encoding="utf-8").read()
    body = text[text.index("FONT5X7[") :]
    body = body[body.index("{") + 1 : body.index("};")]
    body = re.sub(r"//[^\n]*", "", body)
    data = [int(v, 16) for v in re.findall(r"0x[0-9A-Fa-f]{2}", body)]
    assert len(data) == 95 * 5, len(data)
    return data


FONT = load_font()


class Canvas:
    def __init__(self):
        self.px = [[False] * WIDTH for _ in range(HEIGHT)]

    def pixel(self, x, y, on=True):
        if 0 <= x < WIDTH and 0 <= y < HEIGHT:
            self.px[y][x] = on

    def fill_rect(self, x, y, w, h, on=True):
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                self.pixel(xx, yy, on)

    def hline(self, x, y, w, on=True):
        self.fill_rect(x, y, w, 1, on)

    def vline(self, x, y, h, on=True):
        self.fill_rect(x, y, 1, h, on)

    def text(self, x, y, s, on=True, scale=1):
        for ch in s:
            code = ord(ch) if 32 <= ord(ch) <= 126 else ord("?")
            glyph = FONT[(code - 32) * 5 : (code - 32) * 5 + 5]
            for col in range(5):
                for row in range(7):
                    if glyph[col] & (1 << row):
                        self.fill_rect(x + col * scale, y + row * scale, scale, scale, on)
            x += CHAR_W * scale
        return x

    @staticmethod
    def text_width(s, scale=1):
        return len(s) * CHAR_W * scale - scale if s else 0

    # ---- the helpers FDDSynthPro's screens.cpp uses ----

    def title(self, t):
        t = t[:21]
        self.fill_rect(0, 0, WIDTH, TITLE_H)
        self.text(max(0, (WIDTH - self.text_width(t)) // 2), 1, t, on=False)

    def right_text(self, right, y, s, on=True):
        self.text(right - self.text_width(s), y, s, on)

    def centre_text(self, y, s, on=True, scale=1):
        self.text((WIDTH - self.text_width(s, scale)) // 2, y, s, on, scale)

    def line(self, x0, y0, x1, y1, on=True):
        dx, dy = abs(x1 - x0), -abs(y1 - y0)
        sx, sy = (1 if x0 < x1 else -1), (1 if y0 < y1 else -1)
        err = dx + dy
        while True:
            self.pixel(x0, y0, on)
            if x0 == x1 and y0 == y1:
                return
            e2 = 2 * err
            if e2 >= dy:
                err += dy
                x0 += sx
            if e2 <= dx:
                err += dx
                y0 += sy

    def circle(self, cx, cy, r, fill=False, on=True):
        for y in range(-r, r + 1):
            for x in range(-r, r + 1):
                d = x * x + y * y
                if (d <= r * r + r) if fill else (r * r - r <= d <= r * r + r):
                    self.pixel(cx + x, cy + y, on)

    def frame(self, x, y, w, h, on=True):
        self.hline(x, y, w, on)
        self.hline(x, y + h - 1, w, on)
        self.vline(x, y, h, on)
        self.vline(x + w - 1, y, h, on)

    def png(self, path):
        w, h = WIDTH * SCALE + 2 * BORDER, HEIGHT * SCALE + 2 * BORDER
        rows = []
        for y in range(h):
            row = bytearray(w)
            for x in range(w):
                if y < BORDER or y >= h - BORDER or x < BORDER or x >= w - BORDER:
                    row[x] = 90
                else:
                    row[x] = 255 if self.px[(y - BORDER) // SCALE][(x - BORDER) // SCALE] else 0
            rows.append(b"\x00" + bytes(row))

        def chunk(tag, data):
            return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

        with open(path, "wb") as f:
            f.write(b"\x89PNG\r\n\x1a\n")
            f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0)))
            f.write(chunk(b"IDAT", zlib.compress(b"".join(rows), 9)))
            f.write(chunk(b"IEND", b""))


# ---- CD-ROM pictures ----------------------------------------------------------------------


def cd(c, cx, cy, r, rotation=None, spinning=False):
    """A CD from above: the rim, the clamping ring and the hole; with
    `rotation` (0-255), a glint that turns with it, as FDDSynthPro's spoke."""
    c.circle(cx, cy, r)
    c.circle(cx, cy, 4)
    c.circle(cx, cy, 1, fill=True)
    if rotation is None:
        return
    a = rotation * 2 * math.pi / 256

    def at(angle, radius):
        return cx + round(math.sin(angle) * radius), cy - round(math.cos(angle) * radius)

    c.line(*at(a, 6), *at(a, r - 2))
    if spinning:  # motion line behind the glint
        c.line(*at(a - 0.6, r - 4), *at(a - 0.6, r - 2))


def eject_symbol(c, cx, cy):
    for i in range(6):
        c.hline(cx - i, cy - 6 + i, 2 * i + 1)
    c.fill_rect(cx - 6, cy + 2, 13, 3)


def tray(c, x, y, out):
    """The drive's front from above, its tray out (a disc on it) or half in."""
    depth = 24 if out else 12
    bottom = y + 11 + depth - 1
    cd(c, x + 15, bottom - 12, 9)  # the disc sits on the tray, nearer its front
    c.fill_rect(x, y, 30, 12, False)  # the drive's body hides what's still inside
    c.frame(x, y, 30, 12)  # the drive
    c.hline(x + 3, y + 8, 24)  # its slot
    c.frame(x + 3, y + 11, 24, depth)  # the tray


def arrow(c, ax, top, bottom, up):
    c.fill_rect(ax - 1, top + 4, 3, bottom - top - 8)
    for i in range(5):
        c.hline(ax - i, top + i if up else bottom - i, 2 * i + 1)


def mmss(frame):
    """A frame (75 a second, as a CD player counts them) as MM:SS."""
    s = frame // 75
    return "%02u:%02u" % (min(s // 60, 99), s % 60)


def mb(frames):
    """The disc's size as CDs are named: 650M is 650 MiB."""
    return "%uM" % round(frames * 2048 / 1048576)


# ---- the screens --------------------------------------------------------------------------


def home(c, s):
    """The CD-ROM's home screen, FDDSynthPro's laid out for a disc: the time
    where the track was, the disc's size where the floppy's format was, a CD
    in place of the floppy, the laser's place across the disc at the bottom."""
    c.title(s.get("name", "CD-ROM"))
    if not s.get("disc"):
        c.text(2, 13, "TIME")
        c.text(2, 22, "--:--", scale=3)
        eject_symbol(c, 113, 24)
        c.text(2, 46, "NO DISC" if not s.get("changing") else "CHANGING")
        if "list" in s:
            c.right_text(126, 46, "-/%u" % s["list"])
        base_bar(c)
        return
    c.text(2, 13, "TIME")
    c.text(40, 13, "ISO " + mb(s["frames"]))
    c.text(2, 22, mmss(s["frame"]), scale=3)
    cd(c, 113, 30, 11, s.get("rotation", 40), s.get("spinning", True))
    c.text(2, 46, s["activity"])
    if s.get("index") is not None:
        c.right_text(126, 46, "%u/%u" % (s["index"] + 1, s["list"]))
    base_bar(c, s["frame"] / max(1, s["frames"]))


def base_bar(c, where=None):
    """The disc from its centre (left) to its edge (right), as FDDSynthPro's
    cylinders: a tick every 10%, the marker over the laser's place."""
    x0, x1, base = 3, 124, 62
    c.hline(x0, base, x1 - x0 + 1)
    for i in range(11):
        c.vline(x0 + i * (x1 - x0) // 10, base - 2, 2)
    if where is not None:
        hx = x0 + round(min(1.0, where) * (x1 - x0))
        for i in range(4):
            c.hline(hx - 3 + i, 55 + i, 7 - 2 * i)


def no_link(c):
    c.title("CD-ROM")
    c.centre_text(18, "NO LINK", scale=2)
    c.centre_text(40, "IS HDDSYNTHPRO ON?")
    c.centre_text(52, "CHECK THE CABLE")


def discs(c, names, selection, current):
    """FDDSynthPro's chooser: the selection lit, the disc in now marked '>'."""
    c.title("Discs %u/%u" % (selection + 1, len(names)))
    rows, row_h, top = 6, 9, 11
    first = 0
    if len(names) > rows:
        first = min(max(selection - 2, 0), len(names) - rows)
    for r in range(rows):
        i = first + r
        if i >= len(names):
            break
        y = top + r * row_h
        sel = i == selection
        if sel:
            c.fill_rect(0, y - 1, WIDTH, row_h)
        if i == current:
            c.text(1, y, ">", not sel)
        name, frames = names[i]
        name = name[:-4] if name.upper().endswith(".ISO") else name
        c.text(8, y, name[:15], not sel)
        c.right_text(126, y, mb(frames), not sel)


def swap(c, out, name):
    """Changing discs: the tray coming out with the old disc, then going in
    with the new one, until HDDSynthPro answers $HDR."""
    c.title("New disc")
    tray(c, 4, 14, out)
    arrow(c, 42, 20, 46, up=not out)
    c.text(52, 24, "EJECTING" if out else "LOADING")
    name = name[:-4] if name.upper().endswith(".ISO") else name
    c.text(52, 36, name[:12])


def refused(c, reason, name):
    c.title("New disc")
    c.centre_text(16, "NOT LOADED", scale=2)
    c.centre_text(38, reason.upper())
    c.centre_text(50, name[:21])


def laser_trace(c, frames, history, now):
    """FDDSynthPro's head trace for the CD's laser: the start of each READ
    over time, the disc's centre at the top, its edge at the bottom."""
    c.title("Laser  " + mmss(now))
    top, bottom = 13, 63
    for x in range(0, WIDTH, 4):
        c.pixel(x, (top + bottom) // 2)
    px = None
    n = len(history)
    for i, f in enumerate(history):
        x = WIDTH - n + i
        y = top + round(min(f, frames) * (bottom - top) / frames)
        if px is not None:
            c.line(px, py, x, y)
        px, py = x, y


def drives(c, units):
    """Both drives at once (bb48_hdcd): each unit's kind, what it serves,
    and its activity light, from $HDU and $HDD."""
    c.title("Drives")
    for i, u in enumerate(units):
        y = 15 + i * 24
        if u["busy"]:
            c.fill_rect(2, y, 7, 7)
        else:
            c.frame(2, y, 7, 7)
        c.text(12, y, "%u %s" % (u["unit"], "HARD DISK" if u["kind"] == "D" else "CD-ROM"))
        c.right_text(126, y, u["op"])
        c.text(12, y + 10, u["name"][:19])


# ---- the set ------------------------------------------------------------------------------

LIST = [
    ("HDDSYNTH.ISO", 32768),
    ("ENCARTA95.ISO", 289104),
    ("WIN95OSR2.ISO", 301557),
    ("DOOM_SE.ISO", 11431),
    ("MYST.ISO", 255380),
    ("REBEL_ASSAULT.ISO", 196608),
    ("SIMCITY2K.ISO", 47185),
    ("DESCENT.ISO", 98304),
    ("7TH_GUEST_1.ISO", 330000),
    ("7TH_GUEST_2.ISO", 318000),
    ("MONKEY1CD.ISO", 104857),
    ("QUAKE.ISO", 286720),
]


def screens():
    doom = dict(name="DOOM_SE.ISO", disc=True, frames=11431, index=3, list=len(LIST))
    yield "home_reading", lambda c: home(c, dict(doom, frame=4096, activity="READING", rotation=40))
    yield "home_seeking", lambda c: home(c, dict(doom, frame=9300, activity="SEEKING", rotation=150))
    yield "home_idle", lambda c: home(c, dict(doom, frame=9300, activity="IDLE", rotation=150, spinning=False))
    yield "home_no_disc", lambda c: home(c, dict(list=len(LIST)))
    yield "home_no_link", no_link
    yield "discs", lambda c: discs(c, LIST, 6, 3)
    yield "swap_ejecting", lambda c: swap(c, True, "DOOM_SE.ISO")
    yield "swap_loading", lambda c: swap(c, False, "SIMCITY2K.ISO")
    yield "swap_refused", lambda c: refused(c, "the card has changed", "SIMCITY2K.ISO")
    # A game loading: a run of reads near the start, a seek out to its data, and back.
    trace = [300 + 20 * i for i in range(30)] + [8200 + 15 * i for i in range(40)] + [900 + 10 * i for i in range(25)]
    trace += [9300 + 30 * i for i in range(33)]
    yield "laser_trace", lambda c: laser_trace(c, 11431, trace, trace[-1])
    yield "drives", lambda c: drives(
        c,
        [
            dict(unit=0, kind="D", busy=False, op="IDLE", name="ICL386MB.IMG"),
            dict(unit=1, kind="C", busy=True, op="READ", name="DOOM_SE.ISO"),
        ],
    )


def main(out):
    os.makedirs(out, exist_ok=True)
    for name, draw in screens():
        c = Canvas()
        draw(c)
        c.png(os.path.join(out, name + ".png"))
        print(os.path.join(out, name + ".png"))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "..", "docs", "oled-cdrom"))
