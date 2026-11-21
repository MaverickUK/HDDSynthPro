# The Amiga 1200

**Status, 21 Nov 2026 (branch `amiga`, 0.36.0-dev.11, from `synthminder` 0.36.0-dev.8):** the A1200 doesn't boot from the drive yet. Kickstart's detection gives up after ten rounds and shows its "insert disk" screen, without sending a command. This page holds what's been learned, the captures to work from, and the next steps, so the work can carry on from any machine.

**Next to try (dev.11): the register table with `iordy = off`** (section 6 below): `E bus = table`, `E iordy = off`, `!`, then `t` for the raw trace, and power the A1200 on.

Earlier attempts (October, the BB48 on a breadboard) are in `CHANGELOG.md` under 0.24.0-0.26.0: Gayle checks IORDY before each strobe (hence `iordy = off`), and its chip selects and address lines change as the strobe starts (hence `ide_bus_noiordy`).

## The setup

| | |
|---|---|
| Board | PCB rev 0.3, `pcb_drive` firmware, the card on SDIO |
| Host | Amiga 1200 (Gayle), floppy drive disconnected (it makes no difference) |
| Adapter | A fully buffered 44-pin angle adapter on the A1200's IDE header ([eBay 126392783980](https://www.ebay.co.uk/itm/126392783980)): powered from the port's pins 41-42; "for long cables" |
| Cable | 44-pin to 40-pin (2.5-inch to 3.5-inch), its power tail unplugged; the Bay is powered by USB |
| Image | `A1200DH0.IMG` (the A1200's CF card, 520 MB, contiguous on the card; a backup is `A1200DH0.BAK`) |
| Settings | `device0 = A1200DH0.IMG`, `device1 = none`, `isa = off`, `iordy = off` (`early_answers = on`; `bus = auto` in the file is overridden by `iordy = off`, so the program is `noiordy`) |

The same CF card, on the same adapter, boots the A1200 to Workbench.

## What was found

### 1. A buffered adapter needs IORDY pulled up (fixed in dev.9-dev.10)

With the buffered adapter and the Bay attached, the A1200 sat at a black screen. With the cable alone (no device), or the Bay on a direct cable, it reached "insert disk". The drive saw no access at all, and its pins with the A1200 on:

| Line | Seen | Idle on a working bus |
|---|---|---|
| CS0#, CS1#, DIOR#, DIOW# | 0 | 1 |
| IORDY | 0 | 1 |

With `iordy = off` the drive never touches IORDY, and PCB rev 0.3 has no pull-up on it (rev 0.4 adds R53, 10 kΩ to 3.3 V on the header side, for exactly this). A buffer drives Gayle's IORDY from whatever the drive side shows, and a floating IORDY reads low; Gayle never starts an access while IORDY is low.

**The fix:** the RP2350's internal pull-up on IORDY (pull-ups are safe; erratum E9 is about pull-downs):
- `ide_bus::begin` with `iordy = off` (dev.9);
- `ide_table::begin` always, as R53 would be for every host (dev.10).

After it: the lines idle correctly through the buffer, the drive logs the A1200's RESET# pulses, and the A1200 reaches "insert disk".

### 2. The buffer cleans Gayle's lines

The first write Kickstart makes is Device = 00h. On the direct cable the drive saw `WR CMD 6 DEVICE fa00` (junk in the upper byte); through the buffer, `0000`. The listen build (below) saw every IDE access through the buffer with its chip select asserted during the strobe (`CMD`), which the October analysis (on the breadboard and a 30 cm cable) didn't.

### 3. What Kickstart does with a working device (the reference capture)

`host/reference/a1200_cf_kickstart_listen.txt`: the PCB on the **listen build** (`bb48` env: it turns on the PCB's bus switches and never drives a line, so it runs on the PCB unchanged), on the same cable as the CF card, capture window on (`w`): 3,000 accesses from RESET#. Times in µs from the first IDE access:

| µs | Access | CF's answer |
|---|---|---|
| 0 | WR Device = 00h | |
| 1 | RD Cylinder Low (reg 4) | 2Ch |
| 2 | RD Status | 50h |
| 6 | WR Cylinder Low = 12h, RD Cylinder Low | 12h |
| 8 | WR Cylinder Low = 34h, RD Cylinder Low | 34h |
| 1,247-1,255 | The same with Device = A0h (still drive 0) | |
| 1,259 | WR Device Control = 00h | |
| 1,272 | WR Command = 10h (RECALIBRATE) | |
| 2,232 | RD Status | 50h |
| ~1,015,000 | RD Status, WR Device = A0h, WR Command = ECh (IDENTIFY), Status 58h, the data | |

Accesses come about 1-1.5 µs apart, and **each write is read back within the same microsecond** (the register test). Gayle writes 8-bit registers on both bytes (`1212`). The upper byte of 8-bit reads is junk (`4050`, `7c50`) and Kickstart ignores it.

**The A1200's clock chip shares the strobes:** 91 strobes in the capture have no IDE chip select (`NONE`), with 4-bit values written and read in sequence (`0404`, `0707`, `0101`): the real-time clock. A drive must ignore every strobe without a chip select.

### 4. What the Bay does (noiordy)

`host/reference/a1200_bay_noiordy_buffered.txt` (dev.9, through the buffer, raw trace `t` on) and `a1200_bay_noiordy_direct_cable.txt` (dev.8, direct cable). Each second, ten times:

```
EV 1731168299 WR CMD 6 DEVICE 0000 ...
EV 1731168301 RD CMD 7 STATUS 0050 ...     (round 1: one read seen, answered 50h)
EV 1732183565 WR CMD 6 DEVICE 0000 ...
EV 1732183567 NONE NONE 2 - 0050 ...       (rounds 2-10: the read never seen)
```

The Device write arrives every time. The reads after it don't. Kickstart's next access is RD Cylinder Low: the drive misses it, Kickstart gets whatever the bus holds, decides there's no drive, waits a second and tries again. After ten rounds it shows "insert disk". `commands=0`: no command was ever sent.

**The likely reason:** `ide_bus_noiordy` asks core 1 for the answer when the chip select appears, and Gayle leaves 110-200 ns from chip select to strobe. Right after a write, core 1 is still dealing with it (each register access costs core 1 about 0.5 µs of its main loop; CLAUDE.md, *Next* 4), so the answer isn't ready by the strobe, and the program loses the read. The register test, a read of a register written a microsecond earlier, is the hardest case. Not yet proven: see the next steps.

A second risk: once armed, `ide_bus_noiordy` takes any strobe as the armed register's ("whatever the lines show"), so a clock-chip strobe after an IDE chip select would be served as an IDE access.

### 5. The register table

`P t` (console; the file's `bus = auto` overrides `register_table = on`). The A1200 also reached "insert disk" without a command. In table mode the raw trace records nothing (only the RESET# lines), so there's no evidence of what failed. The table decides each read's register from the address at the strobe, which, through the buffer, should now be right; its write path goes through core 1, so the register test's read-back a microsecond after the write may come before core 1 has updated the entry.

### 6. The register table, fixed for Gayle (dev.11, simulated)

The table answers every read from the PIO's own registers within about 30 ns of the strobe, without core 1, which is what a host that never waits needs; `noiordy` waits on core 1 at every read. Through the buffer the address is valid during the strobe, which the table relies on. Three changes:
- **IORDY:** with `iordy = off` the table no longer touches it. Its data state machine had pulled IORDY low for 80 ns at every read that wasn't the data register's (the clock chip's too), and on the RP2350's weak pull-up the line then takes microseconds to rise, with Gayle waiting for it before each access: the likeliest reason the table failed in section 5.
- **Writes published at once,** before the rest of core 1's pass, for the register test.
- **The raw trace** (`t`) records every write (W's ring) and every read (the observer's ring) in table mode.

A Gayle model in `host/tests/test_register_table_sim.py` runs Kickstart's two detection rounds with the clock chip's strobes between: the table reads Cylinder Low, Status 50h, and each value written back (12h, 34h), and drives nothing for the clock chip, with core 1 up to 925 ns late with a write at Gayle's usual 1 µs spacing (625 ns at 700 ns). How late core 1 really is hasn't been measured; on the rig the table passes slices 3-4.

`bus = table` sets it from the file (`register_table = on` is overridden by `bus = auto`); console `P t` still sets it until a power cycle, and `iordy = off` comes from the file either way.

## Next steps

0. **The A1200 on dev.11's table** (`bus = table`, `iordy = off`, `t` on, `scripts/retro_log.py` recording): if detection still fails, compare the trace with the CF card's capture access by access, and see whether the read-backs come back stale (core 1 too slow: then a PIO path for plain register writes) or the reads look wrong (the address during the strobe).
1. **A Gayle host model from the capture** (done for the table in dev.11) (`host/hddproto/pio_sim.py`; `host/tests/test_pio_bus_noiordy_sim.py` has a Gayle model from October to extend): chip select to strobe 110-200 ns, strobes of about 300 ns, accesses 1-1.5 µs apart, Kickstart's exact sequence above (WR Device; RD Cylinder Low; RD Status; WR/RD Cylinder Low twice), and clock-chip strobes with no chip select in between. Run `ide_bus_noiordy` (with core 1's answer delay modelled) and the register table against it.
2. **Fix whichever fails**, e.g. for `noiordy`: answer a register read from a value core 1 left ready before the write was even processed, as the table does; never take a strobe without a chip select as an access. For the table: the write-to-read-back path within 1 µs.
3. **A trace for the register table** (done in dev.11: `t`).
4. **Back on the A1200:** the buffered adapter, dev.10 or later, the settings above, the raw trace on, and `scripts/retro_log.py` recording. Compare the Bay's trace with the reference capture access by access.

## How to reproduce

### Dev.11 on the A1200 (from the Mac)

1. **Build:** `git pull` on branch `amiga`, then `~/.platformio-venv/bin/pio run -e pcb_drive`. Check `.pio/build/pcb_drive/firmware.uf2` is new.
2. **Flash:** the PCB is on the listen build: console `b` (or hold BOOTSEL and plug in), then copy the UF2 to the `RP2350` drive. `h` must say `READY HDDSynthPro 0.36.0-dev.11 slice3`.
3. **Settings,** once (by console `E`, then `!`): `E device0 = A1200DH0.IMG`, `E device1 = none`, `E isa = off`, `E iordy = off`, `E bus = table`. `S` should then show `iordy=off` on its `CONFIG dmarq=...` line, with no `CONFIG bus program set from the console` line, and `s` should show `bus=table` (a console `P` from before is gone after a power cycle; `P 0` clears it otherwise).
4. **Log:** `HDDPRO_PORT=/dev/cu.usbmodemNNNN python3 scripts/retro_log.py a1200` (it records everything to `host/runs/`). In its console: `t` (the raw trace; off after every restart), `p` (the pins: with the A1200 on and idle, CS0#, CS1#, DIOR#, DIOW# and IORDY all 1).
5. **Power the A1200 on** (the buffered adapter, the Bay on USB power). Wait for Workbench or "insert disk", then `s` and `S` in the console, and stop the log.

**What to look for in the trace,** against `host/reference/a1200_cf_kickstart_listen.txt` (the CF card; table in section 3):
- Each round: `WR CMD 6 DEVICE`, `RD CMD 4 LBAMID`, `RD CMD 7 STATUS 0050` (or `xx50`: the upper byte is the bus's), then `WR CMD 4 LBAMID 1212` and `RD CMD 4 LBAMID xx12`, then `3434` and `xx34`. A `CMD` line (RECALIBRATE, then IDENTIFY) means detection passed.
- **Stale read-backs** (`RD ... LBAMID` showing the value before the write): core 1 publishes too late. Then the fix is a PIO path for plain register writes.
- **Reads missing or at the wrong register:** the address isn't valid during the strobe after all (compare the `NONE` lines and the pins).
- **No accesses at all** (only `RESET` lines): IORDY or the lines idle low again; `p` with the A1200 on.
- The clock chip's strobes show as `NONE` lines; the drive must not answer them (their values are the clock's).
- Writes and reads come from two rings, so their order in the trace is right only to within about a microsecond.

The trace format is in `CLAUDE.md` (*Trace format*); `EV` lines' last field is the raw sample.

### Other

- With `bus = auto` instead (`noiordy`, dev.10's run): `E bus = auto`, `!`; `noiordy` misses the reads after each write (section 4).
- A card that ran on the BB48 (SPI) runs over SPI on the PCB until it's taken out and put back.
- The reference capture: the listen build (`pio run -e bb48`) on the PCB, the Bay and a working device on one IDE cable, `w` (capture window), then power on the A1200.
- Simulation: `cd host && python3 -m pytest -q tests/test_register_table_sim.py -k "kickstart or read_back or clock_chip"` runs the Gayle model against the table.
- After the A1200: put the card back for the Evesham (`CLAUDE.md`, *The card's settings*): `bus`, `iordy` and `device0` at least.
