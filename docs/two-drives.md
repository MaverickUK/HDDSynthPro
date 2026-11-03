# Two drives, and a folder as a drive

**Status: an experiment on the `two-drives` branch.** Built and host-tested only; nothing here has run on the rig or a retro PC yet. What to try first is at the end.

The drive can answer as both devices on the cable, device 0 and device 1 (BIOS menus and drive jumpers call them master and slave), each serving its own disk, so a retro PC shows two hard disks (C: and D:). Device 1 can be an image, like device 0, or a **folder on the SD card**, served read-only as a FAT16 volume built on the fly.

```ini
device0 = DOS622.IMG   # the first drive, as before (`image`, its old name, still works)
device1 = GAMES        # a folder on the card: read-only
# device1 = DATA.IMG   # or a second image
```

With no `device1` the drive is the lone device 0 it has always been, and nothing below changes how it behaves.

## Two devices from one drive

An IDE cable carries one set of registers, which both devices see. ATA's rules for two devices, and how the emulator follows them:

| ATA | Here |
|---|---|
| Both devices latch every write to the command block (the task file) | One task file, shared, as on the cable. |
| The Device register's DEV bit selects which device answers reads and runs commands | The device keeps each unit's own state (identity, geometry, status, error, interrupt, standby, write cache) and swaps the selected unit's into place when the host changes DEV. |
| The host must not change DEV while the selected device is BSY or in a data phase (DRQ) | So only one command runs at a time, and one command engine is enough: the sector buffers, read-ahead, write-behind and DMA are shared, and each `Fetch` carries the unit it's for. |
| Device 0 answers for an absent device 1 (Status 00h, its commands refused) | Unchanged, with no `device1` configured. |
| After a reset, each device sets the ATA signature; device 0's Error register reports both devices' diagnostics (01h: both passed or device 1 absent) | Both units reset together, each with the signature and error 01h. |
| Device 1 asserts DASP# at power-up and PDIAG# after a reset, which device 0 waits for (up to 31 s) | Not needed between the two units, since one chip plays both. The BB48 has neither line, so a host that reads DASP# to decide whether device 1 exists won't see it; most BIOSes just select device 1 and probe it. PCB rev 0.4 adds both. |
| Only the selected device drives INTRQ | The selected unit's pending interrupt drives it. |

**Why swap on select rather than route every access?** Core 1 answers each register read in about 1,030-1,100 ns, against ATA's 1,250. Routing every access through a "which device?" step would add to that on every read. Swapping happens only on a Device register write (once per command at most), and every other path is the one-drive code unchanged. What the swap moves is a few bytes and a pointer.

**The cost:** a second IDENTIFY block (512 bytes) and a second unit's state; with no device 1, nothing per access.

Each unit has its own IDENTIFY (device 1's serial ends in `-D1`, its model is `HDDSYNTH PRO D1`) and its own geometry (from its own image's partition table, or `geometry`); its transfer modes are device 0's.

## A folder as a drive (read-only)

