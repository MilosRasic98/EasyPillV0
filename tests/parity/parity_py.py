"""Python half of the firmware/simulator parity harness.

Prints exactly the report tests/parity/parity_driver.cpp prints, but computed
with sim/easypill_sim.py's own scheduler. run_parity.sh diffs the two, so the
diff IS the assertion: 28 of the 87 defects found in review were the firmware
and the simulator quietly disagreeing, and nothing else in the tree notices
when they drift apart again.

Keep the two writers in step: same questions, same order, same formatting.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'sim'))

import easypill_sim as S  # noqa: E402  (path has to be set up first)

# Same fixed day as the C++ driver: 2026-10-31, a Saturday.
DAY = 20757

out = sys.stdout.write


def build(mode, before, after, grace):
    sch = S.default_settings()["schedule"]
    sch["mode"] = mode
    sch["windowBeforeMin"] = before
    sch["windowAfterMin"] = after
    sch["missedGraceMin"] = grace
    sch["sequentialAnchorDay"] = DAY
    return sch


def report_lid_map():
    for m, mode in enumerate(S.LID_MODES):
        sch = build(mode, 60, 120, 15)
        for d in range(DAY - 3, DAY + 11):
            row = "lidday mode=%d day=%d -> lid=%d" % (m, d, S.lid_for_day(sch, d))
            for lid in range(S.LID_COUNT):
                row += " a%d=%d" % (lid, 1 if S.lid_active_on_day(sch, lid, d) else 0)
            out(row + "\n")


def report_time_calc():
    for d in range(DAY - 2, DAY + 9):
        out("time day=%d mon0=%d sun0=%d\n"
            % (d, S.weekday_mon0_from_day(d), S.weekday_sun0(S.local_epoch_for(d, 0))))
    for minute in range(0, 1440, 37):
        text = S.format_hhmm(minute)
        back = S.parse_hhmm(text)
        ok = back is not None
        out("hhmm %d -> %s -> ok=%d %d\n" % (minute, text, 1 if ok else 0, back if ok else 0))


def inst_or_none(inst):
    if inst is None:
        return (0, 0, 0, 0)
    return (1, inst["localEpoch"], inst["lid"], inst["dose"])


def report_next_dose():
    for m, mode in enumerate(S.LID_MODES):
        sch = build(mode, 60, 120, 15)
        for minute in range(0, 1440, 7):
            now = S.local_epoch_for(DAY, 0) + minute * 60
            # The Python returns the instance or None where the C++ returns a
            # DoseInstance with valid=false; flatten both to the same four fields.
            n = inst_or_none(S.next_dose(sch, now))
            p = inst_or_none(S.previous_dose(sch, now))
            out("next mode=%d now=%d -> n(%d,%d,%d,%d) p(%d,%d,%d,%d)\n"
                % (m, minute, n[0], n[1], n[2], n[3], p[0], p[1], p[2], p[3]))


def report_match():
    windows = ((60, 120, 15), (0, 0, 0), (30, 60, 0), (120, 240, 60))
    for m, mode in enumerate(S.LID_MODES):
        for before, after, grace in windows:
            sch = build(mode, before, after, grace)
            for lid in range(S.LID_COUNT):
                for minute in range(-60, 1501, 10):
                    open_local = S.local_epoch_for(DAY, 0) + minute * 60
                    r = S.match_open(sch, lid, open_local)
                    out("match mode=%d w=%d/%d/%d lid=%d open=%d -> m=%d sched=%d "
                        "dose=%d delta=%d state=%s wrong=%d\n"
                        % (m, before, after, grace, lid, minute,
                           1 if r["matched"] else 0, r["scheduledLocal"], r["dose"],
                           r["deltaMin"], r["state"], 1 if r["wrongDay"] else 0))


def report_sweep_and_stats():
    for before, after, grace in ((60, 120, 15), (60, 120, 0), (0, 30, 30)):
        sch = build("weekday", before, after, grace)
        for minute in range(0, 1441, 5):
            log = S.AdherenceLog()
            now = S.local_epoch_for(DAY, 0) + minute * 60
            swept = min(len(log.sweep_missed(sch, now, 2)), 16)
            st = log.today_stats(sch, now)
            out("sweep w=%d/%d/%d now=%d -> swept=%d due=%d taken=%d missed=%d "
                "pending=%d pct=%d\n"
                % (before, after, grace, minute, swept, st["due"], st["taken"],
                   st["missed"], st["pending"], log.adherence_percent(sch, now, 7)))


def report_floor():
    sch = build("weekday", 60, 120, 15)
    now = S.local_epoch_for(DAY, 23 * 60)
    for floor_min in range(-1, 1441, 15):
        log = S.AdherenceLog()
        floor = 0 if floor_min < 0 else S.local_epoch_for(DAY, floor_min)
        swept = min(len(log.sweep_missed(sch, now, 2, floor)), 16)
        st = log.today_stats(sch, now, floor)
        out("floor at=%d -> swept=%d due=%d pending=%d pct=%d\n"
            % (floor_min, swept, st["due"], st["pending"],
               log.adherence_percent(sch, now, 7, floor)))


report_lid_map()
report_time_calc()
report_next_dose()
report_match()
report_sweep_and_stats()
report_floor()
