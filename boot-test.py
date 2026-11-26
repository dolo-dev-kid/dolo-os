#!/usr/bin/env python3
"""Boot DoloOS in QEMU, capture the serial log and a VGA screendump."""
import os
import socket
import subprocess
import sys
import time

D = "/home/hatch/workspace/dolo-os"
MON = "/tmp/doloos-mon.sock"
for p in (MON, f"{D}/boot.log", f"{D}/screen.ppm"):
    try:
        os.unlink(p)
    except FileNotFoundError:
        pass

qemu = subprocess.Popen([
    "qemu-system-x86_64",
    "-cdrom", f"{D}/doloos.iso",
    "-serial", f"file:{D}/boot.log",
    "-display", "none",
    "-monitor", f"unix:{MON},server,nowait",
    "-m", "128",
], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

# Wait for the kernel to boot (TCG is slow; the kernel itself is instant).
time.sleep(8)

# Talk to the QEMU human monitor over the unix socket.
screendump_ok = False
try:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(MON)
    s.recv(4096)  # banner
    s.sendall(b"screendump " + f"{D}/screen.ppm".encode() + b"\n")
    time.sleep(1)
    s.sendall(b"quit\n")
    s.close()
    screendump_ok = os.path.exists(f"{D}/screen.ppm")
except Exception as e:
    print("monitor error:", e)

try:
    qemu.wait(timeout=10)
except subprocess.TimeoutExpired:
    qemu.kill()
    qemu.wait()

print("qemu exit:", qemu.returncode)
print("screendump:", "OK" if screendump_ok else "FAILED")
print("--- boot.log ---")
try:
    print(open(f"{D}/boot.log").read(), end="")
except FileNotFoundError:
    print("(no boot.log — serial produced nothing)")
