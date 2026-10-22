from pathlib import Path

HOST = Path(__file__).resolve().parent.parent
REPO = HOST.parent
RUNS = HOST / "runs"            # one timestamped folder per test run (git-ignored)
REFERENCE = HOST / "reference"  # saved reference traces (committed)
