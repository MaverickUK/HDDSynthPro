"""Slice-4-like writes and read-backs on the adapter's raw disk; on the first
write that ends busy (DONE 30 80) send console R and s at once and save it all."""
import os, random, re, sys, threading, time
import serial

DEV = sys.argv[1] if len(sys.argv) > 1 else "/dev/rdisk4"
OUT = sys.argv[2]
PORT = "/dev/cu.usbmodem1101"
SECTOR = 512

ser = serial.Serial(PORT, 115200, timeout=0.2)
ser.reset_input_buffer()
log = open(OUT, "w")
caught = threading.Event()
stop = threading.Event()


def reader():
    buf = ""
    sent_at = None
    while not stop.is_set():
        try:
            chunk = ser.read(4096).decode(errors="replace")
        except Exception as e:
            log.write(f"# serial error {e}\n"); break
        if not chunk:
            if sent_at and time.time() - sent_at > 8:
                break
            continue
        log.write(chunk); log.flush()
        buf = (buf + chunk)[-400:]
        if not caught.is_set() and re.search(r"DONE \d+ 3[0-1] 80 ", buf):
            caught.set()
            ser.write(b"R"); time.sleep(3); ser.write(b"s")
            sent_at = time.time()
            print("caught: R and s sent", flush=True)


t = threading.Thread(target=reader, daemon=True)
t.start()
ser.write(b"m")  # a MARK at the start

fd = os.open(DEV, os.O_RDWR)
rng = random.Random(int(time.time()))
cycles = 0
try:
    while not caught.is_set() and cycles < 3000:
        lba = 100000 + rng.randrange(0, 256)
        n = rng.randint(1, 32)
        data = rng.randbytes(n * SECTOR)
        os.pwrite(fd, data, lba * SECTOR)
        back = os.pread(fd, n * SECTOR, lba * SECTOR)
        if back != data:
            print(f"mismatch cycle {cycles} lba {lba} n {n}", flush=True)
            ser.write(b"R")
            break
        cycles += 1
        if cycles % 100 == 0:
            print(f"{cycles} cycles", flush=True)
except OSError as e:
    print(f"OSError after {cycles} cycles: {e}", flush=True)
print(f"done: {cycles} cycles, caught={caught.is_set()}", flush=True)
caught.wait(5)
time.sleep(6)
stop.set(); t.join(10)
log.close()
