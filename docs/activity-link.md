# Activity link to the HDD Synth

HDDSynthPro knows exactly what the emulated drive is doing; the HDD Synth only sees an activity LED. This link sends the drive's activity to an HDD Synth over a one-way UART, so the synth can play a different sound for sustained sequential transfers than for random access.

Status: **the HDDSynthPro side is built (0.9.0)** and sends on the rig, with a loopback test (0.10.1): fit a jumper from GPIO 0 to GPIO 1 (EXT1 pins 4 and 5), then console `U`, or `pytest -m link`. Not yet run with the jumper; with the UART's internal loopback it passes. Still to do: the HDD Synth side, briefed in [hddsynth-activity-link-brief.md](hddsynth-activity-link-brief.md); its receiver can copy `activity_link::Receiver`.

As built, it differs from the plan below in two places:
- **UART0 is driven through the Pico SDK, transmit only**, not Arduino's `Serial1`. `Serial1.availableForWrite()` only says whether one byte fits and `write()` waits when the FIFO is full, so it couldn't keep the rule that core 0 never waits; it also enables a receive interrupt, which a floating GPIO 1 could trigger. Now GPIO 1 isn't touched, and a sentence is written only when the 32-byte TX FIFO is empty, so it always fits.
- **The last operation is tracked next to the link** (`activity_link::opOf()` on each media command), not in `activity::Classifier`, which stays as it was.

## What is sent

Three things, all of which the drive already knows:

| Field | Values | Source |
|---|---|---|
| Busy | 0 or 1 | The activity LED's own rule: a command is running, or one started in the last 25 ms (`cfg::LED_MIN_MS`). Instant, so the synth can start its access sound at once, as it does from the LED today. |
| Class | `I` idle, `S` sequential, `R` random | The activity classifier (`src/activity.*`): the recent mix of sequential commands and seeks. It decides within about 100-250 ms (`activity::Config`). |
| Operation | `R` read, `W` write, `-` none | The last media command. Not used by the synth yet; sent so a later version can play different read and write sounds without a protocol change. |

The synth plays its access sound while busy, as today, and chooses the sample by class: `sequential.wav` while sequential, `access.wav` otherwise.

## Wiring

Ground and one data wire. **Never connect the two boards' 3.3 V pins:** each is powered on its own.

| HDDSynthPro (BB48) | | HDD Synth MK5 (header J6) |
|---|---|---|
| GPIO 0 (UART0 TX), EXT1 pin 4 | 1 kΩ in series | pin 3, GP25 (UART1 RX; the OLED's SCL) |
| GND, EXT1 pin 2 | | pin 2, GND |
| GPIO 1 (UART0 RX), EXT1 pin 5 | reserved, not connected | pin 4, GP24 (UART1 TX; the OLED's SDA) |
| (not connected) | | pin 1, 3.3 V |

- The 1 kΩ resistor limits the current into the synth's pin if one board is powered and the other isn't (the HDD Synth runs on its supercap after a power cut, and an unpowered RP2350's pin would otherwise be fed through its protection diodes). It doesn't matter at 115200 baud.
- J6 carries either the OLED or the link, not both: the same pins.
- GPIO 0 and 1 are off the IDE bus and away from the BB48's pull-ups on GPIO 2/3 (see [wiring.md](wiring.md)).

## Protocol, version 1

- UART, **115200 baud, 8N1**, 3.3 V logic, one way (HDDSynthPro → synth).
- ASCII lines in the style of NMEA, so a USB-serial adapter and a terminal show them as they are:

```
$HDA,1,<busy>,<class>,<op>*<checksum>\r\n
```

| Part | Meaning |
|---|---|
| `$HDA` | Sentence name: HDD activity |
| `1` | Protocol version |
| `<busy>` | `0` or `1` |
| `<class>` | `I`, `S` or `R` |
| `<op>` | `R`, `W` or `-` |
| `*<checksum>` | Two upper-case hex digits: the XOR of every byte between `$` and `*`, as in NMEA |

Examples, with their real checksums (use them as known answers in tests):

```
$HDA,1,0,I,-*28      idle
$HDA,1,1,S,R*4C      busy, sequential reads
$HDA,1,1,R,W*48      busy, random writes
$HDA,1,1,S,W*49      busy, sequential writes
$HDA,1,0,R,R*4C      not busy this instant, recent mix still random
```

Rules:
- A sentence is sent **on every change** of any field, and **every 200 ms** regardless, as a heartbeat.
- A receiver ignores any line that doesn't start with `$HDA`, fails its checksum or is longer than 80 bytes, and ignores extra fields after the ones it knows. A later version adds fields at the end and keeps these.
- A receiver treats the link as **present** after one valid sentence, and **lost** after 1 s without one. When lost it falls back to its own activity input.
- Bandwidth: a sentence is 20 bytes (about 1.7 ms on the wire). Even random 4 KB reads at ~76 commands a second change busy far less often than that, because busy holds for 25 ms; the link stays well under its 11.5 KB/s.

## HDDSynthPro side: the plan

All on core 0. Core 1 and the bus timing are untouched.

1. **Pins and docs.** `cfg::PIN_LINK_TX = 0` (and `PIN_LINK_RX = 1`, reserved) in `src/config.h`; a section in `docs/wiring.md` and the wiring diagram; a note in CLAUDE.md's pin rules.
2. **Last operation in the classifier.** `activity::Classifier` keeps whether the last media command read or wrote (from the command byte it already sees). Host-tested.
3. **`src/activity_link.*`**, in two parts:
   - pure C++, host-tested: `formatSentence(busy, state, op, buf)` with the checksum, and `LinkSender`, which decides when to send (on change, or 200 ms since the last);
   - the hardware part: `Serial1` (UART0) on GPIO 0 at 115200. It writes only when `Serial1.availableForWrite()` has room for the whole sentence, so it never blocks core 0 (which must keep serving fetches). A sentence that can't be sent is replaced by the next.
4. **`main.cpp`**: `loop()` feeds the link from the same busy flag as the LED and the classifier's state, once a millisecond with the activity update.
5. **Console:** `s` shows `link_sent=` and the last sentence; `L` turns the link on or off (on by default).
6. **Tests:**
   - C++: sentence format and checksum (known answers shared with the Python parser), the send rules (change, heartbeat, a full UART);
   - Python: `host/hddproto/activity_link.py`, a parser that is also the reference for the synth's, with `sim` tests;
   - on the rig, a loopback test: a jumper from GPIO 0 to GPIO 1, and console `U` reads back what was sent. No HDD Synth needed;
   - with the synth: run the activity rig test (`-m activity`) and watch the synth switch samples.
7. Version bump, CHANGELOG, and a section in [activity.md](activity.md).

## Later

- More sounds from the same link: separate read and write samples (the `op` field), seek sizes, spin-down and spin-up from the drive's STANDBY and wake.
- A reply from the synth on GPIO 1 (link up, which pack, volume), if it turns out to be useful.
