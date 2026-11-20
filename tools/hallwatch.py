#!/usr/bin/env python3
"""Say which hall sensor just saw a magnet.

Wave a magnet over one sensor at a time; this prints the sensor number, the lid
the firmware currently believes it belongs to, and keeps a checklist so you can
tell at the end which of the seven you have actually proved.

    ~/.platformio/penv/bin/python tools/hallwatch.py

It drives the bring-up firmware's `H` live table over USB-CDC and parses it, so
flash `bringup-c3` first. Nothing else may hold the port: the ESP32-C3's native
USB-serial-JTAG allows exactly one owner, and a `pio device monitor` left open
in another window is the usual reason a session "disconnects a few times".
This script survives that anyway -- it reconnects and re-arms by itself, and
says so, rather than dying and losing your tally.

Ctrl-C to stop and print the summary.
"""
import glob
import re
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is missing. Run this with PlatformIO's interpreter, which has it:\n"
             "  ~/.platformio/penv/bin/python tools/hallwatch.py")

BAUD = 115200
LIDS = 7

# A data row of the `H` table carries seven [x] groups: [1] = no magnet,
# [.] = magnet. Anchor on all seven together so a wrapped or half-written line
# is ignored rather than half-parsed.
ROW = re.compile(r"\[([1.])\]\[([1.])\]\[([1.])\]\[([1.])\]\[([1.])\]\[([1.])\]\[([1.])\]")
# The same command prints the mapping it is using, e.g. " s0=Monday, s1=Tuesday".
MAP = re.compile(r"s(\d)=([A-Za-z]+)")


def find_port(argv):
    """Block until a board shows up, rather than exiting. The board gets
    unplugged constantly during rework; starting the watcher first and then
    plugging in should just work."""
    if len(argv) > 1:
        return argv[1]
    said = False
    while True:
        ports = sorted(glob.glob("/dev/cu.usbmodem*"))
        if ports:
            return ports[0]
        if not said:
            print("  no /dev/cu.usbmodem* yet - waiting for the board (Ctrl-C to give up)",
                  flush=True)
            said = True
        time.sleep(0.5)


def connect(port):
    """Block until the board is there and talking. Returns an open port."""
    announced = False
    while True:
        try:
            s = serial.Serial(port, BAUD, timeout=0.2)
            s.dtr, s.rts = True, False
            time.sleep(1.2)            # CDC only starts talking once a host attaches
            # Any key stops a running live table, so if one is already going -
            # left over from an earlier session or a monitor someone closed -
            # an `H` would STOP it instead of starting one, and we would sit
            # here parsing silence. Knock first, then arm.
            s.write(b"\r\n")
            s.flush()
            time.sleep(0.6)
            s.reset_input_buffer()
            s.write(b"H\r\n")          # arm the live table
            s.flush()
            return s
        except (OSError, serial.SerialException) as err:
            if not announced:
                reason = "busy - close any pio device monitor" if "Resource busy" in str(err) \
                         else str(err).strip()
                print("  waiting for %s (%s)" % (port, reason), flush=True)
                announced = True
            time.sleep(0.5)


def main():
    port = find_port(sys.argv)
    print("EasyPill hall watch on %s. Wave a magnet over one sensor at a time." % port,
          flush=True)
    print("Ctrl-C to stop.\n", flush=True)

    names = {}                         # sensor -> lid name, as the firmware maps it
    seen = set()                       # sensors that have shown a magnet at least once
    counts = [0] * LIDS
    state = [None] * LIDS              # True = magnet present
    s = connect(port)
    buf = ""

    try:
        while True:
            try:
                chunk = s.read(256)
            except (OSError, serial.SerialException):
                chunk = b""
                print("  ... link dropped, reconnecting", flush=True)
                try:
                    s.close()
                except Exception:
                    pass
                s = connect(port)
                state = [None] * LIDS  # re-baseline; we missed whatever happened meanwhile
                print("  ... back", flush=True)
                continue

            if not chunk:
                continue
            buf += chunk.decode("utf-8", "replace")

            # Keep the tail: a row may still be arriving.
            lines = buf.split("\n")
            buf = lines.pop()

            for line in lines:
                for sensor, name in MAP.findall(line):
                    names[int(sensor)] = name

                m = ROW.search(line)
                if not m:
                    continue
                for i in range(LIDS):
                    magnet = m.group(i + 1) == "."
                    if state[i] is None:
                        state[i] = magnet       # first sample is the baseline, not an event
                        continue
                    if magnet == state[i]:
                        continue
                    state[i] = magnet
                    if magnet:
                        counts[i] += 1
                        seen.add(i)
                        print("  MAGNET   hall %d   (firmware calls it %s)   [%d of 7 seen]"
                              % (i, names.get(i, "lid %d" % i), len(seen)), flush=True)
                    else:
                        print("    clear  hall %d" % i, flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        try:
            s.write(b"\r\n")           # any key stops the live table
            s.close()
        except Exception:
            pass

    print("\n  summary")
    for i in range(LIDS):
        mark = "ok  " if i in seen else "MISS"
        print("    %s hall %d  %-10s %d trigger(s)"
              % (mark, i, names.get(i, ""), counts[i]))
    missing = [i for i in range(LIDS) if i not in seen]
    if missing:
        print("\n  never saw a magnet: %s" % ", ".join("hall %d" % i for i in missing))
        print("  Unwired sensor, no magnet close enough, or a dead AH1912.")
    else:
        print("\n  all seven sensors responded.")
    print("  If the sensor number did not match the day printed on the lid you lifted,")
    print("  that pair goes in hallToLid (Settings > Hardware in the GUI, or run tools/mapdays.py).")


if __name__ == "__main__":
    main()
