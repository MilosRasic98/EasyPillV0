#!/usr/bin/env python3
"""Guided acceptance test for an assembled EasyPill board.

Covers the seven hall sensors, the button and the battery path. It does NOT
touch the LED or the buzzer -- run `L` and `z` by hand for those.

    ~/.platformio/penv/bin/python tools/hwtest.py

Flash `bringup-c3` first: this drives that firmware's `j` command, which emits
one machine-readable snapshot per poll. Nothing else may hold the port.

The rule this whole script is built around: a reading is not a result. An
AH1912 with no ground reads HIGH exactly like a healthy sensor with no magnet,
and a self-test that called the first one a PASS is how two dead sensors got
signed off on this very board. So nothing here passes on an idle level. Every
sensor must be seen to CHANGE, under your hand, before it counts.

Ctrl-C at any point: you get the report for what was proved so far.
"""
import glob
import json
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is missing. Use PlatformIO's interpreter, which has it:\n"
             "  ~/.platformio/penv/bin/python tools/hwtest.py")

BAUD = 115200
LIDS = 7
POLL_S = 0.15

# A LiPo below this is flat or absent; above it is a bad reading or a bench
# supply. docs/HARDWARE.md 9: the pack is a single cell, 3.0 V to 4.2 V.
VBAT_MIN, VBAT_MAX = 3000, 4400


class Board:
    """The serial link, with the reconnection that USB-CDC makes necessary."""

    def __init__(self, port):
        self.port = port
        self.s = None
        self.open()

    def open(self):
        said = False
        while True:
            try:
                s = serial.Serial(self.port, BAUD, timeout=0.4)
                s.dtr, s.rts = True, False
                time.sleep(1.2)
                s.write(b"\r\n")      # stop any live view a previous session armed
                s.flush()
                time.sleep(0.5)
                s.reset_input_buffer()
                self.s = s
                return
            except (OSError, serial.SerialException) as err:
                if not said:
                    why = ("busy - close any pio device monitor"
                           if "Resource busy" in str(err) else str(err).strip())
                    say("  waiting for %s (%s)" % (self.port, why))
                    said = True
                time.sleep(0.5)

    def sample(self):
        """One snapshot, or None if the link is down. Never raises."""
        try:
            self.s.reset_input_buffer()
            self.s.write(b"j\r\n")
            self.s.flush()
            end = time.time() + 1.5
            buf = ""
            while time.time() < end:
                n = self.s.in_waiting
                if not n:
                    time.sleep(0.02)
                    continue
                buf += self.s.read(n).decode("utf-8", "replace")
                for line in buf.splitlines():
                    line = line.strip()
                    if line.startswith("{") and line.endswith("}"):
                        try:
                            return json.loads(line)
                        except ValueError:
                            pass
            return None
        except (OSError, serial.SerialException):
            return None

    def reconnect(self):
        try:
            self.s.close()
        except Exception:
            pass
        self.s = None
        self.open()


def say(msg=""):
    print(msg, flush=True)


def bits(mask):
    return [(mask >> i) & 1 for i in range(LIDS)]


class Report:
    def __init__(self):
        self.rows = []

    def add(self, ok, item, detail):
        self.rows.append((ok, item, detail))
        say("  [%s] %-18s %s" % ("PASS" if ok else "FAIL", item, detail))

    def failed(self):
        return [r for r in self.rows if not r[0]]


def wait_for(board, done, timeout, tick=None):
    """Poll until done(snapshot) is true. Returns the snapshot, or None."""
    end = time.time() + timeout
    while time.time() < end:
        snap = board.sample()
        if snap is None:
            say("  ... link dropped, reconnecting")
            board.reconnect()
            say("  ... back")
            continue
        if tick:
            tick(snap)
        if done(snap):
            return snap
        time.sleep(POLL_S)
    return None


# --- the phases -------------------------------------------------------------

def phase_bus(board, rep):
    say("\n1. The mux and the address lines")
    snap = wait_for(board, lambda s: True, 10)
    if snap is None:
        rep.add(False, "link", "no JSON from `j` - is bringup-c3 flashed?")
        return None
    if snap.get("gnd"):
        rep.add(True, "mux + address", "channel 7 reads LOW through R38, so MUX_A/B/C and U20 are alive")
    else:
        rep.add(False, "mux + address",
                "channel 7 did not read LOW - every hall reading below is fiction. Stop here.")
    return snap


def phase_halls(board, rep):
    say("\n2. The seven hall sensors")
    say("   Put a magnet on each compartment in turn, then take it away.")
    say("   A sensor only counts once it has been seen BOTH ways - idle HIGH proves nothing,")
    say("   because an unpowered sensor reads HIGH too.\n")

    low_seen = [False] * LIDS       # magnet present at least once
    high_seen = [False] * LIDS      # released at least once
    changes = [0] * LIDS
    last = [None] * LIDS
    order = []                      # the order sensors first responded in

    def tick(snap):
        raw = bits(snap.get("raw", 0))
        for i in range(LIDS):
            magnet = raw[i] == 0
            if magnet:
                if not low_seen[i]:
                    order.append(i)
                low_seen[i] = True
            else:
                high_seen[i] = True
            if last[i] is not None and magnet != last[i]:
                changes[i] += 1
                if magnet:
                    done = sum(1 for k in range(LIDS) if low_seen[k] and high_seen[k])
                    say("    magnet on hall %d   [%d of 7 proved]" % (i, done))
            last[i] = magnet

    proved = lambda s: all(low_seen[i] and high_seen[i] for i in range(LIDS))
    got = wait_for(board, proved, 300, tick)

    for i in range(LIDS):
        if low_seen[i] and high_seen[i]:
            rep.add(True, "hall %d" % i, "went LOW and back, %d transition(s)" % changes[i])
        elif low_seen[i]:
            rep.add(False, "hall %d" % i, "saw the magnet but never released - stuck LOW?")
        else:
            rep.add(False, "hall %d" % i,
                    "never changed. Unpowered, unsoldered, no ground, or the magnet "
                    "never reached it.")
    if got is None:
        say("\n   (timed out waiting for all seven)")
    if order:
        say("\n   First response order was: %s" % ", ".join("hall %d" % i for i in order))
        say("   If that is not the order you touched them in, hallToLid needs remapping.")
    return order


