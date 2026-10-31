# The benchmark image

`DOSBENCH.IMG` is a standard disk for testing HDD Synth Pro in any PC with a 386 or later. It boots FreeDOS and runs `HDDBENCH`, which measures the drive and checks its data the same way on every machine. Results from different PCs therefore compare directly.

## Use it

1. Build it: `python3 tools/bench-image/build_image.py` (needs `nasm` and `mtools`; FreeDOS is downloaded once into `.cache/`). The image is `tools/bench-image/build/DOSBENCH.IMG`.
2. Copy it to the SD card, and set `image = DOSBENCH.IMG` in `HDDSYNTH.INI` (console `E image = DOSBENCH.IMG`, then `!`). For a fresh start each time, copy it again: the tests only write the scratch area and `C:\BENCH.LOG`, but a fresh copy rules even that out.
3. In the PC's BIOS, set the drive to auto-detect, or user type 260 cylinders, 16 heads, 63 sectors.
4. Boot. `AUTOEXEC.BAT` runs `HDDBENCH`. After the benchmark it waits 10 s, then starts the soak, which runs until a key is pressed.
5. Read the results on the screen, or afterwards in `C:\BENCH.LOG` (on the Mac: `mtype -i DOSBENCH.IMG@@32256 ::BENCH.LOG`). Keep the drive's console log too (`scripts/retro_log.py`): it shows the transfer mode, late reads and the bus observer's checks.

`HDDBENCH /NOSOAK` runs only the benchmark, and `HDDBENCH /SOAK` only the soak.

## What it does

Everything goes through the BIOS (INT 13h, CHS addressing, one track per call), as DOS and Windows' compatibility mode do.

| Step | What | Measures |
|---|---|---|
| Sequential read | 16 MB from the pattern area | MB/s (1 MB = 1,000,000 bytes, as the rig tests) |
| Random read | 500 single sectors anywhere on the disk, from a fixed seed | ms per read |
| Sequential write | 8 MB to the scratch area | MB/s |
| Check written | The 8 MB just written, read back | bad sectors |
| Check pattern | The whole 32 MB pattern area, read back | bad sectors |

A bad sector prints `BAD lba=... word=... seen=... want=... bad_words=...` (eight per pass at most) and makes the result FAIL. BIOS errors (after three tries) and retries are counted too.

Each soak pass runs the following, until a key is pressed:
1. It checks the pattern area.
2. It writes the scratch area with a new seed, so stale data can't pass the check, and checks it.
3. It makes 500 random reads in the pattern area and checks each one.

Timing uses the BIOS clock (55 ms ticks), so short steps are approximate. The timed steps take seconds even on fast machines.

## The disk

128 MiB: 260 cylinders × 16 heads × 63 sectors, which is what the drive's IDENTIFY reports for this size. That's inside every BIOS's CHS limit, so no translation is needed.

| LBA | Cylinders | Contents |
|---|---|---|
| 0 | 0 | MBR (`mbr.asm`), one active FAT16 partition |
| 1 | 0 | The `HDDBENCH` header: the layout below |
| 63-131039 | 0-129 | FAT16, 64 MB: FreeDOS 1.3 (kernel 2043, FreeCOM 0.85a), `HDDBENCH.COM`, `AUTOEXEC.BAT`, `FDCONFIG.SYS`, `README.TXT` |
| 131040-196559 | 130-194 | Pattern area, read only: every sector holds `pattern(LBA)` |
| 196560-261071 | 195-258 | Scratch area: the write tests |
| 261072-262079 | 259 | Unused: BIOSes keep the last cylinder for diagnostics |

The pattern, for LBA L with 16-bit halves lo and hi, and a seed S (0 in the pattern area):
- word 0 is lo ^ S;
- word 1 is hi ^ S;
- word i ≥ 2 is (lo + i × 9E37h) ^ (hi + i) ^ S.

`hddbench.asm` and `build_image.py` both implement it.

## Files

- `hddbench.asm`: the benchmark, a 7.5 KB `.COM` for NASM; it needs a 386 or later.
- `build_image.py`: builds the image.
- `mbr.asm`: the boot code. It reads the active partition's boot sector by its CHS, with DL taken from the boot flag, because old BIOSes don't always pass DL.
- `dos/`: `AUTOEXEC.BAT`, `FDCONFIG.SYS`, `README.TXT`.
- `test_qemu.sh`: boots a copy in QEMU (in Docker) and prints its `BENCH.LOG`. A test before carrying it to a real PC: `tools/bench-image/test_qemu.sh 486`.

MS-DOS 6.22 would do as well, but it can't be shared in a public repository, so the image uses FreeDOS (GPL). HDDBENCH runs on either.
