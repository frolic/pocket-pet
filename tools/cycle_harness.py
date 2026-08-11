"""Power-cycle repro harness: reset via RTS, capture serial, classify each boot.

Each cycle: pulse reset, log 75s to cycle-N.log, then print a one-line verdict:
CLEAN (reached ACTIVE after sync), WATCHDOG (stuck-radio watchdog fired),
STUCK (never left sync states), CRASH (backtrace/reset loop detected).
"""
import re
import sys
import time

import serial

PORT = "/dev/cu.usbmodem2101"
CYCLES = int(sys.argv[1]) if len(sys.argv) > 1 else 8
CAPTURE_S = 75
LOG_DIR = sys.argv[2] if len(sys.argv) > 2 else "."

for cycle in range(1, CYCLES + 1):
    path = f"{LOG_DIR}/cycle-{cycle}.log"
    port = serial.Serial(PORT, 115200, timeout=1)
    port.dtr = False
    port.rts = True
    time.sleep(0.1)
    port.rts = False
    start = time.time()
    lines = []
    with open(path, "w") as log:
        while time.time() - start < CAPTURE_S:
            raw = port.readline().decode(errors="replace")
            if not raw:
                continue
            log.write(f"[{time.time()-start:7.2f}] {raw}")
            log.flush()
            lines.append(raw)
    port.close()

    text = "".join(lines)
    flushfails = re.findall(r"flushfail=(\d+)", text)
    max_ff = max((int(f) for f in flushfails), default=0)
    boots = text.count("BOOT reset_reason")
    rtc_boot = "skipping boot sync" in text
    if "Backtrace" in text or boots > 1:
        verdict = "CRASH"
    elif "WATCHDOG" in text:
        verdict = "WATCHDOG"
    elif "SYNC_VISIBLE -> ACTIVE" in text or "SYNCING -> DOZING" in text:
        verdict = "CLEAN"
    elif rtc_boot and "ACTIVE -> DOZING" in text:
        verdict = "CLEAN"  # RTC-restored boot: no sync phase exists
    else:
        verdict = "STUCK"
    synced = "SNTP time sync complete" in text
    print(f"cycle {cycle}: {verdict} flushfail_max={max_ff} sntp={synced}"
          f" rtc={rtc_boot}", flush=True)

print("harness done", flush=True)
