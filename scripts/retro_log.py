"""The console for a retro PC test (docs/retro-test.md): records every line the
drive prints to host/runs/retro-<date>-<time>.log, and shows what matters:
resets, commands other than reads and writes, errors (DONE with ERR set), and
NOIORDY / BUSERR / STUCK / SLOWREAD / CARD lines, marked "!!". Every minute
(and when you press Enter on an empty line) it prints a summary from `s`.

Type console commands and Enter: `s`, `S`, `h`, `p`, `!` as usual, and
`E key = value` to change a setting. `q` quits (after a last summary).

Usage: python3 scripts/retro_log.py [note for the log's name, e.g. 486-pass1]
Runs on macOS or Windows with host/requirements.txt installed (pyserial); HDDPRO_PORT picks the port if it isn't found."""
import os
import re
import sys
import threading
import time

HOST = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "host")
sys.path.insert(0, HOST)
from hddproto.drive_log import DriveLog  # noqa: E402

DATA_COMMANDS = {"READ_SECTORS", "READ_SECTORS_NR", "WRITE_SECTORS", "WRITE_SECTORS_NR", "READ_MULTIPLE",
                 "READ_DMA", "READ_DMA_NR", "WRITE_DMA", "WRITE_DMA_NR", "READ_VERIFY", "READ_VERIFY_NR"}
ALERTS = ("NOIORDY", "BUSERR", "STUCK", "SLOWREAD", "CARD", "ABSENT")
SUMMARY_FIELDS = ("iordy_ignored", "iordy_max_ns", "iordy_max_reg", "bus_sectors", "bus_bad", "bus_gaps",
                  "bus_identify", "bus_identify_bad", "card", "image_name")

note = sys.argv[1] if len(sys.argv) > 1 else "retro"
runs = os.path.join(HOST, "runs")
os.makedirs(runs, exist_ok=True)
path = os.path.abspath(os.path.join(runs, f"retro-{time.strftime('%Y%m%d-%H%M%S')}-{note}.log"))
d = DriveLog(raw_log_path=path)
d.open(timeout=30)
print(f"logging to {path}")
print(d.command("h", "READY", timeout=5))

data_counts = {}  # data commands since the last summary, by name
counts_lock = threading.Lock()
stop = threading.Event()


def stamp():
    return time.strftime("%H:%M:%S")


def show(line):
    if line.startswith(ALERTS):
        print(f"{stamp()} !! {line}")
    elif line.startswith("CMD"):
        parts = line.split()
        name = parts[3] if len(parts) > 3 else "?"
        if name in DATA_COMMANDS:
            with counts_lock:
                data_counts[name] = data_counts.get(name, 0) + 1
        else:
            print(f"{stamp()}    {line}")
    elif line.startswith("DONE"):
        parts = line.split()
        if len(parts) > 4 and int(parts[3], 16) & 0x01:  # ERR in the status
            print(f"{stamp()} !! {line}   (an error: status {parts[3]}, error {parts[4]})")
    elif line.startswith(("RESET", "CONFIG", "SD ", "READY", "#", "LOOPBACK", "MARK")):
        print(f"{stamp()}    {line}")
    elif line.startswith("STATS") and not summarising.is_set():
        print(f"{stamp()}    {line}")


summarising = threading.Event()


def summary():
    summarising.set()
    try:
        fields = dict(re.findall(r"(\w+)=(\S+)", d.command("s", "STATS", timeout=5)))
    except TimeoutError:
        print(f"{stamp()} !! no answer to `s`")
        return
    finally:
        time.sleep(0.1)
        summarising.clear()
    with counts_lock:
        cmds = ", ".join(f"{k} {v}" for k, v in sorted(data_counts.items())) or "no reads or writes"
        data_counts.clear()
    flag = " !!" if fields.get("iordy_ignored", "0") != "0" or fields.get("bus_bad", "0") != "0" else ""
    print(f"{stamp()} --{flag} " + " ".join(f"{k}={fields.get(k, '?')}" for k in SUMMARY_FIELDS))
    print(f"{stamp()} --  since the last summary: {cmds}")


def follow():
    seen = d.mark()
    while not stop.is_set():
        new = d.lines_since(seen)
        seen += len(new)
        for line in new:
            show(line)
        time.sleep(0.05)


def every_minute():
    while not stop.wait(60):
        summary()


threading.Thread(target=follow, daemon=True).start()
threading.Thread(target=every_minute, daemon=True).start()
d.send("S")
time.sleep(2)
summary()
try:
    for typed in sys.stdin:
        typed = typed.strip()
        if typed == "q":
            break
        if not typed:
            summary()
        elif typed.startswith("E ") or typed.startswith("E\t"):
            d.send("E")
            time.sleep(0.3)
            d.send(typed[1:].strip() + "\n")
        else:
            d.send(typed)
except KeyboardInterrupt:
    pass
summary()
stop.set()
d.close()
print(f"log: {path}")
