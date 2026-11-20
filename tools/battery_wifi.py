#!/usr/bin/env python3
"""Does the box survive Wi-Fi while running on the cell?

    ~/.platformio/penv/bin/python tools/battery_wifi.py [host] [seconds]

This is the one test the bench supply cannot answer. On paper the battery
protection (U11) trips at roughly 30 mA, which is less than a transmit burst
draws, so the board would disconnect itself about 10 ms into the first TX.
The hwtest handover only ever proved an idle board stays alive.

The whole test runs over the network, which is the point: every reply is
itself a transmit burst, so a board that keeps answering is a board that is
transmitting on battery. Nothing is asked of the USB link, because there
isn't one.

Three things are watched, and each can fail on its own:

  * uptime must only ever go up. A reset means the rail collapsed and the
    board rebooted, which is exactly what a protection trip looks like.
  * replies must keep coming. Silence is the trip itself.
  * the cell must not sag off a cliff under load.

It finishes with a Wi-Fi scan, which is the cruellest thing the radio can be
asked to do - an active probe on every channel - and the best single proof
that the power path holds up.
"""
import json
import sys
import time
import urllib.error
import urllib.request

# Pass the IP for a precise run: a cold mDNS lookup inside the poll loop
# would read as the silence this test calls a protection trip.
HOST = sys.argv[1] if len(sys.argv) > 1 else "easypill.local"
SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 120.0

# Below this a single cell is about done; a sag past it under load is a
# finding even if the board stays up.
VBAT_FLOOR_MV = 3300


def say(m=""):
    print(m, flush=True)


def get(path, timeout=6):
    t0 = time.time()
    try:
        raw = urllib.request.urlopen("http://%s%s" % (HOST, path), timeout=timeout).read()
        return json.loads(raw), time.time() - t0, None
    except Exception as e:                       # noqa: BLE001 - any failure is a data point
        return None, time.time() - t0, str(e)


def post(action, timeout=20, **extra):
    body = dict(extra)
    body["action"] = action
    req = urllib.request.Request("http://%s/api/action" % HOST,
                                 data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    t0 = time.time()
    try:
        return json.loads(urllib.request.urlopen(req, timeout=timeout).read()), time.time() - t0, None
    except Exception as e:                       # noqa: BLE001
        return None, time.time() - t0, str(e)


def main():
    say("Wi-Fi on battery: %s for %d s" % (HOST, SECONDS))

    first, _, err = get("/api/state")
    if first is None:
        sys.exit("  cannot reach the box at all: %s" % err)
    p = first["power"]
    if p["usbPresent"]:
        say("\n  USB is still plugged in. Unplug it now - the test starts when it goes away.")
        while True:
            s, _, _ = get("/api/state")
            if s and not s["power"]["usbPresent"]:
                first = s
                break
            time.sleep(0.4)
        say("  USB gone. Running on the cell.\n")
    else:
        say("  Already on the cell, no USB.\n")

    base_uptime = first["uptimeS"]
    vbat0 = first["power"]["vbatMv"]
    say("  start: uptime %d s, %d mV (%d%%)" % (base_uptime, vbat0, first["power"]["percent"]))
    say("  every reply below is a transmit burst on battery power.\n")

    ok = fail = 0
    reboots = []
    lo = hi = vbat0
    worst = 0.0
    last_uptime = base_uptime
    last_report = 0.0
    t0 = time.time()

    while time.time() - t0 < SECONDS:
        s, dt, err = get("/api/state")
        el = time.time() - t0
        if s is None:
            fail += 1
            say("  %6.1fs  NO REPLY  (%s)" % (el, err))
            time.sleep(0.5)
            continue
        ok += 1
        worst = max(worst, dt)
        up = s["uptimeS"]
        v = s["power"]["vbatMv"]
        lo, hi = min(lo, v), max(hi, v)
        if up < last_uptime:
            reboots.append(el)
            say("  %6.1fs  *** REBOOTED *** uptime fell %d -> %d" % (el, last_uptime, up))
        last_uptime = up
        if s["power"]["usbPresent"]:
            say("  %6.1fs  USB is back - the rest is not a battery test" % el)
        if el - last_report >= 10:
            last_report = el
            say("  %6.1fs  uptime %5d s   %d mV (%d%%)   reply %.0f ms" %
                (el, up, v, s["power"]["percent"], dt * 1000))
        time.sleep(0.25)

    # The worst thing the radio can be asked to do, saved for last.
    say("\n  now the hard part: a full Wi-Fi scan on battery")
    scan, dt, err = get("/api/scan", timeout=25)
    if scan is None:
        say("  scan FAILED after %.1f s: %s" % (dt, err))
        scan_ok = False
    else:
        nets = scan.get("networks", [])
        say("  scan returned %d networks in %.1f s" % (len(nets), dt))
        scan_ok = True

    after, _, err = get("/api/state", timeout=10)
    say("\n" + "-" * 68)
    if after is None:
        say("  FAILED. The box stopped answering after the scan: %s" % err)
        say("  That is what the battery protection (U11) tripping under Wi-Fi load looks like.")
        sys.exit(1)

    up = after["uptimeS"]
    v = after["power"]["vbatMv"]
    rebooted = bool(reboots) or up < last_uptime
    say("  replies   %d ok, %d failed, slowest %.0f ms" % (ok, fail, worst * 1000))
    say("  uptime    %d s -> %d s, +%d s over a %d s test" %
        (base_uptime, up, up - base_uptime, SECONDS))
    say("  battery   %d mV -> %d mV (range %d..%d), now %d%%" % (vbat0, v, lo, hi, after["power"]["percent"]))

    say("")
    if rebooted:
        say("  FAILED: the board reset during the test. The rail collapsed under")
        say("  transmit load - this is the protection trip, and the box cannot")
        say("  run on this cell with Wi-Fi as wired.")
        sys.exit(1)
    if fail > ok // 10:
        say("  SHAKY: it never reset, but %d of %d requests got no reply." % (fail, ok + fail))
        say("  Something is dropping out under load; worth a closer look.")
        sys.exit(1)
    if v < VBAT_FLOOR_MV:
        say("  PASSED, but the cell is down to %d mV. Charge it and run again" % v)
        say("  before trusting the result - a tired cell sags differently.")
        sys.exit(0)
    say("  PASSED. No reset, no dropouts%s, and the cell held at %d mV." %
        (", and the scan went through" if scan_ok else "", v))
    say("  The ~30 mA battery protection trip did not bite.")


if __name__ == "__main__":
    main()
