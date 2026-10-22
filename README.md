# HDDSynthPro

An RP2350 that pretends to be an IDE hard disk, as the base for a future HDD Synth that *is* the drive rather than one that listens to it.

At this stage it is a proof of concept on an Olimex RP2350-PICO2-BB48, tested through a USB-IDE adapter from the same Mac that builds it. See [docs/plan.md](docs/plan.md) for the slices and [docs/wiring.md](docs/wiring.md) for the connections.

## Status

Slice 0 (listen only): the firmware records every IDE register access and reset it sees, without driving any pin, and streams the trace over USB serial.

## Quick start

```sh
test/run_native_tests.sh                 # C++ unit tests (no hardware)
cd host && python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
.venv/bin/pytest -m sim                  # simulator and parser tests (no hardware)

pio run -t upload                        # build and flash the BB48
pio device monitor                       # h help, p pin levels, s stats

scripts/rig.sh -m slice0                 # everything, against the real adapter
```

Read the power-order rule in [docs/wiring.md](docs/wiring.md) before connecting the adapter.

## Licence

To be decided (HDDSynth_Native is GPL-3.0).
