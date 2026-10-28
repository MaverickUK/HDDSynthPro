# Unattended soaks: surviving a host reset

Status: **not built.** Written up on 2026-10-26 after the overnight soak on 0.8.22 stopped 7 minutes in.

The soak (`host/tests/test_soak.py`, `-m slice6`) has `HDDPRO_SOAK_TOLERATE_RESETS=1` to ride out macOS resetting the adapter: up to 3 resets, each logged as `HOST RESET`. It has never worked unattended. Two bugs stop it, and either one alone ends the run.

## What happened on 2026-10-25

- **20:57** soak started, 10 hours, on 0.8.22 serving the generated disk.
- **21:02** 5-minute check clean: 3,419 reads, 1,888 writes, 0 mismatches, IORDY max 1,120 ns, 440,913 sectors checked on the cable, 0 bad.
- **21:03:01.66** every USB device behind the rig's hub logged `pipe stalled` on endpoint 0 in the same millisecond: the BB48's serial port, the IDE adapter, the NS1081, and both halves of the hub. `pmset -g log` shows no sleep or power event (on AC, `caffeinate -i -s` held). Cause unknown; it may have been the HDD Synth being switched off or unplugged while its ground was joined to the BB48 (not confirmed).
- **The drive was fine.** Its last command finished normally (status 50) at 21:03:01, and its trace has no gap. The adapter then sent nothing for 62 s.
- **21:03:32** macOS: `USB to ATA/ATAPI bridge` endpoint 0x81 timeout, `fConsecutiveResetCount = 1`.
- **21:04:03** macOS reset the adapter (`will be reset!`, `disk5: device/channel is not attached`). The adapter pulsed RESET# three times, sent IDENTIFY and SET FEATURES, and the disk came back as **`disk6`**, owned by root.
- **21:04:03** soak: `HOST RESET 1: I/O error: [Errno 6] Device not configured; waiting for the re-detection`.
- **21:04:13** soak failed: `sudo chown peterbridger /dev/disk5 /dev/rdisk5` returned 1. Nobody was there to type a password, and the node it named no longer existed.

Evidence: `host/runs/20261025-205710-test_soak` and `20261025-205710-serial.log`, and `log show --start "2026-10-25 21:02:58" --end "2026-10-25 21:04:10" --predicate 'process == "kernel"'`.

## Bug 1: re-claiming the disk needs a password

`MacHost.open_raw()` (`host/hddproto/host_macos.py`) runs `sudo chown $USER /dev/diskN /dev/rdiskN` when the raw device isn't writable. macOS gives every re-created disk node back to root (`crw-r----- root operator`), so after a reset `sudo` asks for a password. With no terminal it fails at once.

Fix: a root-owned helper that can only claim the HDDSynthPro disk, with a sudoers rule that lets it run without a password.

1. **`scripts/hddpro-claim-disk`**, installed to `/usr/local/sbin/hddpro-claim-disk`, owned `root:wheel`, mode `755`, so the user can't edit what sudo runs.
   - Argument: a whole-disk name, `diskN`. Reject anything else (`^disk[0-9]+$`, no slices, no paths).
   - Check with `diskutil info -plist diskN` that it is the emulated drive: external, physical, whole disk, `IORegistryEntryName` contains `HDDSYNTH` (match on `IORegistryEntryName`, not `MediaName`; see CLAUDE.md on the NS1081) and size 67,108,864 bytes (the generated disk) or the size of `HDDSYNTH.IMG`.
   - Refuse, with a message, if any check fails. It must never be able to claim the Mac's internal disk or any other external disk.
   - Then `chown "$SUDO_USER" /dev/diskN /dev/rdiskN`, taking the user from `SUDO_USER`, not from an argument.
2. **`/etc/sudoers.d/hddpro`** (mode `440`, edited with `visudo -f`):
   ```
   peterbridger ALL=(root) NOPASSWD: /usr/local/sbin/hddpro-claim-disk
   ```
   Never a rule for `chown` itself: `chown` on `/dev/rdisk0` would hand over the Mac's internal disk.
3. **`scripts/install-claim-disk.sh`**: copies the helper and the sudoers file into place with `sudo install`, then runs `visudo -cf` on the sudoers file. Run once by the owner, after reading both.
4. **`open_raw()`**: if the helper is installed, run `sudo -n /usr/local/sbin/hddpro-claim-disk diskN`. `-n` makes sudo fail instead of waiting for a password. Otherwise keep today's `sudo chown`, which asks once at the start of an attended run.

## Bug 2: after a reset the disk has a new name

`reopen()` in `test_soak.py` reopens the `disk` found at the start (`disk5`). The re-detected disk is `disk6`, so even with bug 1 fixed it would retry the dead node for `DISK_BACK_S` (180 s) and fail.

Fix: in `reopen()`, find the disk again with `MacHost.find_disk(expected_bytes=…, timeout=DISK_BACK_S)` each time round the loop, open that, and `dup2` it onto the old `fd` as now. Log the new name (`resumed on disk6`). `pause()` (the idle pause for standby) reopens the same way and needs the same change.

## Smaller fixes found at the same time

- **Don't hide the real error.** The `finally:` block that restores the write window writes to the dead `fd` and raises a second `OSError`, which hides the first in the pytest summary. Catch and log that error there, so the original failure is the one reported.
- **Say why it stopped, in one line.** On any failure, `note()` a one-line reason before raising (for example `FAILED: could not claim disk6 (helper not installed)`), so the log tail is enough.
- **`bus_contested` and `bus_stalls` stick at 16,777,215** (2^24 − 1) after a long run: the counters fill up and stay there. Widen them, or reset them with `r`. Not a soak failure, but the numbers are meaningless once they hit it.

## Before leaving a soak

- Check `/usr/local/sbin/hddpro-claim-disk` is installed and `sudo -n /usr/local/sbin/hddpro-claim-disk diskN` works. If not, a host reset will end the run.
- Leave the rig alone: don't unplug or switch anything that shares its ground (the HDD Synth on the activity link included), and keep unlocking the Mac to a minimum (on 2026-10-25 a reset at 13:34 followed an unlock by 20 s).
- If mobile push is on (`/config`), the soak's end reaches the owner's phone; otherwise nobody hears of a failure until morning.

## Testing the fix

- `sim` tests for the helper's argument and disk checks, with `diskutil` output recorded from the rig (the internal disk, the CF card in the adapter, the generated disk, an image).
- On the rig, a reset on demand: start a soak with `HDDPRO_SOAK_TOLERATE_RESETS=1`, then unplug and replug the adapter's USB cable. A physical replug gives the same sequence as a macOS reset: RESET#, IDENTIFY, a new disk name. Expect `HOST RESET 1 … resumed on diskN` and the soak carrying on with no password asked.
