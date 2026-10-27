# Telling sequential from random activity

The original HDD Synth is driven by the host's HDD activity LED: one bit, busy or not, so it can only play one "access" sound. HDD Synth Pro *is* the drive, so it sees every command the host sends: which sectors, how many, and when. That's enough to tell a head streaming along the disk from one jumping about, and to choose a sequential or a random sound to match.

This note covers how it works, what the prototype proves, and the limits.

## Why this is feasible

- **Each command carries its start sector (LBA) and length.**
  - A sequential read starts where the last one ended.
  - A random read jumps. The size of the jump is the seek distance, and on a real drive that is what makes a seek loud or quiet: short seeks tick, full-stroke seeks clunk.
- **PATA has no command queueing.** Commands arrive in the order the head would service them, so the stream is a faithful picture of what a real drive would do.
- **What the drive sees is what a real drive would see.** Whatever the host's own cache absorbs never reaches either drive, so nothing needs faking.

## How it works

The classifier (`src/activity.h`) runs on core 0, off the bus's time-critical path. It's fed from the command events already going into the trace.

1. **Each command is sequential or a seek.** Sequential means it starts at, or up to 64 sectors after, the end of one of 4 recently active streams, or up to 8 sectors before it (a re-read).
   - Tracking several streams keeps two files being read at once (an interleaved copy) sequential.
   - Six or more competing streams read as random, which is right: on a real drive the head thrashes.
2. **The state comes from the share of seeks** among the commands in the last 250 ms:
   - 25% or less: **Sequential**.
   - 50% or more: **Random**.
   - In between: no change (hysteresis).

   A switch between Sequential and Random must hold for 100 ms before it takes effect, so the sound never flickers.
3. **Idle** comes 150 ms after the last command ends, and never during a long transfer.
4. **A burst always starts with a seek** to wherever the data is.
   - That first command doesn't count towards the seek share. Its seek distance is still reported, so a seek click can be played.
   - The drive wakes as Sequential, and needs at least 3 counted commands before it calls Random.
   - For one window after waking it follows the share at once, without the dwell, so a random burst is recognised within about 5 commands.

The output is an `ACT` line on each change: `ACT <t_us> <IDLE|SEQ|RAND> <seek %> <average seek, sectors>`. The console's `s` command shows the totals and the current state.

All the thresholds are in `activity::Config`. They're starting values, chosen against synthetic workloads; real traces will refine them.

## What the prototype proves

| Test | What it shows |
|---|---|
| `test/native/test_activity.cpp` | Synthetic workloads: sequential streams, random reads, idle, two interleaved streams, too many streams, metadata reads sprinkled into a copy, phase changes. It checks the time spent in each state, the switch latency, and that there's no flicker |
| `host/tests/test_activity.py` (`-m activity`, drive build) | The real thing. The Mac reads the drive in phases (8 MB sequential, 2 s idle, 300 random 4 KB reads, 2 s idle, two interleaved streams), each bracketed by `MARK` lines. Each phase must spend at least 90% of its time in the right state |
| `test/tools/classify_trace` | Replays any recorded trace through the classifier, to tune the thresholds on real workloads without hardware: `.pio/native-tests/classify_trace < host/runs/<run>/trace.txt` |

The rig test runs on the generated disk, so it can come straight after slice 3. The more convincing tests come later, once the drive can be written (slice 4) and holds a real file system (slice 5):
- copying files;
- booting DOS or Windows;
- running a defragmenter (long runs of both kinds);
- a database-like random workload.

Record their traces, and replay them through `classify_trace` while listening to what each state would sound like.

## Limits and open questions

- **Host caching hides some activity.** A file read twice may come from the host's cache the second time. That's realistic: a real drive would be just as quiet.
- **Read-ahead and write-behind** in modern OSes merge small accesses into large sequential ones. Again realistic; DOS-era hosts do far less of it, so they'll sound more random.
- **File-system metadata.** FAT reads and directory updates between file data are short seeks. The classifier tolerates about 1 in 8 commands without flipping to Random. Whether that matches what a listener expects is a tuning question.
- **USB bridges split large transfers** into chunks of up to 128–256 sectors. They stay contiguous, so they stay sequential.
- **DMA and READ/WRITE MULTIPLE** (later layers) are already in `isMediaCommand`, so the classifier needs no change when they arrive.
- **More than two states.** The seek distance is already measured, which opens up richer sounds later:
  - short versus long seeks (the "average seek" in each `ACT` line);
  - reads versus writes;
  - spin-down after a long idle (the standby commands are already handled).

## Sending it to an HDD Synth

Since 0.9.0 the drive sends its activity (busy, the class above, and whether the last command read or wrote) to an HDD Synth over a one-way UART on GPIO 0, as `$HDA` sentences: on every change and every 200 ms. The protocol, wiring and status are in [activity-link.md](activity-link.md); console `s` shows `link_sent=` and the last sentence, and `L` turns it off and on.
