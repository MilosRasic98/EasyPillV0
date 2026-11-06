#!/usr/bin/env python3
"""Work out which hall sensor sits under which day, and set it.

    ~/.platformio/penv/bin/python tools/mapdays.py [host]

It names a day; you open that lid and close it again. No keyboard -- it watches
`rawMask` and advances by itself, so both hands stay on the box.

`rawMask` is indexed by SENSOR, before any mapping is applied. This sees which
bit moved and builds `hallToLid`, whose index is the sensor and whose value is
the lid. Lid 0 is Monday, lid 6 is Sunday.

It talks to the running app firmware over HTTP, so no USB and no reflash.
That matters more than it sounds: hallToLid lives in the NVS settings blob,
and storeLoadSettings() only falls back to the compiled defaults when the
blob is missing or its version changed. Flashing new firmware therefore does
NOT change the mapping on a board that already has settings stored -- this
does, and it takes effect immediately.

Nothing is written until all seven are mapped and you have said yes, so if you
open the wrong lid just Ctrl-C and start again.
"""
import json
import sys
import time
import urllib.request

HOST = sys.argv[1] if len(sys.argv) > 1 else "easypill.local"
DAYS = ["Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"]
LIDS = 7
POLL_S = 0.12
# Long enough to walk round the box and think, short enough that a dead sensor
# does not hold the bench up all afternoon.
OPEN_TIMEOUT_S = 120
CLOSE_TIMEOUT_S = 120


def say(m=""):
    print(m, flush=True)


def api(path, body=None, timeout=10):
    url = "http://%s%s" % (HOST, path)
    if body is None:
        return json.loads(urllib.request.urlopen(url, timeout=timeout).read())
    req = urllib.request.Request(url, data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    return json.loads(urllib.request.urlopen(req, timeout=timeout).read())


def raw():
    """Current sensor bits, or None if the box did not answer. The web server
    takes one connection at a time, so a miss here is routine - never fatal."""
    try:
        return api("/api/state")["sensors"]["rawMask"]
    except Exception:                            # noqa: BLE001
        return None


def settle(want=None, timeout=6.0):
    """The steady value of rawMask. With `want`, insist on that value being the
    steady one -- used to confirm a lid really went back down."""
    end = time.time() + timeout
    last, since = raw(), time.time()
    while time.time() < end:
        v = raw()
        if v is None:
            time.sleep(0.2)
            continue
        if v != last:
            last, since = v, time.time()
        elif time.time() - since > 0.5 and (want is None or v == want):
            return v
        time.sleep(POLL_S)
    return last


def wait_for_change(base, timeout):
    """Block until rawMask differs from `base` and holds. Returns the list of
    sensor bits that moved, or None on timeout."""
    end = time.time() + timeout
    while time.time() < end:
        v = raw()
        if v is None or v == base:
            time.sleep(POLL_S)
            continue
        v = settle()                              # let the debounce finish
        if v is None or v == base:
            continue
        return [i for i in range(LIDS) if ((v >> i) & 1) != ((base >> i) & 1)]
    return None


def wait_for_return(base, timeout):
    """Block until rawMask is back to `base`. True if it got there."""
    end = time.time() + timeout
    while time.time() < end:
        if raw() == base and settle(want=base, timeout=2.0) == base:
            return True
        time.sleep(POLL_S)
    return False


def main():
    say("Mapping sensors to days on %s" % HOST)
    try:
        api("/api/action", {"action": "stayAwake", "seconds": 1800})
    except Exception as e:                       # noqa: BLE001
        sys.exit("  cannot reach the box: %s\n  Is it powered and on the network?" % e)

    base = settle()
    if base is None:
        sys.exit("  no reading from the box")
    if base != 0:
        say("\n  rawMask is 0x%02X, so something reads open already." % base)
        say("  Close every lid and run this again - the baseline has to be all-closed.")
        sys.exit(1)
    say("  baseline rawMask = 0x%02X, all seven closed\n" % base)
    say("  Open the lid I name, then close it again. I will keep up.")
    say("  Ctrl-C at any point: nothing is written until the end.\n")

    mapping = {}                                  # sensor -> lid
    for lid, day in enumerate(DAYS):
        while True:
            say("  open %s ..." % day.upper())
            moved = wait_for_change(base, OPEN_TIMEOUT_S)
            if moved is None:
                say("  ! nothing moved in %d s. Lift it further, or that sensor is dead"
                    " (docs/HARDWARE.md 3).\n" % OPEN_TIMEOUT_S)
                continue
            if len(moved) > 1:
                say("  ! %d sensors moved together (%s). One lid at a time - close them"
                    " and I will retry.\n" % (len(moved), ", ".join(str(m) for m in moved)))
                wait_for_return(base, CLOSE_TIMEOUT_S)
                continue
            sensor = moved[0]
            if sensor in mapping:
                say("  ! sensor %d is already %s. Wrong lid - close it and I will retry.\n"
                    % (sensor, DAYS[mapping[sensor]]))
                wait_for_return(base, CLOSE_TIMEOUT_S)
                continue
            say("      %-9s -> sensor %d   [%d of 7]" % (day, sensor, len(mapping) + 1))
            if not wait_for_return(base, CLOSE_TIMEOUT_S):
                say("  ! it never closed again. Leaving %s unmapped would be worse than"
                    " stopping here." % day)
                sys.exit(1)
            mapping[sensor] = lid
            break

    table = [mapping[s] for s in range(LIDS)]
    say("\n" + "-" * 60)
    for s in range(LIDS):
        say("    sensor %d  ->  %s" % (s, DAYS[table[s]]))
    say("\n  hallToLid = %s" % json.dumps(table))

    if table == list(range(LIDS)):
        say("\n  That is the identity order the box already has - nothing to change.")
        return

    say("\n  Write this to the box? Takes effect at once, no reflash. [y/N]")
    try:
        answer = input().strip().lower()
    except EOFError:
        answer = ""
    if answer not in ("y", "yes"):
        say("  Nothing written.")
        return
    api("/api/settings", {"hw": {"hallToLid": table}})
    back = list(api("/api/settings")["hw"]["hallToLid"])
    if back == table:
        say("  Written and read back: %s" % json.dumps(back))
        say("\n  Put this in applyDefaults() in")
        say("  firmware/lib/easypill_core/ep_settings.cpp so a factory reset or a")
        say("  freshly flashed board starts out this way too:")
        say("    hallToLid = %s" % json.dumps(table))
    else:
        say("  ! wrote %s but read back %s" % (json.dumps(table), json.dumps(back)))
        say("  The box is NOT mapped the way you just proved. Do not trust it yet.")


if __name__ == "__main__":
    main()