`device1 = GAMES` (any folder in the card's root, with its subfolders) serves the folder's files as a disk:

- **At start-up** core 0 lists the folder and lays out a FAT16 volume (`src/folder_disk.*`, pure C++, host-tested): an MBR with one partition, a boot record, two FATs, a root folder and the files' clusters. Each file and folder gets one contiguous run of clusters, so the FAT never needs storing: entry *n* is *n*+1 inside a run, end-of-chain at its end.
- **Every sector is computed when it's read**, as the welcome disk's are: the boot record and FATs from the layout, directory sectors from the list of entries, and a file's sectors read from the file on the card, through one open file kept from read to read (a DOS `COPY` reads one file sequentially).
- **RAM:** 32 bytes per file or folder (its 8.3 name, size, first cluster, and where it is on the card), on the heap. The list is capped at `cfg::FOLDER_MAX_ENTRIES` (1,024) entries and leaves `cfg::FOLDER_HEAP_LEFT` (16 KB) of heap free; a bigger folder is served with what fits, and `S` says it was cut short and why.
- **Names** are 8.3 short names made from the card's long names as Windows makes them (`LONGFI~1.TXT`, `~2` if that's taken). Long names, for Windows 9x, can come later as VFAT entries.
- **Size:** up to 2 GB (FAT16's limit, with 32 KB clusters), enough for DOS 3.31 onwards. The cluster size is the smallest that fits the folder.
- **Snapshot:** the folder is read once, at start-up. Files changed on the card later are served as they were listed (sizes, dates), so change the card's files with the drive off.
- **Writes are refused** (the command fails with ABRT; DOS reports a write fault). Nothing on the card is ever written: the folder drive only reads.

### Read/write folders: why later

A host writing to a FAT volume writes raw sectors: FAT entries, directory entries and data clusters, in its own order, with its own caching. To keep the card's files in step, the drive would have to:

1. Hold the whole FAT and every directory in RAM (or work them out from writes), to know what each written sector means.
2. Recognise creates, renames, deletes, truncations and appends from directory and FAT changes, sometimes split across many writes, and only act once the host has finished (it never says when).
3. Put new data somewhere before knowing which file it belongs to (DOS writes the data clusters before the directory entry), then move it into the right file on the card.
4. Do all of that without ever leaving the card's file system inconsistent if power is cut.

For a 2 GB FAT16 volume the FAT alone is 128 KB, more than the 48 KB the RP2350 has free, so it needs the PSRAM PCB rev 0.4 adds (8 MB on GPIO 47). Until then, a read/write disk is an image.

## Testing without hardware

All of these pass, locally and in CI (with ASan and UBSan there):

- `test_two_drives.cpp`: two units' register behaviour (selection, status per unit, the signature after resets, diagnostics, interrupts), and the virtual host playing a BIOS's probe of both devices, then reading and writing each one's disk.
- `test_folder_disk.cpp`: short names, and volumes read back through a small FAT16 reader in the test (the MBR, boot record, both FATs, folders and chains, as DOS follows them): cluster sizes up to 32 KB, the 2 GB budget, the entry cap, the root's 511, a folder spanning clusters.
- `make_folder_img` (`test/tools`): builds the volume from a real directory into an image file, so real FAT tools can check it. `scripts/check_folder_img.py` reads it through pyfatfs and compares every folder and file with the directory, byte for byte; CI also runs `fsck.fat -n` and `mdir` (mtools) on one with nested folders, long names, an empty file, an empty folder and a 5 MB random file.
- `test_folder_disk.cpp`'s last test runs the whole path: the folder as device 1 behind `ata::Device`, read through ATA commands, sector for sector the same as the volume, and a write refused with ABRT.

## On the rig: what to try first

1. **One drive, no `device1`:** `-m speed` and slices 1-4, against 0.28.0's figures: core 1's code moved, so check it didn't slow (CLAUDE.md, core split). Done for `-m speed` on 3 Nov (0.29.0-2d.2 against 0.28.0, the same day and rig: reads 1.18 MB/s both, writes 1.06 against 1.08, 74.9 random reads a second both).
2. **Two images** (`device1 = SECOND.IMG`): the rig's USB adapter (a JMicron JM20337) shows device 1 as a second disk once its USB connection restarts (3 Nov: 300 rounds of interleaved reads and writes across both, 0 mismatches). Device 1's `CMD` lines on the console end in `d1`. **On retro PCs (3 Nov evening, [retro-test.md](retro-test.md#results-3-nov-2026-evening)):** the ICL 386 (ISA card, `isa = on`) and the Evesham (on the data lane) both found device 1 without DASP#. On the 386, FDISK, FORMAT and `FC /B` on D: were fine (1,489 device 1 commands, none failed); on the Evesham, `FC /B` was identical (345, none failed).
3. **A folder** as device 1, on the retro PC: `DIR D:`, `COPY` a big file and compare it (`FC /B`), then a write (it should fail cleanly). **3 Nov: the board doesn't start with one.** With `device1 = GAMES` the BB48 never shows its USB serial port, at every power-up; without the card it starts at once. The same folder builds cleanly in `make_folder_img` (ASan) and mounts with every file right, so the fault is on the board's side: `sd_card`'s `SdFolderSource` (opening entries by index, copying `FsFile`s) or `chooseDevice1()` (the list's allocation from a small heap: the build uses 91.6% of RAM). Next: a build that prints each step of `chooseDevice1()` and flushes, to see where it stops. To recover meanwhile, take the card out, start the board, put the card back and `E device1 = none`.
   - The test folder: `cd host && .venv/bin/python -m hddproto.test_folder make /Volumes/CARD/GAMES`. Its files' contents follow from their names, and its `CHECK.BAT` copies everything from D: to `C:\HDDTEST` and compares each copy (`FC /B`); `test_folder check` checks the copies on the Mac afterwards.
