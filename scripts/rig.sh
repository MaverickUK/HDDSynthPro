#!/usr/bin/env bash
# One command for the rig: host unit tests, build, flash, then the pytest
# suite against the real drive. Extra arguments go to pytest.
#
#   scripts/rig.sh -m slice0                    # slice 0 only
#   scripts/rig.sh -m "slice0 or slice1"        # a slice plus its predecessors
#   scripts/rig.sh -m slice0 --save-reference   # keep this trace as the reference
#   NO_FLASH=1 scripts/rig.sh -m slice0         # test the firmware already on the board
#
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
    "$PIO" run -t upload
fi

echo "== Rig tests"
cd host
exec .venv/bin/pytest -s "$@"
