#!/usr/bin/env python3
"""Flash the app firmware and report whether it got onto Wi-Fi.

    ~/.platformio/penv/bin/python tools/flashapp.py

Waits for a board, uploads `app-c3`, then resets it and reads the boot log so
you find out in one go whether the network it was built with actually works.

Why the reset: the firmware waits up to 1.5 s for a USB-CDC host and then
prints its banner, so attaching after an upload reliably misses everything and
leaves you staring at a silent port. Resetting the chip while we hold the link
is the only way to see boot from the start.

Nothing else may hold the port -- the ESP32-C3's USB-serial-JTAG allows one
owner, and a `pio device monitor` in another window is the usual culprit.
"""
import glob
import os
import subprocess
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is missing. Use PlatformIO's interpreter, which has it:\n"
             "  ~/.platformio/penv/bin/python tools/flashapp.py")

HOME = os.path.expanduser("~")
PIO = os.path.join(HOME, ".platformio/penv/bin/pio")
ESPTOOL = os.path.join(HOME, ".platformio/packages/tool-esptoolpy/esptool.py")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BAUD = 115200
ENV = "app-c3"

# How long to listen after the reset. A join is usually done inside five
# seconds; the 12 s join timeout plus a retry needs more room than that.
LISTEN_S = 40


def say(msg=""):
    print(msg, flush=True)


def find_port(timeout=None):
    """Block until a board appears. Returns the device path, or None."""
    said = False
    end = None if timeout is None else time.time() + timeout
    while end is None or time.time() < end:
        found = sorted(glob.glob("/dev/cu.usbmodem*"))
        if found:
            return found[0]
        if not said and timeout is None:
            say("  no /dev/cu.usbmodem* yet - plug the board in (Ctrl-C to give up)")
            said = True
        time.sleep(0.3)
    return None


def upload(port):
    say("\n  uploading %s to %s" % (ENV, port))
    r = subprocess.run([PIO, "run", "-e", ENV, "-t", "upload", "--upload-port", port],
                       cwd=os.path.join(ROOT, "firmware"))
    return r.returncode == 0


def reset(port):
    """Hard-reset over USB-JTAG. The port vanishes and comes back."""
    subprocess.run([sys.executable, ESPTOOL, "--chip", "esp32c3", "--port", port,
                    "--after", "hard_reset", "read_mac"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def listen(port, seconds):
    """Stream the log, and stop early once the network question is settled.

    Returns (verdict, lines) where verdict is one of 'up', 'ap', 'failed' or
    'quiet'."""
    deadline = time.time() + seconds
    lines, buf, verdict = [], "", "quiet"
    s = None
    while time.time() < deadline:
        if s is None:
            try:
                s = serial.Serial(port, BAUD, timeout=0.2)
                s.dtr, s.rts = True, False
            except (OSError, serial.SerialException) as err:
                if "Resource busy" in str(err):
                    say("  port is busy - close any pio device monitor")
                time.sleep(0.2)
                continue
        try:
            chunk = s.read(512)
        except (OSError, serial.SerialException):
            s = None
            continue
        if not chunk:
            continue
        buf += chunk.decode("utf-8", "replace")
        parts = buf.split("\n")
        buf = parts.pop()
        for line in parts:
            line = line.rstrip("\r")
            if not line:
                continue
            lines.append(line)
            say("    | " + line)
            if line.startswith("[net] up:"):
                verdict = "up"
                deadline = min(deadline, time.time() + 2)   # let mDNS print too
            elif "setup AP" in line:
                verdict = "ap"
            elif "join failed" in line and verdict == "quiet":
                verdict = "failed"
    if s is not None:
        try:
            s.close()
        except Exception:
            pass
    return verdict, lines


def main():
    say("EasyPill app flash. Waiting for a board.")
    port = sys.argv[1] if len(sys.argv) > 1 else find_port()

    if not upload(port):
        sys.exit("\n  upload failed - nothing was changed on the board")

    # The upload reboots the board, so the banner is already gone. Reset it
    # again with the link in hand.
    port = find_port(20) or port
    say("\n  resetting to catch the boot log")
    reset(port)
    port = find_port(20) or port
    verdict, lines = listen(port, LISTEN_S)

    say("\n" + "-" * 68)
    net = [l for l in lines if l.startswith("[net]")]
    if verdict == "up":
        say("  On the network.")
        for l in net:
            say("    " + l)
        host = "easypill"
        for l in lines:
            if "mDNS up:" in l:
                host = l.split("http://")[-1].strip()
        say("\n  GUI: http://%s" % host)
    elif verdict == "ap":
        say("  It did NOT join, and fell back to the setup AP.")
        say("  Either the credentials in firmware/include/wifi_default.h are wrong,")
        say("  or the router did not answer. The joins it tried:")
        for l in net:
            say("    " + l)
    elif verdict == "failed":
        say("  Join failed at least once and it is still retrying:")
        for l in net:
            say("    " + l)
    else:
        say("  Nothing came out of the port in %d s." % LISTEN_S)
        say("  Something else may be holding it, or the reset did not take.")
        say("  Try again, or run: ~/.platformio/penv/bin/pio device monitor -e %s" % ENV)


if __name__ == "__main__":
    main()
