#!/usr/bin/env bash
# One command for the rig: host unit tests, build, flash, then the pytest
# suite against the real drive. Extra arguments go to pytest.
#
#   scripts/rig.sh -m slice0                    # slice 0 only
#   scripts/rig.sh -m "slice0 or slice1"        # a slice plus its predecessors
#   scripts/rig.sh -m slice0 --save-reference   # keep this trace as the reference
#   NO_FLASH=1 scripts/rig.sh -m slice0         # test the firmware already on the board
#   PIO_ENV=bb48_drive scripts/rig.sh -m "slice1 or slice2 or slice3"   # the drive build
#
# PIO_ENV picks the build: bb48 (default, listen only) for slice 0,
# bb48_drive for slices 1-3. Tests that need the other build skip themselves.
# Set HDDPRO_UHUBCTL (see host/hddproto/host_macos.py) for hands-free replugs.
set -euo pipefail
cd "$(dirname "$0")/.."

PIO="${PIO:-$(command -v pio || echo "$HOME/.platformio-venv/bin/pio")}"
VENV=host/.venv

if [[ ! -x "$VENV/bin/pytest" ]]; then
    python3 -m venv "$VENV"
    "$VENV/bin/pip" install -q -r host/requirements.txt
fi

echo "== Host unit tests"
test/run_native_tests.sh > .pio/native-tests.log 2>&1 || { cat .pio/native-tests.log; exit 1; }
tail -1 .pio/native-tests.log

if [[ -z "${NO_FLASH:-}" ]]; then
    echo "== Build and flash"
    "$PIO" run -e "${PIO_ENV:-bb48}" -t upload
fi

echo "== Rig tests"
cd host
exec .venv/bin/pytest -s "$@"
