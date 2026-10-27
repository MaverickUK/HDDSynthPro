# Brief: activity link from HDDSynthPro, and `sequential.wav`

For Claude Code working in **HDDSynth_Native** (the MK5 firmware). Read this repo's CLAUDE.md first; everything below fits its rules (core 0 owns all I/O, core 1 only mixes from RAM, host-testable pure C++, SemVer and CHANGELOG with every change).

## Goal

HDDSynthPro is a separate board that emulates an IDE hard disk, so it knows exactly what the drive is doing. It will send that over a one-way UART. When a new setting is on, the HDD Synth listens for it and uses it instead of the optocoupler input:

- **busy** drives the access sound exactly as the activity input does today (with the same `ACCESS_HOLD_TIME_MS` release);
- **class** chooses the sample: a new, optional per-pack **`sequential.wav`** while the drive is reading or writing sequentially, `access.wav` for random access.

Without the setting, or without HDDSynthPro attached, nothing changes.

## Hardware

The link uses header **J6**, the OLED's header, so the OLED and the link can't be used together:

| J6 pin | MK5 signal | Link use |
|---|---|---|
| 1 | 3.3 V | not connected (each board has its own supply) |
| 2 | GND | ground, to HDDSynthPro's GND |
| 3 | GP25 (OLED SCL) | **UART1 RX**: data from HDDSynthPro (through a 1 kΩ resistor at its end) |
| 4 | GP24 (OLED SDA) | UART1 TX: reserved, not used in version 1 |

On the RP2350, GP24/GP25 are UART1 TX/RX (function 2). In Arduino-Pico that is `Serial2`: `Serial2.setRX(25); Serial2.setTX(24); Serial2.begin(115200);`. Check the pin assignment against the RP2350 datasheet's GPIO function table before relying on it.

## Protocol (version 1)

115200 baud, 8N1, ASCII lines:

```
$HDA,1,<busy>,<class>,<op>*<checksum>\r\n
```

- `1`: protocol version.
- `<busy>`: `0` or `1`. Already held for at least 25 ms after each command starts, by the sender.
- `<class>`: `I` idle, `S` sequential, `R` random.
- `<op>`: `R` read, `W` write, `-` none. **Parse it, but don't use it yet** (a later version may add read and write samples).
- `<checksum>`: two upper-case hex digits, the XOR of every byte between `$` and `*` (as in NMEA).

The sender sends a sentence on every change and every 200 ms as a heartbeat.

Receiver rules:
- Ignore lines that don't start with `$HDA`, fail the checksum, or are longer than 80 bytes. Accept any version number: a later version keeps these fields and adds any new ones at the end, so ignore fields after `<op>`. Accept lower-case hex in the checksum.
- The link is **present** from the first valid sentence and **lost** 1 s after the last. While lost, use the optocoupler input as today. Log each change through `Console` (`[link] up` / `[link] lost`).

Known answers for tests:

```
$HDA,1,0,I,-*28      idle
$HDA,1,1,S,R*4C      busy, sequential reads
$HDA,1,1,R,W*48      busy, random writes
$HDA,1,1,S,W*49      busy, sequential writes
$HDA,1,0,R,R*4C      not busy this instant, recent mix still random
```

## Changes

1. **Setting `ACTIVITY_LINK`** (bool, default `false`) in `settings.cpp`, and in the README's settings table. If both `ACTIVITY_LINK` and `OLED_ENABLED` are true, the link wins: don't start the OLED, and log why.
2. **`src/link_parser.h/.cpp`, pure C++ (no Arduino headers), host-tested:**
   - feed it bytes one at a time; it assembles lines (dropping anything over 80 bytes) and validates them. HDDSynthPro's `activity_link::parseLine` and `Receiver` (`src/activity_link.*`, with its tests in `test/native/test_activity_link.cpp`) do exactly this and are pure C++: copy them;
   - it yields `LinkState { bool busy; Class cls; Op op; }` for each valid sentence;
   - `LinkWatch`, a pure helper: given "valid sentence at t" and "now", is the link present (1 s timeout)?
3. **Receiving, on core 0:** start `Serial2` in `setup()` when the setting is on, with `setFIFOSize(512)` or so, so a slow loop pass (an SD refill) can't overflow the 32-byte hardware FIFO. In the main loop, read whatever is available without blocking, feed the parser, keep the latest state. Core 1 never touches the UART.
4. **Activity input:** in `io` (or a small new module), when the link is present, `io::activity()` uses the link's busy flag through the existing fast-attack, slow-release logic instead of the optocoupler; add `io::activityClass()` (Idle, Sequential, Random; Random when the link isn't present, so the optocoupler path behaves exactly as now). Simulation mode keeps working: make it produce sequential spells too (for example, alternate random bursts with longer sequential ones), so packs can be auditioned without HDDSynthPro.
5. **`sequential.wav`, optional per pack:**
   - a new streamed, looping voice, `VOICE_SEQUENTIAL`, handled like `VOICE_ACCESS` (its own ring and `sd::Stream`, the same format and sample-rate checks, `checkPackRate()` including it);
   - packs without it work as before: the sequential class then plays `access.wav`;
   - in `runStep()`, while busy, play `sequential.wav` at the access level when the class is Sequential and the pack has it, otherwise `access.wav`; the other one at level 0. The existing 8 ms level fade covers the switch;
   - move its loop to a random point after a burst, as `jumpAccess()` does for the access loop;
   - check the RAM budget: one more ring of `RING_SAMPLES` (32 KB); `HEAP_RESERVE_BYTES` must still hold.
6. **Console and OLED:** add the link state (present or lost, last class, valid and rejected sentence counts) to the stats/`h` output.
7. **Docs:**
   - README: the setting, a section "Activity link (HDDSynthPro)" with the J6 wiring above, and `sequential.wav` in the sample pack file list (optional);
   - note for the HDDSynth_Samples repo: `sequential.wav` is optional; same format and sample rate as the pack's other files; a seamless loop, like `access.wav`.
8. **Version:** a MINOR bump, a CHANGELOG entry, and a `vX.Y.Z` tag, as CLAUDE.md describes.

## Tests

- Host tests for the parser:
  - the known answers above;
  - a bad checksum, a missing `*`, lower-case hex (accepted), a version that isn't a number, unknown sentence names;
  - lines split across feeds, two sentences in one feed, a line over 80 bytes followed by a good one, `\n` without `\r`;
  - extra trailing fields accepted.
- Host tests for `LinkWatch` (present, still present at 999 ms, lost at 1000 ms, back again) and for the sample choice (class × pack has `sequential.wav` × busy → which voice is at the access level).
- Run the host tests under ASan/UBSan as usual. On hardware:
  - with `ACTIVITY_LINK` on and nothing attached, the synth behaves exactly as now (optocoupler);
  - with a USB-serial adapter on J6 pin 3, typing or pasting the example sentences switches the sound;
  - with HDDSynthPro attached, a large file copy plays `sequential.wav`, and random access plays `access.wav`.

## Not in scope

Reply messages from the synth (J6 pin 4 stays unused), read/write-specific samples, and driving spin-up or spin-down from the link. The protocol leaves room for them.
