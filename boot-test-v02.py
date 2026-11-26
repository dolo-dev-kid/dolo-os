#!/usr/bin/env python3
"""Boot DoloOS v0.2 in QEMU and drive the shell with real keystrokes.

Uses the QEMU human monitor (unix socket) to sendkey commands, then
checks the serial log for the banner, the prompt, and each command's
output. Also saves a VGA screendump (screen.ppm + screen.png).
"""
import os
import socket
import subprocess
import sys
import time

D = "/home/hatch/workspace/dolo-os"
MON = "/tmp/doloos-mon.sock"
LOG = f"{D}/boot.log"

for p in (MON, LOG, f"{D}/screen.ppm", f"{D}/screen.png"):
    try:
        os.unlink(p)
    except FileNotFoundError:
        pass

qemu = subprocess.Popen([
    "qemu-system-x86_64",
    "-cdrom", f"{D}/doloos.iso",
    "-serial", f"file:{LOG}",
    "-display", "none",
    "-monitor", f"unix:{MON},server,nowait",
    "-m", "128",
], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

# Wait for the kernel to boot (TCG is slow; the kernel itself is instant).
time.sleep(8)


def mon_connect():
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(MON)
    try:
        s.recv(4096)  # greeting banner, ignore
    except socket.timeout:
        pass
    return s


def mon_send(s, cmd):
    s.sendall(cmd.encode() + b"\n")
    time.sleep(0.4)
    try:
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
    except socket.timeout:
        pass


def sendkey(s, keys, pause=1.5):
    print(f"  sendkey {keys}")
    mon_send(s, f"sendkey {keys}")
    time.sleep(pause)


s = mon_connect()

# 1. help
sendkey(s, "h-e-l-p-ret", 2)
# 2. about
sendkey(s, "a-b-o-u-t-ret", 2)
# 3. ver
sendkey(s, "v-e-r-ret", 1)
# 4. mem
sendkey(s, "m-e-m-ret", 1)
# 5. clear
sendkey(s, "c-l-e-a-r-ret", 1)
# 6. backspace test: type "helx", erase the x, type "p" -> runs "help"
sendkey(s, "h-e-l-x-backspace-p-ret", 2)
# 7. unknown command
sendkey(s, "x-y-z-z-y-ret", 1)

# Screendump, then quit.
screendump_ok = False
try:
    mon_send(s, f"screendump {D}/screen.ppm")
    screendump_ok = os.path.exists(f"{D}/screen.ppm")
    mon_send(s, "quit")
except Exception as e:
    print("monitor error:", e)
s.close()

try:
    qemu.wait(timeout=10)
except subprocess.TimeoutExpired:
    qemu.kill()
    qemu.wait()

# Convert the screendump to PNG for easy viewing.
png_ok = False
if screendump_ok:
    try:
        from PIL import Image
        Image.open(f"{D}/screen.ppm").save(f"{D}/screen.png")
        png_ok = os.path.exists(f"{D}/screen.png")
    except Exception as e:
        print("png convert error:", e)

log = ""
try:
    log = open(LOG).read()
except FileNotFoundError:
    pass

print("qemu exit:", qemu.returncode)
print("screendump:", "OK" if screendump_ok else "FAILED",
      "| png:", "OK" if png_ok else "FAILED")

checks = [
    ("banner version", "DoloOS v0.2" in log),
    ("banner tagline", "Kernel up. v0.2 - it boots and types." in log),
    ("memory self-test", "self-test OK" in log and "memory: " in log
     and " pages, self-test OK" in log),
    ("help hint", "Type 'help' and press Enter." in log),
    ("shell prompt", "dolo> " in log),
    ("help output", "Commands:" in log),
    ("help lists clear", "clear - clear the screen" in log),
    ("about output", "Wesley Robinson (Dolo45)" in log),
    ("ver output", "DoloOS v0.2 - kernel 0.2, shell 1.0" in log),
    ("mem output", "pages free (4 KB each)" in log
     and "Memory: " in log and " of " in log),
    ("clear ran", "[screen cleared]" in log),
    ("unknown command", "unknown command: xyzzy" in log),
    ("help ran twice (backspace test)", log.count("Commands:") >= 2),
]
failed = 0
for name, ok in checks:
    print(("PASS" if ok else "FAIL"), "-", name)
    if not ok:
        failed += 1

print("--- boot.log ---")
print(log, end="")
sys.exit(1 if failed else 0)
