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
# bb48_drive for slice 1 onwards. Tests that need the other build skip themselves.
# Rig settings (hands-free replugs etc.) come from host/rig.env if it exists:
# see host/rig.env.example and host/hddproto/host.py.
#
# On Windows, run it from Git Bash in an administrator terminal (raw disk access).
set -euo pipefail
cd "$(dirname "$0")/.."
[[ -f host/rig.env ]] && source host/rig.env

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) PYTHON="${PYTHON:-python}"; VENV_BIN=Scripts ;;  # Windows
    *)                    PYTHON="${PYTHON:-python3}"; VENV_BIN=bin ;;
esac
export PYTHONUTF8=1  # logs as UTF-8 on Windows too
PIO="${PIO:-$(command -v pio || ls "$HOME"/.platformio-venv/bin/pio "$HOME"/.platformio/penv/Scripts/pio.exe 2>/dev/null | head -1 || true)}"
VENV=host/.venv

if [[ ! -x "$VENV/$VENV_BIN/pytest" ]]; then
    "$PYTHON" -m venv "$VENV"
    "$VENV/$VENV_BIN/pip" install -q -r host/requirements.txt
fi

echo "== Host unit tests"
if command -v "${CXX:-c++}" > /dev/null; then
    test/run_native_tests.sh > .pio/native-tests.log 2>&1 || { cat .pio/native-tests.log; exit 1; }
    tail -1 .pio/native-tests.log
else
    echo "skipped: no ${CXX:-c++} (CI runs them; on Windows, WSL can)"
fi

if [[ -z "${NO_FLASH:-}" ]]; then
    echo "== Build and flash"
    "$PIO" run -e "${PIO_ENV:-bb48}"
    if [[ "${PIO_ENV:-bb48}" == bb48_drive ]]; then
        "$PYTHON" scripts/check_core1_ram.py  # nothing core 1 calls may be in flash
    fi
    "$PIO" run -e "${PIO_ENV:-bb48}" -t upload
fi

echo "== Rig tests"
cd host
exec ".venv/$VENV_BIN/pytest" -s "$@"
