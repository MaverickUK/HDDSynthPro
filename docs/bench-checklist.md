# Next bench session (rewritten 18 Nov 2026)

What's left needs the rig: the Windows PC (an administrator Git Bash), the USB adapter (JMicron `152D:2338`) and the PCB (rev 0.3) on COM4. The rig's quirks are in `CLAUDE.md` (*Rig*). Older checklists are in this file's git history, and their steps stay in `host/bench_session.py` (`--group nov10`).

Four things are owed, and `bench_session.py`'s `nov18` steps run them all:
- **0.35.0's regression run through the adapter** (*Next*, 0): `-m speed` on early answers, the data lane, the register table and in multiword DMA; slices 3-4 with the defaults; slices 3-4 with the program flipped every second (console `Z`); and slices 1-2 by re-detection on the register table (owed since 0.33.0).
- **Branch `synthminder`'s core 1 layout** (`check_core1_layout.py` fails there: the block is 224 bytes longer, for SynthMinder's 8Dh): the same speeds on 0.35.0, then `synthminder`, then 0.35.0 again (A-B-A), so the adapter's drift shows.
- **Branch `ram-overlay-heap`** (0.35.1-ram.1: the RAM overlay on the heap, static RAM 93.8% to 42.9%; its core 0 block moved 20 bytes): the same speeds and slices 3-4, between 0.35.0's second and third rounds (`d`, then `a3`).
- **Branch `rev04-firmware` on rev 0.3:** slices 3-4, and `board=rev0.3/bb48`, `card_power_cycles=0` and no `PINS DASP#` line: the new code sees rev 0.3 and touches nothing.

## 1. Bring the rig back

- [ ] **Restart the PC.** The adapter declined during the card soak (18 Nov: its USB serial reads `152D20337A0C`, which should be `984220294922`); only a restart has cleared that.
- [ ] Windows shows `HDDSYNTH PROTO` as disk 1; the board is on 0.36.0-dev.1 (`synthminder`) serving `SYNTHMIN.IMG`.
- [ ] The worktrees beside this checkout: `HDDSynthPro-v035` (detached at `v0.35.0`), `HDDSynthPro-synthminder`, `HDDSynthPro-rev04` and `HDDSynthPro-ram`, each up to date with its branch (`git -C ../HDDSynthPro-synthminder pull --ff-only`, and the same for `-rev04` and `-ram`).

## 2. Unattended: `bench_session.py` (about two and a half hours)

From an administrator Git Bash in `host/`:

```
export PYTHONUTF8=1 HDDPRO_DISK=PhysicalDrive1 HDDPRO_PORT=COM4
python bench_session.py --dry-run --leave-on b    # what it will do
python bench_session.py --leave-on b
```

It flashes each build with the rig watchdog, serves the test disk (`T`), picks the bus program (`P`), sets `dma = on` for the DMA steps (and puts it back), makes the adapter re-detect the drive (`V`, then `pnputil`), and runs the tests. At the end it puts `HDDSYNTH.INI` back, clears `T` and `P`, and flashes a normal `synthminder` build. Results: `host/runs/bench-<time>/summary.md`, with the A-B-A table at the end. If a step fails, the rest still run; `--from STEP` carries on after a stop.

## 3. After it

- [ ] **0.35.0's regression run:** the slices passed, the `Z` step's switches lost nothing, and the speeds are as 0.34.0's (CHANGELOG 0.35.0, [speed-log.md](speed-log.md); those are from the Mac, so compare the runs here with each other, and with the Windows figures in speed-log). Record it in `main`'s CHANGELOG (*Unreleased*, *Tested*) and strike it off `CLAUDE.md`'s *Next*, 0.
- [ ] **Slices 1-2 on the register table:** strike them off *Next*, 1, if they passed (slice 1's IORDY check fails on every build since 0.28.0: known).
- [ ] **`synthminder`'s layout:** if `b`'s speeds are within the spread of `a1` and `a2`, add the table to `synthminder`'s `docs/speed-log.md`, run `scripts/check_core1_layout.py pcb_drive --update` and `bb48_drive --update` there, commit and push (CI then passes on that branch). If `b` is slower than both, find which function's placement moved before recording anything.
- [ ] **`ram-overlay-heap`:** `d-slices` passed with `overlay=256/512` in its stats, and `d`'s speeds are within the spread of `a2` and `a3`: add the table to that branch's `docs/speed-log.md`, run `scripts/check_core1_layout.py pcb_drive --update` and `bb48_drive --update` there, commit, and it can merge. Its `ring_max` figures (in the `d` steps' stats; only this branch has them) say how much of the trace ring's 128 KB the drive build uses: the next thing to shrink.
- [ ] **`rev04-firmware`:** the board check says *as expected* and the slices passed: note it in that branch's CHANGELOG (0.35.1-dev.1, *Tested*).

## 4. Retro PCs

- [ ] **The new defaults (`bus = auto`, `dma = auto`) on the Pentium 200 and the ICL 386** (*Next*, 0). The Pentium's PIIX changing timing without a reset is the risk. On each, the table in [auto-bus.md](auto-bus.md#what-to-record-on-each-pc) at three moments (the BIOS has found the drive, the DOS prompt, after a benchmark; `r` before each). **Never `P l` (the data lane) on an ISA machine.**

## 5. Put the card back

- [ ] The owner's settings as they were (`S` to check). Only `HDDSYNTH.INI` changes; never format the card.
- [ ] No console `T`, `J` or `P` left set (`S`: the image served, no `CONFIG bus program set from the console`).
- [ ] A normal build flashed (no `RIG_WATCHDOG_MS`) before the board goes anywhere else (`--leave-on` does it).
