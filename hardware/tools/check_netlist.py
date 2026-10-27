#!/usr/bin/env python3
"""Checks the KiCad schematic against src/config.h and the ATA pinout.

Exports the netlist with kicad-cli, then follows each IDE signal from its
GPIO on the PGA2350, through any series resistor and the bus switch, to the
IDE header, and checks it lands on the right pin. Also checks the microSD
socket, the activity link, the LED, the bus-switch enable and the IDE grounds.
Run it after editing the schematic: CLAUDE.md requires the pin map in
src/config.h, docs/wiring.md and the hardware to agree.

Usage: python3 hardware/tools/check_netlist.py   (exit status 1 on a mismatch)
"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from sexpr import find, first, parse

HW = Path(__file__).resolve().parent.parent
REPO = HW.parent

# The ATA 40-pin header (drive's view).
IDE_PIN = {**{f"DD{i}": p for i, p in enumerate([17, 15, 13, 11, 9, 7, 5, 3, 4, 6, 8, 10, 12, 14, 16, 18])},
           "DA0": 35, "DA1": 33, "DA2": 36, "CS0#": 37, "CS1#": 38, "DIOR#": 25, "DIOW#": 23,
           "IORDY": 27, "INTRQ": 31, "RESET#": 1, "DMARQ": 21, "DMACK#": 29}
IDE_GND = [2, 19, 22, 24, 26, 30, 40]

# Pins this board adds that config.h doesn't have yet.
BOARD_ONLY = {"DMARQ": 31, "DMACK#": 3, "SD_CD": 40, "BUS_EN#": 41, "LINK_TX": 0, "LINK_RX": 1}


def config_gpios():
    text = (REPO / "src" / "config.h").read_text()
    c = {k: int(v) for k, v in re.findall(r"constexpr unsigned (\w+) = (\d+);", text)}
    g = {f"DD{i}": c["PIN_DD0"] + i for i in range(16)}
    g.update({f"DA{i}": c["PIN_DA0"] + i for i in range(3)})
    g.update({"CS0#": c["PIN_CS0_N"], "CS1#": c["PIN_CS1_N"], "DIOR#": c["PIN_DIOR_N"], "DIOW#": c["PIN_DIOW_N"],
              "IORDY": c["PIN_IORDY"], "INTRQ": c["PIN_INTRQ"], "RESET#": c["PIN_RESET_N"],
              "LED": c["PIN_BOARD_LED"], "SD_CLK": c["PIN_SD_CLK"], "SD_CMD": c["PIN_SD_CMD"],
              "SD_DAT0": c["PIN_SD_DAT0"], "SD_DAT3": c["PIN_SD_DAT3"]})
    g["SD_DAT1"], g["SD_DAT2"] = g["SD_DAT0"] + 1, g["SD_DAT0"] + 2
    g.update(BOARD_ONLY)
    return g


class Netlist:
    def __init__(self, path):
        root = parse(path.read_text())
        self.value, self.lib = {}, {}
        for comp in find(first(root, "components"), "comp"):
            ref = first(comp, "ref")[1]
            self.value[ref] = first(comp, "value")[1]
            ls = first(comp, "libsource")
            self.lib[ref] = f'{first(ls, "lib")[1]}:{first(ls, "part")[1]}'
        self.pin_name = {}  # (ref, pin) -> pin name
        self.net_of = {}    # (ref, pin) -> net
        self.nodes = {}     # net -> [(ref, pin)]
        for net in find(first(root, "nets"), "net"):
            name = first(net, "name")[1]
            for node in find(net, "node"):
                key = (first(node, "ref")[1], first(node, "pin")[1])
                pf = first(node, "pinfunction")
                fn = pf[1] if pf else ""
                if fn.endswith("_" + key[1]):  # KiCad 10 writes "name_number"
                    fn = fn[: -len(key[1]) - 1]
                self.pin_name[key] = fn
                self.net_of[key] = name
                self.nodes.setdefault(name, []).append(key)

    def refs(self, lib_id):
        return sorted(r for r, l in self.lib.items() if l == lib_id)

    def pin_named(self, ref, name):
        for (r, p), n in self.pin_name.items():
            if r == ref and n == name:
                return p
        return None

    def through(self, net, seen):
        """Hop through 2-pin series resistors under 100 ohm and bus-switch channels."""
        for ref, pin in self.nodes.get(net, []):
            if (ref, pin) in seen:
                continue
            lib = self.lib[ref]
            other = None
            if lib == "Device:R" and ohms(self.value[ref]) < 100:
                other = (ref, "2" if pin == "1" else "1")
            elif "CB3T" in lib.upper():  # bus switch: A<n> <-> B<n> (or 1A<n> <-> 1B<n>)
                name = self.pin_name[(ref, pin)]
                m = re.fullmatch(r"([12]?)([AB])(\d+)", name)
                if m:
                    twin = m.group(1) + ("B" if m.group(2) == "A" else "A") + m.group(3)
                    other = (ref, self.pin_named(ref, twin))
            if other and other in self.net_of:
                seen |= {(ref, pin), other}
                yield other, self.net_of[other]


def ohms(v):
    m = re.fullmatch(r"([\d.]+)([kKmM]?)", v.replace("R", "").replace("Ω", ""))
    if not m:
        return float("inf")
    return float(m.group(1)) * {"": 1, "k": 1e3, "K": 1e3, "m": 1e6, "M": 1e6}[m.group(2)]


def main():
    sch = HW / "hddsynthpro.kicad_sch"
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "hddsynthpro.net"
        subprocess.run(["kicad-cli", "sch", "export", "netlist", "--format", "kicadsexpr", "-o", str(out), str(sch)],
                       check=True, capture_output=True)
        nl = Netlist(out)

    errors = []
    gp = config_gpios()
    mcu = nl.refs("HDDSynthPro:PGA2350")
    if len(mcu) != 1:
        sys.exit(f"expected one PGA2350, found {mcu}")
    u1 = mcu[0]
    ide = [r for r in nl.refs("Connector_Generic:Conn_02x20_Odd_Even")]
    j_ide = ide[0] if len(ide) == 1 else None
    if not j_ide:
        errors.append(f"expected one 40-pin IDE header, found {ide}")

    def gpio_net(name):
        pad = nl.pin_named(u1, f"GP{gp[name]}")
        return nl.net_of.get((u1, pad)) if pad else None

    def reach(name):
        """Every (ref, pin) reachable from the signal's GPIO through series parts."""
        start = gpio_net(name)
        if not start:
            return set()
        found, frontier, seen = set(nl.nodes[start]), [start], set()
        while frontier:
            net = frontier.pop()
            for _, nxt in nl.through(net, seen):
                found |= set(nl.nodes[nxt])
                frontier.append(nxt)
        return found

    for sig, pin in IDE_PIN.items():
        hits = sorted(int(p) for r, p in reach(sig) if r == j_ide)
        if hits != [pin]:
            errors.append(f"{sig} (GP{gp[sig]}) reaches IDE pin {hits or 'none'}, expected {pin}")
    for pin in IDE_GND:
        if nl.net_of.get((j_ide, str(pin))) != "GND":
            errors.append(f"IDE pin {pin} is not on GND")

    sd = nl.refs("Connector:Micro_SD_Card_Det_Hirose_DM3AT")
    for sig, card_pin in [("SD_CLK", "CLK"), ("SD_CMD", "CMD"), ("SD_DAT0", "DAT0"), ("SD_DAT1", "DAT1"),
                          ("SD_DAT2", "DAT2"), ("SD_DAT3", "DAT3/CD"), ("SD_CD", "DET_A")]:
        p = nl.pin_named(sd[0], card_pin) if sd else None
        if not p or nl.net_of.get((sd[0], p)) != gpio_net(sig):
            errors.append(f"{sig} (GP{gp[sig]}) is not on the microSD's {card_pin}")

    # The link and the LED pass through resistors of 470 ohm and up, so follow them by hand.
    def via_resistor(sig):
        out = set()
        for ref, pin in nl.nodes.get(gpio_net(sig), []):
            if nl.lib[ref] == "Device:R":
                other = nl.net_of.get((ref, "2" if pin == "1" else "1"))
                out |= set(nl.nodes.get(other, []))
        return out

    link = nl.refs("Connector_Generic:Conn_01x04")
    for sig, pin in [("LINK_TX", "3"), ("LINK_RX", "4")]:
        if not link or (link[0], pin) not in via_resistor(sig):
            errors.append(f"{sig} (GP{gp[sig]}) doesn't reach the link header's pin {pin} through a resistor")
    if link and nl.net_of.get((link[0], "2")) != "GND":
        errors.append("link header pin 2 is not GND")
    if not any(nl.lib[r] == "Device:LED" for r, _ in via_resistor("LED")):
        errors.append(f"LED (GP{gp['LED']}) doesn't reach an LED through a resistor")

    en = gpio_net("BUS_EN#")
    oe = [(r, p) for (r, p), n in nl.pin_name.items() if "CB3T" in nl.lib[r].upper() and "OE" in n]
    on_en = [k for k in oe if nl.net_of.get(k) == en]
    if not oe or len(on_en) != len(oe):
        errors.append(f"BUS_EN# (GP{gp['BUS_EN#']}) drives {len(on_en)} of {len(oe)} switch enables")
    if not any(nl.lib[r] == "Device:R" and "+3V3" in (nl.net_of.get((r, "1")), nl.net_of.get((r, "2")))
               for r, _ in nl.nodes.get(en, [])):
        errors.append("BUS_EN# has no pull-up to +3V3")

    if errors:
        print("\n".join("FAIL " + e for e in errors))
        sys.exit(1)
    print(f"OK: {len(IDE_PIN)} IDE signals, {len(IDE_GND)} IDE grounds, microSD, link, LED and bus enable "
          f"match src/config.h")


if __name__ == "__main__":
    main()