def phase_button(board, rep):
    say("\n3. The button")
    say("   Press and hold it for a moment, then let go.\n")
    snap = wait_for(board, lambda s: s.get("btn") == 1, 90)
    if snap is None:
        rep.add(False, "button press", "never went active within 90 s. Check the JST and R30.")
        return
    say("    pressed  (%d mV at the pin)" % snap.get("btnMv", 0))
    rel = wait_for(board, lambda s: s.get("btn") == 0, 60)
    if rel is None:
        rep.add(False, "button release", "stayed active - stuck switch, or a short")
        return
    rep.add(True, "button", "pressed at %d mV and released at %d mV"
            % (snap.get("btnMv", 0), rel.get("btnMv", 0)))


def phase_battery(board, rep):
    say("\n4. The battery and the power path")
    snap = board.sample()
    while snap is None:
        board.reconnect()
        snap = board.sample()

    vbat = snap.get("vbat", 0)
    if VBAT_MIN <= vbat <= VBAT_MAX:
        rep.add(True, "battery voltage", "%d mV (%d%%) - a plausible single cell"
                % (vbat, snap.get("pct", 0)))
    else:
        rep.add(False, "battery voltage",
                "%d mV is outside %d..%d mV - no cell fitted, flat, or the divider is wrong"
                % (vbat, VBAT_MIN, VBAT_MAX))
        say("   Skipping the handover test; connect a charged cell and run again.")
        return

    if not snap.get("usbIn"):
        rep.add(False, "USB rail", "no USB detected, so the handover test cannot run")
        return

    # The real test of the power path, and the only one that proves the board
    # can live without the bench: pull USB and see whether it keeps running on
    # the cell. The CDC link dies with the USB, so uptime is the evidence - if
    # it kept counting across the gap the board never rebooted.
    before = snap.get("uptimeMs", 0)
    say("\n   Now UNPLUG the USB cable. The link will drop - that is expected.")
    say("   Wait about five seconds, then plug it back in.\n")

    end = time.time() + 180
    after = None
    while time.time() < end:
        s = board.sample()
        if s is None:
            board.reconnect()
            continue
        if s.get("uptimeMs", 0) < before:        # the clock restarted: it rebooted
            after = s
            break
        if not s.get("usbIn"):                   # still unplugged, keep waiting
            time.sleep(POLL_S)
            continue
        if s.get("uptimeMs", 0) > before + 4000:  # back, and time kept running
            after = s
            break
        time.sleep(POLL_S)

    if after is None:
        rep.add(False, "battery handover", "timed out waiting for the cable to come back")
    elif after.get("uptimeMs", 0) < before:
        rep.add(False, "battery handover",
                "uptime restarted, so the board REBOOTED when USB went away - it is not "
                "running from the cell. Check the latch, the cell and the buck/boost.")
    else:
        rep.add(True, "battery handover",
                "uptime ran from %.1f s to %.1f s across the unplug, so it stayed alive "
                "on the cell" % (before / 1000.0, after.get("uptimeMs", 0) / 1000.0))
        if after.get("chg"):
            rep.add(True, "charger", "STAT says charging now that USB is back")
        else:
            say("  [note] charger        STAT does not say charging - full cell, or no charge current")


def find_port(argv):
    """Block until a board shows up. You end up unplugging this thing a lot -
    the handover test in phase 4 asks you to - so starting before the cable is
    in has to be allowed rather than punished with an exit."""
    if len(argv) > 1:
        return argv[1]
    said = False
    while True:
        found = sorted(glob.glob("/dev/cu.usbmodem*"))
        if found:
            return found[0]
        if not said:
            say("  no /dev/cu.usbmodem* yet - waiting for the board (Ctrl-C to give up)")
            said = True
        time.sleep(0.5)


def main():
    port = find_port(sys.argv)

    say("EasyPill hardware test on %s" % port)
    say("Halls, button and battery. LED and buzzer are not covered - use `L` and `z`.")
    say("Ctrl-C stops and reports what was proved.")

    board = Board(port)
    rep = Report()
    try:
        first = phase_bus(board, rep)
        if first is not None and first.get("gnd"):
            phase_halls(board, rep)
            phase_button(board, rep)
            phase_battery(board, rep)
    except KeyboardInterrupt:
        say("\n  (stopped)")
    finally:
        try:
            board.s.close()
        except Exception:
            pass

    say("\n" + "-" * 68)
    bad = rep.failed()
    say("  %d passed, %d failed, of %d checks run"
        % (len(rep.rows) - len(bad), len(bad), len(rep.rows)))
    if bad:
        say("\n  still to fix:")
        for _, item, detail in bad:
            say("    %-18s %s" % (item, detail))
    else:
        say("\n  Halls, button and battery all proved. LED and buzzer still untested.")


if __name__ == "__main__":
    main()
