#!/usr/bin/env python3
"""EasyPill V0 host simulator -- the whole product, minus the hardware.

Why this exists: the box is a 7-lid pillbox whose entire user-visible surface is
a web GUI and a stream of push notifications. None of that needs an ESP32 to
exercise, so this file fakes the hardware (lids, battery, chargers, clock) and
serves the *real* GUI out of web/ plus the same HTTP API as the firmware.
Open the printed URL and you are looking at what the device will serve.

Two things are deliberately faithful rather than convenient:

  * the scheduling and adherence maths is a line-for-line reimplementation of
    firmware/lib/easypill_core/ep_schedule.h and ep_adherence.h, local-epoch
    integer arithmetic included, so a bug here is a bug there;
  * there is no RTC on the board, so the simulated device clock *drifts* and
    only becomes trustworthy after an SNTP "sync". Everything derives from the
    simulated clock, never from time.time(), which is what lets a 3600x run
    exercise a week of schedule in three minutes.

Python 3.9, standard library only. Run it with no arguments.
"""

import argparse
import datetime
import gzip
import http.server
import json
import os
import posixpath
import random
import secrets
import signal
import socket
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

FW_VERSION = "0.1.0"          # ep::kFirmwareVersion
SIM_VERSION = "1.0.0"
CHIP_NAME = "SIM"
LID_COUNT = 7                 # ep::kLidCount
# ep::kDefaultHallToLid. The V0 sensor row runs opposite to the days; measured
# on hardware. Must match the core byte for byte or the
# parity harness diverges on the very first settings comparison.
DEFAULT_HALL_TO_LID = [6, 5, 4, 3, 2, 1, 0]
MAX_DOSES_PER_LID = 4         # ep::kMaxDosesPerLid
MAX_PILLS_PER_DOSE = 4        # ep::kMaxPillsPerDose
NO_LID = 0xFF                 # ep::kNoLid
EVENT_CAPACITY = 64           # ep::EventLog::kCapacity
RECORD_CAPACITY = 128         # ep::AdherenceLog::kCapacity
SECRET_PLACEHOLDER = "__set__"

SEC_PER_MIN = 60
SEC_PER_HOUR = 3600
SEC_PER_DAY = 86400
MIN_PER_DAY = 1440

# ---------------------------------------------------------------------------
# Enum spellings. These go into the JSON API, so they are not reworded
# casually: ep_types.h toString() must agree with every name here.
# ---------------------------------------------------------------------------

EV_BOOT = "boot"
EV_LID_OPENED = "lidOpened"
EV_LID_CLOSED = "lidClosed"
EV_DOSE_TAKEN = "doseTaken"
EV_DOSE_MISSED = "doseMissed"
EV_DOSE_DUE = "doseDue"
EV_DOSE_REMINDER = "doseReminder"
EV_UNEXPECTED_OPEN = "unexpectedOpen"
EV_WRONG_DAY_OPEN = "wrongDayOpen"
EV_BUTTON_PRESS = "buttonPress"
EV_BUTTON_HOLD = "buttonHold"
EV_CHARGER_IN = "chargerIn"
EV_CHARGER_OUT = "chargerOut"
EV_BATTERY_LOW = "batteryLow"
EV_BATTERY_CRITICAL = "batteryCritical"
EV_TIME_SYNCED = "timeSynced"
EV_WIFI_UP = "wifiUp"
EV_WIFI_DOWN = "wifiDown"
EV_SLEEP_ENTER = "sleepEnter"
EV_SLEEP_EXIT = "sleepExit"
EV_CONFIG_CHANGED = "configChanged"
EV_SENSOR_FAULT = "sensorFault"

ST_PENDING = "pending"
ST_TAKEN = "taken"
ST_LATE = "late"
ST_EARLY = "early"
ST_MISSED = "missed"
ST_SKIPPED = "skipped"
ST_NONE = "none"              # /api/state todayState only: nothing scheduled

DOSE_STATES = (ST_PENDING, ST_TAKEN, ST_LATE, ST_EARLY, ST_MISSED, ST_SKIPPED)
TAKEN_STATES = (ST_TAKEN, ST_LATE, ST_EARLY)   # ep::isTakenState

LID_MODES = ("weekday", "sequential")
SLEEP_POLICIES = ("alwaysOn", "smart", "aggressive")

WEEKDAY_NAMES = ("Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
                 "Saturday", "Sunday")


def is_taken_state(state):
    return state in TAKEN_STATES


def default_lid_name(lid, mode):
    if mode == "sequential":
        return "Day %d" % (lid + 1)
    return WEEKDAY_NAMES[lid % 7]


# ---------------------------------------------------------------------------
# ep_timecalc.h -- calendar arithmetic in local epoch seconds.
#
# Local epoch is UTC epoch already shifted by the device's UTC offset, so day
# and minute maths is plain integer division and the GUI can render a timestamp
# without knowing the timezone.
# ---------------------------------------------------------------------------

def day_index(local_epoch):
    return int(local_epoch) // SEC_PER_DAY


def day_start(local_epoch):
    return day_index(local_epoch) * SEC_PER_DAY


def minute_of_day(local_epoch):
    return (int(local_epoch) % SEC_PER_DAY) // SEC_PER_MIN


def weekday_mon0_from_day(day_idx):
    # 1970-01-01 was a Thursday, which is the whole trick: Thursday is 3 with
    # Monday as 0.
    return (int(day_idx) + 3) % 7


def weekday_mon0(local_epoch):
    return weekday_mon0_from_day(day_index(local_epoch))


def weekday_sun0(local_epoch):
    return (day_index(local_epoch) + 4) % 7


def local_epoch_for(day_idx, minute):
    return int(day_idx) * SEC_PER_DAY + int(minute) * SEC_PER_MIN


def format_hhmm(minute):
    minute = int(minute) % MIN_PER_DAY
    return "%02d:%02d" % (minute // 60, minute % 60)


def parse_hhmm(text):
    """Accepts "8:30", "08:30", "0830". Returns None on anything else."""
    if not isinstance(text, str):
        return None
    s = text.strip()
    if not s:
        return None
    if ":" in s:
        parts = s.split(":")
        if len(parts) != 2:
            return None
        hh, mm = parts[0].strip(), parts[1].strip()
    elif len(s) == 4 and s.isdigit():
        hh, mm = s[:2], s[2:]
    else:
        return None
    if not (hh.isdigit() and mm.isdigit()):
        return None
    h, m = int(hh), int(mm)
    if h > 23 or m > 59:
        return None
    return h * 60 + m


def minute_delta(from_local, to_local):
    """Signed minutes, truncated toward zero and saturated like an int16."""
    d = int(to_local) - int(from_local)
    # C++ integer division truncates toward zero; Python's floors. Match C++.
    mins = int(d / SEC_PER_MIN)
    return max(-32767, min(32767, mins))


def format_local(local_epoch):
    """"Sun 2026-11-01 08:30" -- timekeeper.h timeFormat()."""
    dt = datetime.datetime(1970, 1, 1) + datetime.timedelta(seconds=int(local_epoch))
    return dt.strftime("%a %Y-%m-%d %H:%M")


def host_tz_offset_sec(when_utc=None):
    """Seconds east of UTC for the host right now, DST included."""
    when = when_utc or datetime.datetime.now()
    off = when.astimezone().utcoffset()
    return int(off.total_seconds()) if off else 0


# ---------------------------------------------------------------------------
# ep_settings.h -- applyDefaults(). One place decides what a fresh device
# looks like: no Wi-Fi, Europe/Belgrade, 08:00 and 20:00 every day so the GUI
# has something to show, notifications off.
# ---------------------------------------------------------------------------

def default_dose(minute, label):
    return {"enabled": True, "minute": minute, "label": label, "pills": []}


def default_schedule():
    lids = []
    for lid in range(LID_COUNT):
        doses = [default_dose(8 * 60, "Morning"), default_dose(20 * 60, "Evening")]
        # ep_settings.cpp defaultDose() seeds one placeholder pill per dose, so a
        # fresh device shows a populated schedule instead of seven empty days
        # that look like a bug. default_dose() itself stays blank, because it is
        # also the blank-dose factory for parse_dose() and apply_dose_to_lids(),
        # where the firmware builds a zeroed Dose carrying no pills at all.
        for dose in doses:
            dose["pills"] = [{"name": "Pill", "dose": "", "count": 1}]
        lids.append({
            "name": default_lid_name(lid, "weekday"),
            "doses": doses,
        })
    return {
        "mode": "weekday",
        "sequentialAnchorDay": 0,
        "windowBeforeMin": 60,
        "windowAfterMin": 120,
        "missedGraceMin": 15,
        "remindBeforeMin": 0,
        "lids": lids,
    }


def default_settings():
    return {
        "ui": {"deviceName": "EasyPill", "patientName": "Grandma", "showAdvanced": False},
        "wifi": {"ssid": "", "pass": "", "hostname": "easypill",
                 "apFallback": True, "apPassword": ""},
        "time": {"ntp1": "pool.ntp.org", "ntp2": "time.google.com",
                 "tz": "CET-1CEST,M3.5.0,M10.5.0/3",
                 "driftPpm": 0, "resyncIntervalSec": 21600},
        "notify": {"ntfyEnabled": False, "ntfyServer": "https://ntfy.sh",
                   "ntfyTopic": "", "ntfyToken": "",
                   "pushoverEnabled": False, "pushoverToken": "", "pushoverUser": "",
                   "webhookEnabled": False, "webhookUrl": "",
                   "onLidOpen": True, "onDoseTaken": True, "onDoseMissed": True,
                   "onDoseDue": False, "onUnexpectedOpen": True,
                   "onLowBattery": True, "onBoot": False, "onDailySummary": False,
                   "dailySummaryMinute": 1260,
                   "quietFromMinute": 0, "quietToMinute": 0},
        "power": {"policy": "smart", "awakeAfterEventSec": 300,
                  "wakeIntervalSec": 900, "lowBatteryMv": 3500,
                  "criticalBatteryMv": 3300, "buzzerOnReminder": True,
                  "ledOnReminder": True},
        "hw": {"hallClosedIsLow": True, "hallInvertMask": 0,
               "hallToLid": list(DEFAULT_HALL_TO_LID),
               "ledChannelRed": 2, "ledChannelGreen": 1, "ledChannelBlue": 0,
               "ledBrightness": 120,
               "buzzerChannel": 3, "buzzerToneHz": 2700, "buzzerSelfDrive": False,
               "vbatDivNum": 175, "vbatDivDen": 100,
               "railDivNum": 175, "railDivDen": 75,
               "buttonDivNum": 1, "buttonDivDen": 1,
               "buttonThresholdMv": 1500, "statThresholdMv": 1500,
               "hallDebounceMs": 60, "buttonDebounceMs": 80},
        "schedule": default_schedule(),
    }


# ---------------------------------------------------------------------------
# ep_schedule.h -- turning a Schedule into concrete dose instances.
# ---------------------------------------------------------------------------

def lid_for_day(sch, day_idx):
    if sch["mode"] == "sequential":
        anchor = int(sch.get("sequentialAnchorDay", 0))
        return int(day_idx - anchor) % LID_COUNT
    return weekday_mon0_from_day(day_idx)


def lid_active_on_day(sch, lid, day_idx):
    return lid_for_day(sch, day_idx) == lid


def next_day_for_lid(sch, lid, from_day):
    """First day index >= from_day on which `lid` is the active lid."""
    for d in range(int(from_day), int(from_day) + LID_COUNT + 1):
        if lid_active_on_day(sch, lid, d):
            return d
    return None


def dose_instances_for_lid_on_day(sch, lid, day_idx):
    """Every enabled instance of one lid placed on one day, ascending.

    Deliberately ignores whose day it is: matchOpen needs the hypothetical
    placement so that opening Friday's lid on Thursday can be recognised as the
    right time in the wrong compartment.
    """
    out = []
    for dose_idx, dose in enumerate(sch["lids"][lid]["doses"][:MAX_DOSES_PER_LID]):
        if not dose.get("enabled"):
            continue
        out.append({"localEpoch": local_epoch_for(day_idx, dose["minute"]),
                    "lid": lid, "dose": dose_idx})
    out.sort(key=lambda i: (i["localEpoch"], i["dose"]))
    return out


def dose_instances_in_range(sch, from_local, to_local, max_out=512):
    """Real timeline instances in [from_local, to_local), ascending.

    One lid owns each day, so this walks days and asks only that day's lid.
    """
    out = []
    if to_local <= from_local:
        return out
    first_day = day_index(from_local)
    last_day = day_index(to_local - 1)
    for d in range(first_day, last_day + 1):
        lid = lid_for_day(sch, d)
        for inst in dose_instances_for_lid_on_day(sch, lid, d):
            if from_local <= inst["localEpoch"] < to_local:
                out.append(inst)
                if len(out) >= max_out:
                    return out
    return out


def next_dose(sch, now_local):
    """Soonest enabled dose strictly after now_local, searching 14 days."""
    now_local = int(now_local)
    for d in range(day_index(now_local), day_index(now_local) + 15):
        lid = lid_for_day(sch, d)
        for inst in dose_instances_for_lid_on_day(sch, lid, d):
            if inst["localEpoch"] > now_local:
                return inst
    return None


def previous_dose(sch, now_local):
    """Most recent enabled dose at or before now_local, searching 14 days back."""
    now_local = int(now_local)
    today = day_index(now_local)
    for d in range(today, today - 15, -1):
        lid = lid_for_day(sch, d)
        for inst in reversed(dose_instances_for_lid_on_day(sch, lid, d)):
            if inst["localEpoch"] <= now_local:
                return inst
    return None


def next_dose_for_lid(sch, lid, now_local):
    """Next instance of one lid on one of *its* days. Feeds lids[].nextAt."""
    now_local = int(now_local)
    for d in range(day_index(now_local), day_index(now_local) + 15):
        if not lid_active_on_day(sch, lid, d):
            continue
        for inst in dose_instances_for_lid_on_day(sch, lid, d):
            if inst["localEpoch"] > now_local:
                return inst
    return None


def enabled_dose_count(sch, lid):
    return sum(1 for d in sch["lids"][lid]["doses"] if d.get("enabled"))


def lid_has_schedule(sch, lid):
    return len(sch["lids"][lid]["doses"]) > 0


def total_pills_per_week(sch):
    total = 0
    for day in range(7):
        lid = lid_for_day(sch, day)   # one full week covers every lid once
        for dose in sch["lids"][lid]["doses"]:
            if not dose.get("enabled"):
                continue
            for pill in dose.get("pills", []):
                total += int(pill.get("count", 0))
    return total


# ---------------------------------------------------------------------------
# ep_adherence.h -- matching an open to an instance, and keeping the record.
# ---------------------------------------------------------------------------

# ep_adherence.cpp kOnTimeSlackSec: how far before D still counts as on time.
ON_TIME_SLACK_SEC = 15 * 60


def classify_open(sch, sched_local, open_local):
    """Taken / Late / Early inside the match window.

    Line for line with ep_adherence.cpp matchOpen(): on time is
    [D - 15 min, D + windowAfterMin/2], earlier is `early`, later is `late`.
    Half the after-window is the boundary so that "a bit late" and "very late"
    are distinguishable without another setting. Compared in *seconds* because
    that is what the firmware compares -- a minute-truncated test disagrees with
    the board inside the last minute.
    """
    sched_local = int(sched_local)
    open_local = int(open_local)
    after_sec = int(sch.get("windowAfterMin", 0)) * SEC_PER_MIN
    if open_local < sched_local - ON_TIME_SLACK_SEC:
        return ST_EARLY
    if open_local > sched_local + after_sec // 2:
        return ST_LATE
    return ST_TAKEN


def match_open(sch, lid, open_local):
    """Which dose instance does an open of `lid` at `open_local` belong to?

    Considers the lid's instances on the previous, current and next day so an
    open just after midnight still finds a 23:50 dose. Nearest candidate inside
    [D - windowBeforeMin, D + windowAfterMin + missedGraceMin] wins; ties go to
    the earlier instance.
    """
    open_local = int(open_local)
    result = {"matched": False, "scheduledLocal": 0, "dose": 0,
              "deltaMin": 0, "state": ST_PENDING, "wrongDay": False}
    before = int(sch.get("windowBeforeMin", 0)) * SEC_PER_MIN
    # The window runs through the grace period too, so a lid opened while the
    # dose is merely overdue still belongs to that dose. Without the grace an
    # open at D + windowAfter + 1 min reads as an unexpectedOpen and is then
    # followed by a doseMissed for the very same dose.
    after = (int(sch.get("windowAfterMin", 0))
             + int(sch.get("missedGraceMin", 0))) * SEC_PER_MIN
    today = day_index(open_local)
    # The day the open *happened* on owns this verdict, and it is set whether or
    # not anything matched: opening Tuesday's lid on Monday is worth reporting
    # even nowhere near a dose time. A 23:50 dose opened at 00:10 is therefore
    # wrongDay as well, which is correct by this rule and self-explanatory once
    # the matched dose time is shown beside it.
    result["wrongDay"] = not lid_active_on_day(sch, lid, today)

    candidates = []
    for d in (today - 1, today, today + 1):
        for inst in dose_instances_for_lid_on_day(sch, lid, d):
            sched = inst["localEpoch"]
            if sched - before <= open_local <= sched + after:
                candidates.append(inst)
    if not candidates:
        return result

    candidates.sort(key=lambda i: (abs(open_local - i["localEpoch"]), i["localEpoch"]))
    best = candidates[0]
    result["matched"] = True
    result["scheduledLocal"] = best["localEpoch"]
    result["dose"] = best["dose"]
    result["deltaMin"] = minute_delta(best["localEpoch"], open_local)
    result["state"] = classify_open(sch, best["localEpoch"], open_local)
    return result


class AdherenceLog:
    """Fixed-capacity ring of dose records, oldest first, newest at the end."""

    def __init__(self):
        self.records = []

    def clear(self):
        self.records = []

    def size(self):
        return len(self.records)

    def find(self, scheduled_local, lid, dose):
        for rec in self.records:
            if (rec["scheduledLocal"] == scheduled_local and rec["lid"] == lid
                    and rec["dose"] == dose):
                return rec
        return None

    def find_or_create(self, scheduled_local, lid, dose):
        rec = self.find(scheduled_local, lid, dose)
        if rec is not None:
            return rec
        rec = {"scheduledLocal": int(scheduled_local), "actualLocal": 0,
               "lid": int(lid), "dose": int(dose), "state": ST_PENDING,
               "deltaMin": 0}
        if len(self.records) >= RECORD_CAPACITY:
            self.records.pop(0)        # evict the oldest, never fail
        self.records.append(rec)
        self.records.sort(key=lambda r: (r["scheduledLocal"], r["lid"], r["dose"]))
        return rec

    def record_open(self, sch, lid, open_local):
        """Returns the MatchResult. An open that matches nothing is not
        recorded here at all; the caller logs it as unexpectedOpen."""
        m = match_open(sch, lid, open_local)
        if not m["matched"]:
            return m
        rec = self.find_or_create(m["scheduledLocal"], lid, m["dose"])
        # First open wins: a second lift of the same lid must not rewrite the
        # time the pills actually came out.
        if rec["actualLocal"] and is_taken_state(rec["state"]):
            return m
        rec["actualLocal"] = int(open_local)
        rec["state"] = m["state"]
        rec["deltaMin"] = m["deltaMin"]
        return m

    def mark_state(self, scheduled_local, lid, dose, state, now_local):
        if state not in DOSE_STATES:
            return False
        rec = self.find_or_create(scheduled_local, lid, dose)
        rec["state"] = state
        if is_taken_state(state):
            if not rec["actualLocal"]:
                rec["actualLocal"] = int(now_local)
            rec["deltaMin"] = minute_delta(rec["scheduledLocal"], rec["actualLocal"])
        elif state in (ST_MISSED, ST_SKIPPED):
            rec["actualLocal"] = 0
            rec["deltaMin"] = 0
        return True

    def sweep_missed(self, sch, now_local, lookback_days, floor_local=0):
        """Mark every instance whose window closed more than missedGraceMin ago
        and that nobody opened. Each newly-missed record is returned exactly
        once, because the mark itself is what stops it coming back."""
        now_local = int(now_local)
        closed_by = (int(sch.get("windowAfterMin", 0))
                     + int(sch.get("missedGraceMin", 0))) * SEC_PER_MIN
        # Whole days back from the start of today, which is what the firmware
        # does (ep_adherence.cpp walks dayIndex(now) - lookbackDays .. dayIndex(now)).
        # A rolling now-minus-48h window would instead let a dose fall out of
        # range depending on what time of day the sweep happens to run, so a
        # miss could go unreported entirely.
        start = day_start(now_local) - int(lookback_days) * SEC_PER_DAY
        out = []
        for inst in dose_instances_in_range(sch, start, now_local):
            if inst["localEpoch"] < int(floor_local):
                continue        # the box was not powered for it
            if inst["localEpoch"] + closed_by >= now_local:
                continue
            rec = self.find(inst["localEpoch"], inst["lid"], inst["dose"])
            if rec is not None and rec["state"] != ST_PENDING:
                continue
            rec = self.find_or_create(inst["localEpoch"], inst["lid"], inst["dose"])
            rec["state"] = ST_MISSED
            rec["actualLocal"] = 0
            rec["deltaMin"] = 0
            out.append(rec)
        return out

    def observed(self, inst, floor_local):
        """Was this instance the simulator's business?

        Anything scheduled before the simulator started was nobody's to watch,
        so it is not counted as due and never declared missed -- but if a lid
        actually opened for it there is a record, and a record outranks the
        floor.
        """
        if inst["localEpoch"] >= int(floor_local):
            return True
        return self.find(inst["localEpoch"], inst["lid"], inst["dose"]) is not None

    def adherence_percent(self, sch, now_local, days, floor_local=0):
        """0..100 over the trailing `days`, counting only instances whose
        window has fully closed, grace included -- a dose still inside its
        grace period has not failed yet and must not drag the number down.
        100 when nothing was due -- nothing to fail."""
        now_local = int(now_local)
        start = now_local - int(days) * SEC_PER_DAY
        closed_by = (int(sch.get("windowAfterMin", 0))
                     + int(sch.get("missedGraceMin", 0))) * SEC_PER_MIN
        due = 0
        taken = 0
        for inst in dose_instances_in_range(sch, start, now_local):
            if inst["localEpoch"] + closed_by >= now_local:
                continue
            if not self.observed(inst, floor_local):
                continue
            rec = self.find(inst["localEpoch"], inst["lid"], inst["dose"])
            if rec is not None and rec["state"] == ST_SKIPPED:
                continue           # a human said it was not needed
            due += 1
            if rec is not None and is_taken_state(rec["state"]):
                taken += 1
        if due == 0:
            return 100
        # Integer half-up, the same arithmetic as ep_adherence.cpp. Python's
        # round() is half-to-even, which disagrees on 12.5 and friends.
        return (taken * 100 + due // 2) // due

    def today_stats(self, sch, now_local, floor_local=0):
        start = day_start(now_local)
        end = start + SEC_PER_DAY
        due = taken = missed = pending = 0
        for inst in dose_instances_in_range(sch, start, end):
            if not self.observed(inst, floor_local):
                continue
            rec = self.find(inst["localEpoch"], inst["lid"], inst["dose"])
            state = rec["state"] if rec is not None else ST_PENDING
            if state == ST_SKIPPED:
                continue
            due += 1
            if is_taken_state(state):
                taken += 1
            elif state == ST_MISSED:
                missed += 1
            else:
                pending += 1
        return {"due": due, "taken": taken, "missed": missed, "pending": pending}

    def lid_state_today(self, sch, lid, now_local, floor_local=0):
        """The one word /api/state puts on a lid for today."""
        if not lid_active_on_day(sch, lid, day_index(now_local)):
            return ST_NONE
        states = []
        for inst in dose_instances_for_lid_on_day(sch, lid, day_index(now_local)):
            if not self.observed(inst, floor_local):
                continue
            rec = self.find(inst["localEpoch"], lid, inst["dose"])
            states.append(rec["state"] if rec is not None else ST_PENDING)
        if not states:
            return ST_NONE
        # Worst news first: a miss outranks a pending dose, which outranks the
        # shades of "it got taken".
        # Same order as web_server.cpp's kStatesWorstFirst, skipped ahead of
        # taken: "somebody decided not to" is more worth surfacing than "done".
        for want in (ST_MISSED, ST_PENDING, ST_LATE, ST_EARLY, ST_SKIPPED, ST_TAKEN):
            if want in states:
                return want
        return ST_PENDING


# ---------------------------------------------------------------------------
# ep_events.h -- the rolling log behind the GUI's activity feed.
# ---------------------------------------------------------------------------

class EventLog:
    def __init__(self):
        self.events = []      # index 0 is the newest

    def push(self, event):
        self.events.insert(0, event)
        del self.events[EVENT_CAPACITY:]

    def clear(self):
        self.events = []

    def size(self):
        return len(self.events)

    def find_latest(self, etype):
        for e in self.events:
            if e["type"] == etype:
                return e
        return None

    def backfill_times(self, now_local, now_uptime_ms):
        """Events logged before the clock was trustworthy carry localEpoch 0.
        Once a sync lands, work their real timestamp back out of their uptime.
        timeVerified stays false: the timestamp is derived, not observed."""
        filled = 0
        for e in self.events:
            if e["localEpoch"]:
                continue
            age_s = max(0, int(now_uptime_ms) - int(e["uptimeMs"])) // 1000
            e["localEpoch"] = max(0, int(now_local) - age_s)
            filled += 1
        return filled


# ---------------------------------------------------------------------------
# The simulated clock.
#
# Two timelines: `true` is the reference wall clock of the simulated world, and
# `device` is what the box believes, which runs fast or slow by drift_ppm until
# a sync snaps it back. Both advance at `scale` x real time, and both move
# together on a jump -- a jump moves the world, not the device's error.
# ---------------------------------------------------------------------------

class SimClock:
    def __init__(self, scale, start_local, drift_ppm=-1840.0, device_local=None):
        self.scale = float(scale)
        self.drift_ppm = float(drift_ppm)
        self._mono = time.monotonic()
        self._true_base = float(start_local)
        self._dev_base = float(start_local if device_local is None else device_local)

    def _elapsed(self):
        return (time.monotonic() - self._mono) * self.scale

    def true_local(self):
        return self._true_base + self._elapsed()

    def device_local(self):
        return self._dev_base + self._elapsed() * (1.0 + self.drift_ppm / 1e6)

    def _rebase(self):
        t, d = self.true_local(), self.device_local()
        self._mono = time.monotonic()
        self._true_base, self._dev_base = t, d

    def set_scale(self, scale):
        self._rebase()
        self.scale = max(0.1, min(3600.0, float(scale)))

    def jump(self, delta_sec):
        self._rebase()
        self._true_base += float(delta_sec)
        self._dev_base += float(delta_sec)

    def set_device_clock(self, local_epoch):
        """What setClock means: move the whole simulated world to that moment,
        keeping whatever error the device currently has."""
        self.jump(float(local_epoch) - self.device_local())

    def set_device_belief(self, local_epoch):
        """Move only what the device thinks the time is, leaving the reference
        clock alone. This is the NVS restore path: a cold boot starts from an
        approximately-right epoch and is wrong until SNTP says otherwise."""
        self._rebase()
        self._dev_base = float(local_epoch)

    def sync(self):
        """Snap the device clock to truth. Returns the correction in seconds
        (positive when the device was running fast)."""
        self._rebase()
        error = self._dev_base - self._true_base
        self._dev_base = self._true_base
        return error


# ---------------------------------------------------------------------------
# Battery. A single-cell LiPo discharge curve, lightly smoothed, pessimistic
# at the top so a freshly-charged pack does not sit at 100 % for an hour.
# ---------------------------------------------------------------------------

LIPO_CURVE = ((3000, 0), (3200, 2), (3300, 6), (3400, 12), (3500, 22),
              (3600, 35), (3700, 48), (3800, 61), (3900, 73), (4000, 85),
              (4100, 94), (4200, 100))


def lipo_percent(mv):
    mv = float(mv)
    if mv <= LIPO_CURVE[0][0]:
        return 0
    if mv >= LIPO_CURVE[-1][0]:
        return 100
    for i in range(1, len(LIPO_CURVE)):
        hi_mv, hi_pct = LIPO_CURVE[i]
        lo_mv, lo_pct = LIPO_CURVE[i - 1]
        if mv <= hi_mv:
            span = hi_mv - lo_mv
            return int(round(lo_pct + (mv - lo_mv) * (hi_pct - lo_pct) / span))
    return 100


# ---------------------------------------------------------------------------
# ep_settings.h validate() plus ep_json.h settingsFromJson(): merge a partial
# object, clamp everything into range, and say whether clamping happened so
# the caller knows to re-save.
# ---------------------------------------------------------------------------

# field -> ("str"|"secret", maxlen) | ("bool",) | ("int", lo, hi) | ("enum", values)
SETTINGS_SPEC = {
    "ui": {"deviceName": ("str", 32), "patientName": ("str", 32),
           "showAdvanced": ("bool",)},
    "wifi": {"ssid": ("str", 33), "pass": ("secret", 65), "hostname": ("str", 32),
             "apFallback": ("bool",), "apPassword": ("secret", 65)},
    "time": {"ntp1": ("str", 48), "ntp2": ("str", 48), "tz": ("str", 64),
             "driftPpm": ("int", -200000, 200000),
             "resyncIntervalSec": ("int", 600, 86400)},
    "notify": {"ntfyEnabled": ("bool",), "ntfyServer": ("str", 64),
               "ntfyTopic": ("str", 48), "ntfyToken": ("secret", 72),
               "pushoverEnabled": ("bool",), "pushoverToken": ("secret", 40),
               "pushoverUser": ("secret", 40),
               "webhookEnabled": ("bool",), "webhookUrl": ("str", 160),
               "onLidOpen": ("bool",), "onDoseTaken": ("bool",),
               "onDoseMissed": ("bool",), "onDoseDue": ("bool",),
               "onUnexpectedOpen": ("bool",), "onLowBattery": ("bool",),
               "onBoot": ("bool",), "onDailySummary": ("bool",),
               "dailySummaryMinute": ("int", 0, 1439),
               "quietFromMinute": ("int", 0, 1439),
               "quietToMinute": ("int", 0, 1439)},
    "power": {"policy": ("enum", SLEEP_POLICIES),
              "awakeAfterEventSec": ("int", 10, 3600),
              "wakeIntervalSec": ("int", 30, 21600),
              "lowBatteryMv": ("int", 3000, 4200),
              "criticalBatteryMv": ("int", 2800, 4200),
              "buzzerOnReminder": ("bool",), "ledOnReminder": ("bool",)},
    "hw": {"hallClosedIsLow": ("bool",), "hallInvertMask": ("int", 0, 127),
           "ledChannelRed": ("int", 0, 7), "ledChannelGreen": ("int", 0, 7),
           "ledChannelBlue": ("int", 0, 7), "ledBrightness": ("int", 0, 255),
           "buzzerChannel": ("int", 0, 7), "buzzerToneHz": ("int", 100, 10000),
           "buzzerSelfDrive": ("bool",),
           "vbatDivNum": ("int", 1, 10000), "vbatDivDen": ("int", 1, 10000),
           "railDivNum": ("int", 1, 10000), "railDivDen": ("int", 1, 10000),
           "buttonDivNum": ("int", 1, 10000), "buttonDivDen": ("int", 1, 10000),
           "buttonThresholdMv": ("int", 0, 5000),
           "statThresholdMv": ("int", 0, 5000),
           "hallDebounceMs": ("int", 0, 5000),
           "buttonDebounceMs": ("int", 0, 5000)},
}

# Every one of these reads back as "__set__" when stored and "" when empty,
# posting "__set__" back leaves the stored value alone and posting "" clears it.
# The Pushover user key is as much a credential as the token,
# and the GUI marks its input data-secret, so it belongs here too.
SECRET_FIELDS = (("wifi", "pass"), ("wifi", "apPassword"),
                 ("notify", "ntfyToken"), ("notify", "pushoverToken"),
                 ("notify", "pushoverUser"))


def _as_bool(value, fallback):
    if isinstance(value, bool):
        return value, False
    if isinstance(value, (int, float)):
        return bool(value), False
    if isinstance(value, str):
        low = value.strip().lower()
        if low in ("true", "1", "yes", "on"):
            return True, False
        if low in ("false", "0", "no", "off"):
            return False, False
    return fallback, True          # unparseable: keep what we had, flag it


def _as_int(value, fallback, lo, hi):
    try:
        n = int(round(float(value)))
    except (TypeError, ValueError):
        return fallback, True
    clamped = max(lo, min(hi, n))
    return clamped, clamped != n


def merge_settings(settings, incoming):
    """Returns (changed_any, clamped_any). Absent fields are left alone."""
    changed = False
    clamped = False
    if not isinstance(incoming, dict):
        return False, True
    for section, fields in SETTINGS_SPEC.items():
        src = incoming.get(section)
        if not isinstance(src, dict):
            continue
        dst = settings[section]
        for name, spec in fields.items():
            if name not in src:
                continue
            value = src[name]
            kind = spec[0]
            if kind in ("str", "secret"):
                if not isinstance(value, str):
                    clamped = True
                    continue
                if kind == "secret" and value == SECRET_PLACEHOLDER:
                    continue       # "leave the stored value alone"
                new = value[:spec[1] - 1]
                if len(value) > len(new):
                    clamped = True
            elif kind == "bool":
                new, bad = _as_bool(value, dst[name])
                clamped = clamped or bad
            elif kind == "int":
                new, bad = _as_int(value, dst[name], spec[1], spec[2])
                clamped = clamped or bad
            else:                  # enum
                if value in spec[1]:
                    new = value
                else:
                    new, clamped = dst[name], True
            if dst[name] != new:
                dst[name] = new
                changed = True

    # hallToLid has to stay a permutation or the hall mapping silently loses a
    # lid; anything else gets repaired to this board's real wiring, not to
    # identity, which on the V0 board is just a different wrong answer.
    hall = incoming.get("hw", {}).get("hallToLid") if isinstance(incoming.get("hw"), dict) else None
    if isinstance(hall, list):
        wanted = [int(x) for x in hall if isinstance(x, (int, float))]
        if len(wanted) == LID_COUNT and sorted(wanted) == list(range(LID_COUNT)):
            if settings["hw"]["hallToLid"] != wanted:
                settings["hw"]["hallToLid"] = wanted
                changed = True
        else:
            settings["hw"]["hallToLid"] = list(DEFAULT_HALL_TO_LID)
            clamped = True
            changed = True

    # Critical has to trip *below* the warning or the box shuts down the instant
    # it warns, so equal values are repaired too, with the firmware's 100 mV gap.
    # lowBatteryMv is already >= 3000 here, so low - 100 cannot fall below 2800.
    if settings["power"]["criticalBatteryMv"] >= settings["power"]["lowBatteryMv"]:
        settings["power"]["criticalBatteryMv"] = settings["power"]["lowBatteryMv"] - 100
        clamped = True
    return changed, clamped


def parse_pills(raw):
    pills = []
    if not isinstance(raw, list):
        return pills
    for item in raw[:MAX_PILLS_PER_DOSE]:
        if not isinstance(item, dict):
            continue
        count, _ = _as_int(item.get("count", 1), 1, 0, 255)
        pills.append({"name": str(item.get("name", ""))[:23],
                      "dose": str(item.get("dose", ""))[:15],
                      "count": count})
    return pills


def parse_dose(raw, fallback=None):
    """time ("HH:MM") wins over minute (0-1439) when both are present."""
    base = fallback or default_dose(8 * 60, "")
    if not isinstance(raw, dict):
        return dict(base)
    minute = base["minute"]
    if "minute" in raw:
        minute, _ = _as_int(raw["minute"], minute, 0, MIN_PER_DAY - 1)
    if "time" in raw:
        parsed = parse_hhmm(raw.get("time"))
        if parsed is not None:
            minute = parsed
    enabled = base["enabled"]
    if "enabled" in raw:
        enabled, _ = _as_bool(raw["enabled"], enabled)
    return {"enabled": enabled, "minute": minute,
            "label": str(raw.get("label", base["label"]))[:15],
            "pills": parse_pills(raw["pills"]) if "pills" in raw else list(base["pills"])}


def parse_schedule(raw, current):
    """POST /api/schedule replaces the schedule wholesale."""
    sch = {
        "mode": raw.get("mode") if raw.get("mode") in LID_MODES else current["mode"],
        "sequentialAnchorDay": current["sequentialAnchorDay"],
        "windowBeforeMin": current["windowBeforeMin"],
        "windowAfterMin": current["windowAfterMin"],
        "missedGraceMin": current["missedGraceMin"],
        "remindBeforeMin": current["remindBeforeMin"],
        "lids": [],
    }
    clamped = False
    for key, lo, hi in (("sequentialAnchorDay", 0, 100000),
                        ("windowBeforeMin", 0, 720),
                        ("windowAfterMin", 0, 720),
                        ("missedGraceMin", 0, 720),
                        ("remindBeforeMin", 0, 720)):
        if key in raw:
            sch[key], bad = _as_int(raw[key], sch[key], lo, hi)
            clamped = clamped or bad

    in_lids = raw.get("lids") if isinstance(raw.get("lids"), list) else []
    by_index = {}
    for entry in in_lids:
        if not isinstance(entry, dict):
            continue
        idx, bad = _as_int(entry.get("lid", len(by_index)), 0, 0, LID_COUNT - 1)
        clamped = clamped or bad
        by_index[idx] = entry

    for lid in range(LID_COUNT):
        entry = by_index.get(lid)
        old = current["lids"][lid]
        if entry is None:
            sch["lids"].append({"name": old["name"], "doses": list(old["doses"])})
            continue
        name = str(entry.get("name", old["name"]))[:15] or default_lid_name(lid, sch["mode"])
        raw_doses = entry.get("doses")
        if not isinstance(raw_doses, list):
            # ep_json.cpp scheduleFromJson: `if (doses.isNull()) continue;`. No
            # doses array means a name-only rename, so this lid's prescription
            # survives untouched. An explicit [] still clears it. The isNull()
            # test is also true for a wrong-typed value, hence isinstance and
            # not a plain "doses" in entry.
            sch["lids"].append({"name": name, "doses": list(old["doses"])})
            continue
        if len(raw_doses) > MAX_DOSES_PER_LID:
            clamped = True
        doses = [parse_dose(d) for d in raw_doses[:MAX_DOSES_PER_LID]]
        sch["lids"].append({"name": name, "doses": doses})
    return sch, clamped


def apply_dose_to_lids(sch, lid_mask, dose_index, dose):
    """ep_settings.cpp applyDoseToLids: 0xFF appends into the first free slot.

    Storage stays in the order the GUI wrote it -- the core sorts only while it
    generates instances, and every DoseRecord is keyed on the storage index, so
    re-sorting here would orphan every record a lid already holds. Writing past
    the end fills the gap with *disabled* placeholders, mirroring the memset,
    because an enabled 00:00 pad is a real timeline instance that would push a
    reminder at midnight and then be swept missed every morning.
    """
    changed = 0
    for lid in range(LID_COUNT):
        if not (lid_mask >> lid) & 1:
            continue
        doses = sch["lids"][lid]["doses"]
        if dose_index == 0xFF:
            if len(doses) >= MAX_DOSES_PER_LID:
                continue
            doses.append(dict(dose))
        else:
            if dose_index >= MAX_DOSES_PER_LID:
                continue            # slot out of range, silently no change
            while len(doses) < dose_index:
                doses.append({"enabled": False, "minute": 0, "label": "",
                              "pills": []})
            if dose_index < len(doses):
                doses[dose_index] = dict(dose)
            else:
                doses.append(dict(dose))
        changed += 1
    return changed


def remove_dose_from_lids(sch, lid_mask, dose_index):
    """ep_settings.cpp removeDoseFromLids: 0xFF is "no particular slot", which
    clears the lid. The HTTP layer refuses that spelling -- op 'clear' is how a
    client asks for it -- but the function keeps it so the two cores agree."""
    changed = 0
    for lid in range(LID_COUNT):
        if not (lid_mask >> lid) & 1:
            continue
        doses = sch["lids"][lid]["doses"]
        if dose_index == 0xFF:
            del doses[:]
            changed += 1
            continue
        if 0 <= dose_index < len(doses):
            doses.pop(dose_index)      # close the gap
            changed += 1
    return changed


# ---------------------------------------------------------------------------
# Notifications. The point of the whole box: it pushes out rather than being
# polled. Wire-identical to notifier.cpp -- ntfy posts the message as the body
# with Title/Priority/Tags headers, Pushover takes a form post, the webhook
# takes a JSON object.
# ---------------------------------------------------------------------------

# event type -> (notify toggle, ntfy priority 1..5, tags)
# Toggle, ntfy priority and tags per event. The priorities and tags are
# notifier.cpp's priorityFor()/tagsFor() copied exactly -- the firmware is the
# reference, and an automation keyed on priority or tags has to behave the same
# whether it is pointed at the box or at this.
NOTIFY_RULES = {
    EV_LID_OPENED: ("onLidOpen", 3, "door"),
    EV_DOSE_TAKEN: ("onDoseTaken", 3, "pill,white_check_mark"),
    EV_DOSE_MISSED: ("onDoseMissed", 4, "pill,x"),
    EV_DOSE_DUE: ("onDoseDue", 4, "alarm_clock,pill"),
    EV_DOSE_REMINDER: ("onDoseDue", 3, "alarm_clock,pill"),
    EV_UNEXPECTED_OPEN: ("onUnexpectedOpen", 4, "warning,door"),
    EV_WRONG_DAY_OPEN: ("onUnexpectedOpen", 4, "warning,calendar"),
    EV_BATTERY_LOW: ("onLowBattery", 4, "battery"),
    EV_BATTERY_CRITICAL: ("onLowBattery", 5, "rotating_light,battery"),
    EV_BOOT: ("onBoot", 2, "arrows_counterclockwise"),
}

PUSHOVER_URL = "https://api.pushover.net/1/messages.json"


def _http_post(url, data, headers, timeout=8.0):
    """Returns (ok, http_code, detail). code 0 means nothing went out."""
    try:
        req = urllib.request.Request(url, data=data, headers=headers, method="POST")
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            code = int(getattr(resp, "status", 0) or resp.getcode())
            resp.read(256)
            return (200 <= code < 300), code, "HTTP %d" % code
    except urllib.error.HTTPError as err:
        body = ""
        try:
            body = err.read(160).decode("utf-8", "replace").strip().replace("\n", " ")
        except Exception:
            pass
        return False, int(err.code), ("HTTP %d %s" % (err.code, body))[:95]
    except urllib.error.URLError as err:
        return False, 0, ("no route: %s" % getattr(err, "reason", err))[:95]
    except Exception as err:                      # socket timeouts, bad URLs
        return False, 0, ("failed: %s" % err)[:95]


class Notifier:
    """Sends in a worker thread so a slow network never blocks an HTTP
    response; notifyTest() is the one synchronous path, because the GUI wants
    the real status code back."""

    def __init__(self, verbose=False):
        self.verbose = verbose
        self._queue = []
        self._lock = threading.Lock()
        self._wake = threading.Event()
        self._stop = False
        self.sent_count = 0
        self.fail_count = 0
        self.last_detail = ""
        self._thread = threading.Thread(target=self._worker, name="notify",
                                        daemon=True)

    def start(self):
        self._thread.start()

    def stop(self):
        self._stop = True
        self._wake.set()

    def queue_empty(self):
        with self._lock:
            return not self._queue

    def enqueue(self, notify, title, message, priority, tags, meta=None):
        with self._lock:
            if len(self._queue) >= 32:
                self._queue.pop(0)     # a backlog that big is already stale
            self._queue.append((dict(notify), title, message, priority, tags,
                                dict(meta or {})))
        self._wake.set()

    def _worker(self):
        while not self._stop:
            self._wake.wait(0.25)
            self._wake.clear()
            while not self._stop:
                with self._lock:
                    if not self._queue:
                        break
                    job = self._queue.pop(0)
                outcomes = self.send(job[0], "all", job[1], job[2], job[3],
                                     job[4], job[5])
                for out in outcomes:
                    if out["ok"]:
                        self.sent_count += 1
                    else:
                        self.fail_count += 1
                    self.last_detail = "%s: %s" % (out["target"], out["detail"])
                    if self.verbose or not out["ok"]:
                        print("[notify] %s -> %s" % (out["target"], out["detail"]),
                              flush=True)
                time.sleep(0.05)       # one push per pass, like notifyService()

    def targets_configured(self, notify):
        out = []
        if notify.get("ntfyEnabled") and notify.get("ntfyTopic"):
            out.append("ntfy")
        if (notify.get("pushoverEnabled") and notify.get("pushoverToken")
                and notify.get("pushoverUser")):
            out.append("pushover")
        if notify.get("webhookEnabled") and notify.get("webhookUrl"):
            out.append("webhook")
        return out

    def send(self, notify, target, title, message, priority, tags, meta=None):
        wanted = self.targets_configured(notify)
        if target != "all":
            wanted = [t for t in wanted if t == target]
            if not wanted:
                return [{"target": target, "ok": False, "httpCode": 0,
                         "detail": "not configured"}]
        results = []
        for name in wanted:
            if name == "ntfy":
                ok, code, detail = self._send_ntfy(notify, title, message, priority, tags)
            elif name == "pushover":
                ok, code, detail = self._send_pushover(notify, title, message, priority)
            else:
                ok, code, detail = self._send_webhook(notify, title, message,
                                                      priority, tags, meta)
            results.append({"target": name, "ok": ok, "httpCode": code,
                            "detail": detail})
        return results

    def _send_ntfy(self, notify, title, message, priority, tags):
        server = (notify.get("ntfyServer") or "https://ntfy.sh").rstrip("/")
        topic = urllib.parse.quote((notify.get("ntfyTopic") or "").strip("/"), safe="")
        if not topic:
            return False, 0, "no topic"
        headers = {"Content-Type": "text/plain; charset=utf-8",
                   "Title": _ascii_header(title),
                   "Priority": str(int(priority))}
        if tags:
            headers["Tags"] = tags
        token = notify.get("ntfyToken") or ""
        if token:
            headers["Authorization"] = "Bearer " + token
        return _http_post("%s/%s" % (server, topic),
                          message.encode("utf-8"), headers)

    def _send_pushover(self, notify, title, message, priority):
        # ntfy's 1..5 does not map onto Pushover's -2..2 one for one; this is
        # the mapping the firmware uses.
        po_priority = {1: -2, 2: -1, 3: 0, 4: 1, 5: 2}.get(int(priority), 0)
        form = urllib.parse.urlencode({
            "token": notify.get("pushoverToken", ""),
            "user": notify.get("pushoverUser", ""),
            "title": title, "message": message, "priority": po_priority,
        }).encode("utf-8")
        return _http_post(PUSHOVER_URL, form,
                          {"Content-Type": "application/x-www-form-urlencoded"})

    def _send_webhook(self, notify, title, message, priority, tags, meta=None):
        # Key for key with sendWebhook() in notifier.cpp, so a consumer -- Home
        # Assistant, n8n, Discord -- cannot tell the simulator from the box
        # except by `source`. `lid` is -1 when the push is not about one
        # compartment, and `event` is "unknown" for a push that is not an event,
        # which is what toString() gives the firmware for those.
        meta = meta or {}
        body = json.dumps({"device": meta.get("device", ""),
                           "patient": meta.get("patient", ""),
                           "title": title,
                           "message": message,
                           "event": meta.get("event") or "unknown",
                           "lid": int(meta.get("lid", -1)),
                           "lidName": meta.get("lidName", ""),
                           "localEpoch": int(meta.get("localEpoch", 0)),
                           "priority": int(priority),
                           "tags": [t for t in (tags or "").split(",") if t],
                           "source": "easypill-sim",
                           "fw": FW_VERSION}).encode("utf-8")
        return _http_post(notify.get("webhookUrl", ""), body,
                          {"Content-Type": "application/json"})


def _ascii_header(text):
    """HTTP headers are latin-1 on the wire and ntfy titles go in a header, so
    anything exotic is transliterated rather than raising."""
    return text.encode("ascii", "replace").decode("ascii")


# ---------------------------------------------------------------------------
# The simulated device: app_state.h, the hardware it would have talked to, and
# the service loop that would have been loop().
# ---------------------------------------------------------------------------

IDLE_DISCHARGE_MV_PER_HOUR = 2.0     # the box is asleep most of the time
CHARGE_MV_PER_HOUR = 300.0           # MCP73831 at 500 mA into a small cell
NOTIFY_COST_MV = 2.0                 # a TLS push costs real charge
REQUEST_COST_MV = 0.02               # a GUI poll costs a little
USB_RAIL_MV = 5020
COIL_RAIL_MV = 4960


class Device:
    def __init__(self, clock, notifier, state_file=None, verbose=False):
        self.lock = threading.RLock()
        self.clock = clock
        self.notifier = notifier
        self.state_file = state_file
        self.verbose = verbose

        self.settings = default_settings()
        self.events = EventLog()
        self.adherence = AdherenceLog()

        self.lid_open = [False] * LID_COUNT
        self.lid_last_open = [0] * LID_COUNT
        self.vbat_mv = 3950.0
        self.usb_present = False
        self.coil_present = False
        self.button_until_mono = 0.0
        self.powered_off = False
        self.stay_awake_until = 0.0
        self.mac = _fake_mac()
        self.rssi = -54
        self.request_count = 0
        self.last_request_mono = 0.0

        # Clock discipline, app_state.h TimeStatus.
        self.time_verified = False
        self.last_sync_utc = 0
        self.last_sync_local = 0
        self.sync_count = 0
        self.measured_drift_ppm = 0
        self.next_sync_due_local = 0
        self.wifi_up = False
        self.boot_true_local = clock.true_local()
        # A dose whose window closed before the simulator started was never
        # the simulator's business, so nothing before this is ever swept,
        # counted as due, or notified about.
        self.floor_local = int(clock.device_local())

        self._last_tick_true = clock.true_local()
        self._fired_due = set()
        self._fired_reminder = set()
        self._last_summary_day = -1
        self._low_battery_latched = False
        self._critical_latched = False
        self._dirty = False
        self._last_save_mono = 0.0
        self._boot_mono_value = time.monotonic()

        # Set by main() once the server and the web directory are known.
        self.web_dir = None
        self.ip_address = "127.0.0.1"
        self.build_date = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
        self.demo_running = False

    # -- time ---------------------------------------------------------------

    def now_local(self):
        return int(self.clock.device_local())

    def now_utc(self):
        return self.now_local() - self.tz_offset_sec()

    def tz_offset_sec(self):
        return host_tz_offset_sec()

    def uptime_ms(self):
        return int(max(0.0, self.clock.true_local() - self.boot_true_local) * 1000.0)

    def clock_usable(self):
        return self.time_verified

    # -- hall sensors -------------------------------------------------------

    def open_mask(self):
        mask = 0
        for lid in range(LID_COUNT):
            if self.lid_open[lid]:
                mask |= 1 << lid
        return mask

    def raw_mask(self):
        """What the seven AH1912 outputs would read, through hallToLid and the
        polarity settings. Magnet present -> LOW, and the magnet sits over the
        sensor while the lid is shut, so closed is low by default."""
        hw = self.settings["hw"]
        mask = 0
        for sensor in range(LID_COUNT):
            lid = hw["hallToLid"][sensor]
            is_open = self.lid_open[lid]
            high = is_open if hw["hallClosedIsLow"] else not is_open
            if (hw["hallInvertMask"] >> sensor) & 1:
                high = not high
            if high:
                mask |= 1 << sensor
        return mask

    def wake_up_high(self):
        # Hardware OR of the seven halls, the button and charger presence.
        return bool(self.open_mask()) or self.button_pressed() or self.charger_present()

    def button_pressed(self):
        return time.monotonic() < self.button_until_mono

    def charger_present(self):
        return self.usb_present or self.coil_present

    def charging(self):
        return self.charger_present() and self.vbat_mv < 4190.0

    def low_battery(self):
        return self.vbat_mv <= self.settings["power"]["lowBatteryMv"]

    # -- events -------------------------------------------------------------

    def log_event(self, etype, lid=NO_LID, detail=0, notify=True):
        """Appends with the right timestamp. localEpoch stays 0 until the clock
        has been checked against NTP: the box never invents a wall-clock time."""
        event = {
            "localEpoch": self.now_local() if self.time_verified else 0,
            "uptimeMs": self.uptime_ms(),
            "type": etype,
            "lid": int(lid),
            "detail": max(-32768, min(32767, int(detail))),
            "timeVerified": self.time_verified,
        }
        self.events.push(event)
        self._dirty = True
        if self.verbose:
            print("[event] %-16s %s" % (etype, self.describe_event(event)), flush=True)
        if notify:
            self._notify_event(event)
        return event

    def lid_name(self, lid):
        if 0 <= lid < LID_COUNT:
            return self.settings["schedule"]["lids"][lid]["name"]
        return "Lid ?"

    def describe_event(self, event):
        """app_state.h describeEvent(): the one-liner the GUI and the pushes
        both use. Derived from type/lid/detail, never stored."""
        etype = event["type"]
        lid = event["lid"]
        detail = event["detail"]
        name = self.lid_name(lid) if lid != NO_LID else ""
        dev = self.settings["ui"]["deviceName"] or "EasyPill"
        if etype == EV_BOOT:
            return "%s started" % dev
        if etype == EV_LID_OPENED:
            return "%s lid opened" % name
        if etype == EV_LID_CLOSED:
            return "%s lid closed" % name
        if etype == EV_DOSE_TAKEN:
            if detail > 0:
                return "%s dose taken %d min late" % (name, detail)
            if detail < 0:
                return "%s dose taken %d min early" % (name, -detail)
            return "%s dose taken on time" % name
        if etype == EV_DOSE_MISSED:
            return "%s dose missed" % name
        if etype == EV_DOSE_DUE:
            return "%s dose due now" % name
        if etype == EV_DOSE_REMINDER:
            return "%s dose due in %d min" % (name, detail)
        if etype == EV_UNEXPECTED_OPEN:
            return "%s lid opened with nothing scheduled" % name
        if etype == EV_WRONG_DAY_OPEN:
            today = ""
            if event["localEpoch"]:
                today = WEEKDAY_NAMES[weekday_mon0(event["localEpoch"])]
            if today:
                return "%s lid opened on %s" % (name, today)
            return "%s lid opened on the wrong day" % name
        if etype == EV_BUTTON_PRESS:
            return "Button pressed"
        if etype == EV_BUTTON_HOLD:
            return "Button held"
        if etype == EV_CHARGER_IN:
            return "Charger connected"
        if etype == EV_CHARGER_OUT:
            return "Charger removed"
        if etype == EV_BATTERY_LOW:
            return "Battery low (%d mV)" % detail
        if etype == EV_BATTERY_CRITICAL:
            return "Battery critical (%d mV)" % detail
        if etype == EV_TIME_SYNCED:
            if detail:
                word = "fast" if detail > 0 else "slow"
                return "Clock synced, device was %d s %s" % (abs(detail), word)
            return "Clock synced"
        if etype == EV_WIFI_UP:
            return "Wi-Fi up (%d dBm)" % detail
        if etype == EV_WIFI_DOWN:
            return "Wi-Fi down"
        if etype == EV_SLEEP_ENTER:
            return "Going to sleep"
        if etype == EV_SLEEP_EXIT:
            return "Woke up"
        if etype == EV_CONFIG_CHANGED:
            return "Settings changed"
        if etype == EV_SENSOR_FAULT:
            return "Mux ground reference did not read low"
        return etype

    def _quiet_now(self):
        notify = self.settings["notify"]
        start = int(notify["quietFromMinute"])
        end = int(notify["quietToMinute"])
        if start == end:
            return False               # equal values = off
        minute = minute_of_day(self.now_local())
        if start < end:
            return start <= minute < end
        return minute >= start or minute < end   # wraps midnight

    def _notify_event(self, event):
        rule = NOTIFY_RULES.get(event["type"])
        if rule is None:
            return False
        toggle, priority, tags = rule
        notify = self.settings["notify"]
        # notifier.cpp exempts BatteryCritical from the per-event toggle and
        # from quiet hours: by then the box is minutes from switching itself
        # off, and a silenced warning is the one nobody can act on.
        critical = event["type"] == EV_BATTERY_CRITICAL
        if not critical and not notify.get(toggle):
            return False
        if not self.notifier.targets_configured(notify):
            return False
        if not critical and self._quiet_now():
            return False
        self.notifier.enqueue(notify, self._push_title(),
                              self._push_message(self.describe_event(event)),
                              priority, tags, self.push_meta(event))
        self.vbat_mv -= NOTIFY_COST_MV
        return True

    def _push_title(self):
        return self.settings["ui"]["deviceName"] or "EasyPill"

    def push_meta(self, event=None):
        """What notifier.cpp puts in a QueuedPush beside the sentence, for the
        webhook body. A push the box would not file as an event -- the test
        push, the daily summary -- carries EventType::Count, whose spelling is
        "unknown", and kNoLid, which the body reports as -1."""
        lid = int(event["lid"]) if event else NO_LID
        at = int(event["localEpoch"]) if event else 0
        return {"device": self._push_title(),
                "patient": self.settings["ui"]["patientName"],
                "event": event["type"] if event else "unknown",
                "lid": lid if lid < LID_COUNT else -1,
                "lidName": self.lid_name(lid) if lid < LID_COUNT else "",
                "localEpoch": at or self.now_local()}

    def _push_message(self, text):
        patient = self.settings["ui"]["patientName"]
        stamp = format_local(self.now_local())[-5:] if self.time_verified else "--:--"
        if patient:
            return "%s - %s (%s)" % (patient, text, stamp)
        return "%s (%s)" % (text, stamp)

    def notify_daily_summary(self):
        """notifier.h notifyDailySummary(): how the day went, once a day."""
        with self.lock:
            sch = self.settings["schedule"]
            now = self.now_local()
            stats = self.adherence.today_stats(sch, now, self.floor_local)
            pct = self.adherence.adherence_percent(sch, now, 7, self.floor_local)
            text = ("today %d of %d doses taken, %d missed, %d still to come; "
                    "7-day adherence %d%%" % (stats["taken"], stats["due"],
                                              stats["missed"], stats["pending"], pct))
            notify = self.settings["notify"]
            if not notify.get("onDailySummary"):
                return False
            if not self.notifier.targets_configured(notify) or self._quiet_now():
                return False
            self.notifier.enqueue(notify, self._push_title(),
                                  self._push_message(text), 3, "clipboard",
                                  self.push_meta())
            return True

    # -- the hardware the GUI pokes through /api/sim ------------------------

    def set_lid(self, lid, is_open):
        """One hall transition, with everything that falls out of it."""
        with self.lock:
            lid = int(lid)
            if not (0 <= lid < LID_COUNT) or self.lid_open[lid] == bool(is_open):
                return
            self.lid_open[lid] = bool(is_open)
            now = self.now_local()
            if not is_open:
                self.log_event(EV_LID_CLOSED, lid)
                return
            self.lid_last_open[lid] = now
            self.log_event(EV_LID_OPENED, lid)
            match = self.adherence.record_open(self.settings["schedule"], lid, now)
            if not match["matched"]:
                self.log_event(EV_UNEXPECTED_OPEN, lid)
            else:
                self.log_event(EV_DOSE_TAKEN, lid, match["deltaMin"])
                # Right time, wrong compartment: the one mistake seven hall
                # sensors are genuinely good at spotting, so it goes out as well
                # as the taken event and never instead of it (main.cpp). The
                # record is kept so history shows what happened, but a foreign
                # lid is not a timeline instance, so it cannot flatter the
                # percentages.
                if match["wrongDay"]:
                    self.log_event(EV_WRONG_DAY_OPEN, lid, match["deltaMin"])
            self._fired_due.discard((match["scheduledLocal"], lid, match["dose"]))

    def press_button(self, ms):
        with self.lock:
            ms = max(1, min(60000, int(ms)))
            self.button_until_mono = time.monotonic() + ms / 1000.0
            hold = ms >= 1200
            self.log_event(EV_BUTTON_HOLD if hold else EV_BUTTON_PRESS, detail=ms)

    def set_vbat(self, mv):
        with self.lock:
            self.vbat_mv = float(max(2500, min(4300, int(mv))))
            self._battery_edges()

    def set_rail(self, which, present):
        with self.lock:
            before = self.charger_present()
            if which == "usb":
                self.usb_present = bool(present)
            else:
                self.coil_present = bool(present)
            after = self.charger_present()
            if after != before:
                self.log_event(EV_CHARGER_IN if after else EV_CHARGER_OUT,
                               detail=USB_RAIL_MV if after else 0)
                if after:
                    # A charger means mains power, which means a radio, which
                    # is the moment to re-check the clock.
                    self._sync_time("charger")

    def sim_reset(self):
        """POST /api/sim {"reset": true}: back to default *hardware*. Settings
        are left alone -- wiping the ntfy topic mid-demo would be a surprise,
        and /api/action factoryReset is the endpoint that resets settings."""
        with self.lock:
            self.lid_open = [False] * LID_COUNT
            self.lid_last_open = [0] * LID_COUNT
            self.vbat_mv = 3950.0
            self.usb_present = False
            self.coil_present = False
            self.button_until_mono = 0.0
            self.powered_off = False
            self.clock.set_scale(1.0)
            self.clock.set_device_clock(host_local_now())
            self.events.clear()
            self.adherence.clear()
            self._fired_due.clear()
            self._fired_reminder.clear()
            self._low_battery_latched = False
            self._critical_latched = False
            self._last_summary_day = -1
            self.floor_local = self.now_local()
            self._last_tick_true = self.clock.true_local()
            self.log_event(EV_BOOT)
            self._dirty = True

    # -- clock discipline ---------------------------------------------------

    def _sync_time(self, reason):
        """What an SNTP answer does. There is no RTC, so this is the only
        thing that makes the clock trustworthy, and the drift it measures is
        what keeps it roughly right in between."""
        before = self.clock.device_local()
        error = self.clock.sync()
        now = int(self.clock.device_local())
        # The drift estimate is only meaningful from the second sync onwards:
        # it is elapsed device seconds measured against elapsed NTP seconds.
        interval = now - self.last_sync_local if self.sync_count else 0
        if self.sync_count >= 1 and interval > 300:
            ppm = int(round(error / float(interval) * 1e6))
            self.measured_drift_ppm = max(-200000, min(200000, ppm))
            self.settings["time"]["driftPpm"] = self.measured_drift_ppm
        self.sync_count += 1
        self.last_sync_local = now
        self.last_sync_utc = now - self.tz_offset_sec()
        self.time_verified = True
        self.next_sync_due_local = now + int(self.settings["time"]["resyncIntervalSec"])
        filled = self.events.backfill_times(now, self.uptime_ms())
        # timekeeper.cpp logs the measured drift in ppm here, not the
        # correction in seconds, so this does the same.
        self.log_event(EV_TIME_SYNCED, detail=int(self.measured_drift_ppm))
        if self.floor_local == 0 or self.floor_local > now:
            self.floor_local = now
        self._dirty = True
        if self.verbose or self.sync_count == 1:
            print("[time] sync (%s): %s, device was %+.1f s, %d event(s) back-filled"
                  % (reason, format_local(now), error, filled), flush=True)
        # Same keys the firmware's syncTime reports, so the GUI renders one
        # shape. lastSyncUtc is the single UTC field and says so in its name.
        return {"synced": True, "before": int(before), "after": now,
                "correctionSec": int(round(error)), "verified": self.time_verified,
                "lastSyncUtc": self.last_sync_utc,
                "driftPpm": self.measured_drift_ppm, "syncCount": self.sync_count}

    def force_sync(self):
        with self.lock:
            if not self.wifi_up:
                self._wifi_connect()
            return self._sync_time("manual")

    def set_credentials(self, ssid, password):
        """POST /api/wifi. A fresh join is a chance to check the clock, which
        on a board with no RTC is the whole timekeeping strategy."""
        with self.lock:
            self.settings["wifi"]["ssid"] = str(ssid)[:32]
            if password != SECRET_PLACEHOLDER:
                self.settings["wifi"]["pass"] = str(password)[:64]
            if self.wifi_up:
                self.wifi_up = False
                self.log_event(EV_WIFI_DOWN, notify=False)
            self._wifi_connect()
            self._sync_time("wifi")
            self.mark_dirty()
            return True

    def jump_clock_to(self, local_epoch):
        with self.lock:
            self.clock.set_device_clock(local_epoch)
            self._after_jump()

    def advance_clock(self, seconds):
        with self.lock:
            self.clock.jump(seconds)
            self._after_jump()

    def _after_jump(self):
        now = self.now_local()
        # A jump is the simulated world moving, so the next resync is still due
        # on the same schedule; a backwards jump just moves the floor with it.
        if self.floor_local > now:
            self.floor_local = now
        if self.next_sync_due_local and self.next_sync_due_local < now - SEC_PER_DAY:
            self.next_sync_due_local = now + int(self.settings["time"]["resyncIntervalSec"])
        self._last_summary_day = day_index(now)
        self._dirty = True

    def reboot(self):
        """What a reset really costs without an RTC: the epoch survives only
        because it was written to NVS, and it is untrusted until SNTP agrees."""
        with self.lock:
            self.save()
            self.time_verified = False
            self.wifi_up = False
            self.sync_count = 0
            self.boot_true_local = self.clock.true_local()
            self._boot_mono_value = time.monotonic()
            self.log_event(EV_BOOT)

    def web_usage(self):
        """Stands in for the LittleFS numbers in /api/info: how big the GUI is."""
        used = 0
        if self.web_dir and os.path.isdir(self.web_dir):
            for name in os.listdir(self.web_dir):
                path = os.path.join(self.web_dir, name)
                if os.path.isfile(path):
                    used += os.path.getsize(path)
        return used, 1441792      # the spiffs partition the firmware reserves

    def _wifi_connect(self):
        self.wifi_up = True
        self.rssi = random.randint(-72, -42)
        self.log_event(EV_WIFI_UP, detail=self.rssi)

    # -- the service loop ---------------------------------------------------

    def service(self):
        """Called a few times a second from the ticker thread. Everything that
        would have happened in loop() happens here, on simulated time."""
        with self.lock:
            now_true = self.clock.true_local()
            dt = now_true - self._last_tick_true
            self._last_tick_true = now_true
            if dt < 0:
                dt = 0.0               # the clock jumped backwards
            now = self.now_local()

            self._service_battery(dt)
            self._service_time(now)
            self._service_schedule(now, dt)
            self._service_summary(now)
            self._maybe_save()

    def _service_battery(self, dt_sim_sec):
        hours = dt_sim_sec / 3600.0
        if self.charger_present():
            if self.vbat_mv < 4200.0:
                self.vbat_mv = min(4200.0, self.vbat_mv + CHARGE_MV_PER_HOUR * hours)
        else:
            self.vbat_mv -= IDLE_DISCHARGE_MV_PER_HOUR * hours
            self.vbat_mv = max(2900.0, self.vbat_mv)
        self._battery_edges()

    def _battery_edges(self):
        power = self.settings["power"]
        if self.vbat_mv <= power["criticalBatteryMv"]:
            if not self._critical_latched:
                self._critical_latched = True
                self.log_event(EV_BATTERY_CRITICAL, detail=int(self.vbat_mv))
                # The real box drops POWER_LATCH here. The simulator keeps
                # serving so you can still look at the GUI, and says so.
                self.powered_off = True
        elif self.vbat_mv > power["criticalBatteryMv"] + 50:
            self._critical_latched = False
            if self.powered_off and self.charger_present():
                self.powered_off = False

        if self.vbat_mv <= power["lowBatteryMv"]:
            if not self._low_battery_latched:
                self._low_battery_latched = True
                self.log_event(EV_BATTERY_LOW, detail=int(self.vbat_mv))
        elif self.vbat_mv > power["lowBatteryMv"] + 50:
            self._low_battery_latched = False

    def _service_time(self, now):
        if not self.wifi_up:
            # Pretend the join takes a couple of seconds of real time.
            if (time.monotonic() - self._boot_mono()) > 1.5:
                self._wifi_connect()
            return
        if not self.time_verified:
            self._sync_time("boot")
            return
        if self.next_sync_due_local and now >= self.next_sync_due_local:
            self._sync_time("interval")

    def _boot_mono(self):
        # Real monotonic, not simulated: the fake Wi-Fi join should take a
        # couple of seconds of wall time whatever the time scale is.
        return self._boot_mono_value

    def _service_schedule(self, now, dt_sim_sec):
        sch = self.settings["schedule"]
        if not self.time_verified:
            return                     # never act on a clock we do not trust

        # Due and reminder events only fire for time the simulation actually
        # lived through. A deliberate jump of hours or days skips them;
        # sweepMissed is what notices what was skipped over.
        if 0 < dt_sim_sec <= 6 * SEC_PER_HOUR:
            window_from = int(now - dt_sim_sec)
            remind = int(sch.get("remindBeforeMin", 0)) * SEC_PER_MIN
            for inst in dose_instances_in_range(sch, max(window_from, self.floor_local),
                                                now + 1):
                key = (inst["localEpoch"], inst["lid"], inst["dose"])
                if key in self._fired_due:
                    continue
                self._fired_due.add(key)
                self.log_event(EV_DOSE_DUE, inst["lid"])
            if remind:
                for inst in dose_instances_in_range(
                        sch, max(window_from + remind, self.floor_local), now + remind + 1):
                    key = (inst["localEpoch"], inst["lid"], inst["dose"])
                    if key in self._fired_reminder or key in self._fired_due:
                        continue
                    self._fired_reminder.add(key)
                    self.log_event(EV_DOSE_REMINDER, inst["lid"],
                                   detail=minute_delta(now, inst["localEpoch"]))

        for rec in self.adherence.sweep_missed(sch, now, 14, self.floor_local):
            self.log_event(EV_DOSE_MISSED, rec["lid"],
                           detail=minute_delta(rec["scheduledLocal"], now))

        if len(self._fired_due) > 512:
            cutoff = now - 30 * SEC_PER_DAY
            self._fired_due = {k for k in self._fired_due if k[0] >= cutoff}
            self._fired_reminder = {k for k in self._fired_reminder if k[0] >= cutoff}

    def _service_summary(self, now):
        if not self.time_verified:
            return
        day = day_index(now)
        minute = minute_of_day(now)
        want = int(self.settings["notify"]["dailySummaryMinute"])
        if day != self._last_summary_day and minute >= want:
            if self._last_summary_day >= 0:        # never on the first pass
                self.notify_daily_summary()
            self._last_summary_day = day

    # -- persistence --------------------------------------------------------

    def mark_dirty(self):
        with self.lock:
            self._dirty = True

    def note_request(self):
        """Every HTTP request is radio time, and radio time is charge."""
        self.request_count += 1
        self.last_request_mono = time.monotonic()
        self.vbat_mv -= REQUEST_COST_MV

    def _maybe_save(self):
        if not self._dirty or not self.state_file:
            return
        if (time.monotonic() - self._last_save_mono) < 2.0:
            return
        self.save()

    def save(self):
        if not self.state_file:
            return
        with self.lock:
            blob = {
                "simVersion": SIM_VERSION,
                "settings": self.settings,
                "events": self.events.events,
                "records": self.adherence.records,
                "lids": self.lid_open,
                "lidLastOpen": self.lid_last_open,
                "vbatMv": round(self.vbat_mv, 1),
                "usb": self.usb_present,
                "coil": self.coil_present,
                "mac": self.mac,
                # The firmware persists its last known epoch to NVS so a cold
                # boot without Wi-Fi starts approximately right, flagged
                # unverified. Same idea here.
                "savedEpoch": self.now_local(),
                "savedAtReal": int(time.time()),
            }
            self._dirty = False
            self._last_save_mono = time.monotonic()
        tmp = self.state_file + ".tmp"
        try:
            with open(tmp, "w") as handle:
                json.dump(blob, handle, indent=1, sort_keys=True)
            os.replace(tmp, self.state_file)
        except OSError as err:
            print("[sim] could not write %s: %s" % (self.state_file, err), flush=True)

    def load(self):
        if not self.state_file or not os.path.exists(self.state_file):
            return False
        try:
            with open(self.state_file) as handle:
                blob = json.load(handle)
        except (OSError, ValueError) as err:
            print("[sim] ignoring unreadable state file (%s)" % err, flush=True)
            return False
        with self.lock:
            stored = blob.get("settings")
            if isinstance(stored, dict):
                merge_settings(self.settings, stored)
                sch = stored.get("schedule")
                if isinstance(sch, dict):
                    parsed, _ = parse_schedule(sch, self.settings["schedule"])
                    self.settings["schedule"] = parsed
            self.events.events = [e for e in blob.get("events", [])
                                  if isinstance(e, dict)][:EVENT_CAPACITY]
            self.adherence.records = [r for r in blob.get("records", [])
                                      if isinstance(r, dict)][-RECORD_CAPACITY:]
            lids = blob.get("lids")
            if isinstance(lids, list) and len(lids) == LID_COUNT:
                self.lid_open = [bool(x) for x in lids]
            last = blob.get("lidLastOpen")
            if isinstance(last, list) and len(last) == LID_COUNT:
                self.lid_last_open = [int(x) for x in last]
            self.vbat_mv = float(blob.get("vbatMv", self.vbat_mv))
            self.usb_present = bool(blob.get("usb", False))
            self.coil_present = bool(blob.get("coil", False))
            self.mac = blob.get("mac") or self.mac
            saved_epoch = int(blob.get("savedEpoch", 0))
            if saved_epoch:
                # Restore it as a *belief*, not as truth: unverified until the
                # first sync, exactly like the NVS path on the real board.
                self.clock.set_device_belief(saved_epoch)
            self.floor_local = self.now_local()
        return True


def host_local_now():
    """Local epoch for the host's wall clock: the simulated world's "real" time."""
    return int(time.time()) + host_tz_offset_sec()


def _fake_mac():
    tail = secrets.token_hex(3).upper()
    return "A4:CF:12:%s:%s:%s" % (tail[0:2], tail[2:4], tail[4:6])


# ---------------------------------------------------------------------------
# The HTTP API, endpoint by endpoint. Field names and enum spellings here are
# the contract: the GUI and the ESP32 server use the same ones, so nothing in
# this section gets renamed for convenience.
# ---------------------------------------------------------------------------

def dose_to_json(dose):
    return {"enabled": bool(dose["enabled"]),
            "time": format_hhmm(dose["minute"]),
            "minute": int(dose["minute"]),
            "label": dose.get("label", ""),
            "pills": [{"name": p.get("name", ""), "dose": p.get("dose", ""),
                       "count": int(p.get("count", 0))}
                      for p in dose.get("pills", [])]}


def event_to_json(dev, event):
    return {"at": int(event["localEpoch"]),
            "uptimeMs": int(event["uptimeMs"]),
            "type": event["type"],
            "lid": int(event["lid"]),
            "detail": int(event["detail"]),
            "timeVerified": bool(event["timeVerified"]),
            "text": dev.describe_event(event)}


def record_to_json(record):
    return {"scheduledAt": int(record["scheduledLocal"]),
            "actualAt": int(record["actualLocal"]),
            "lid": int(record["lid"]),
            "dose": int(record["dose"]),
            "state": record["state"],
            "deltaMin": int(record["deltaMin"])}


def build_state(dev):
    with dev.lock:
        sch = dev.settings["schedule"]
        now = dev.now_local()
        today = day_index(now)

        lids = []
        for lid in range(LID_COUNT):
            nxt = next_dose_for_lid(sch, lid, now)
            lids.append({
                "lid": lid,
                "name": dev.lid_name(lid),
                "open": bool(dev.lid_open[lid]),
                "lastOpenedAt": int(dev.lid_last_open[lid]),
                "activeToday": lid_active_on_day(sch, lid, today),
                "doseCount": enabled_dose_count(sch, lid),
                "todayState": dev.adherence.lid_state_today(sch, lid, now, dev.floor_local),
                "nextAt": int(nxt["localEpoch"]) if nxt else 0,
            })

        nxt = next_dose(sch, now)
        if nxt:
            dose = sch["lids"][nxt["lid"]]["doses"][nxt["dose"]]
            next_block = {
                "valid": True,
                "at": int(nxt["localEpoch"]),
                "inMin": minute_delta(now, nxt["localEpoch"]),
                "lid": nxt["lid"],
                "dose": nxt["dose"],
                "lidName": dev.lid_name(nxt["lid"]),
                "label": dose.get("label", ""),
                "pills": dose_to_json(dose)["pills"],
            }
        else:
            next_block = {"valid": False, "at": 0, "inMin": 0, "lid": 0, "dose": 0,
                          "lidName": "", "label": "", "pills": []}

        stats = dev.adherence.today_stats(sch, now, dev.floor_local)
        ui = dev.settings["ui"]
        wifi = dev.settings["wifi"]
        return {
            "ok": True,
            "now": now,
            "tzOffsetSec": dev.tz_offset_sec(),
            "timeVerified": dev.time_verified,
            # See web_server.cpp: the GUI distinguishes "not taken" from "the
            # box was not watching yet" with this. 0 means no floor.
            "watchingSince": dev.floor_local,
            "uptimeS": dev.uptime_ms() // 1000,
            "fw": FW_VERSION,
            "chip": CHIP_NAME,
            "simulated": True,
            "deviceName": ui["deviceName"],
            "patientName": ui["patientName"],
            "lids": lids,
            "nextDose": next_block,
            "power": {
                "vbatMv": int(round(dev.vbat_mv)),
                "percent": lipo_percent(dev.vbat_mv),
                "usbMv": USB_RAIL_MV if dev.usb_present else 0,
                "coilMv": COIL_RAIL_MV if dev.coil_present else 0,
                "usbPresent": dev.usb_present,
                "coilPresent": dev.coil_present,
                "charging": dev.charging(),
                "buttonPressed": dev.button_pressed(),
                "lowBattery": dev.low_battery(),
            },
            "wifi": {
                "mode": "sta" if dev.wifi_up else "connecting",
                "ssid": wifi["ssid"] or "SimNet",
                "rssi": dev.rssi,
                "ip": dev.ip_address,
                "hostname": wifi["hostname"],
            },
            "adherence": {
                "pct7d": dev.adherence.adherence_percent(sch, now, 7, dev.floor_local),
                "pct30d": dev.adherence.adherence_percent(sch, now, 30, dev.floor_local),
                "dueToday": stats["due"],
                "takenToday": stats["taken"],
                "missedToday": stats["missed"],
                "pendingToday": stats["pending"],
            },
            "sensors": {
                "rawMask": dev.raw_mask(),
                "openMask": dev.open_mask(),
                "gndRefOk": True,
                "wakeUpHigh": dev.wake_up_high(),
            },
            "events": [event_to_json(dev, e) for e in dev.events.events[:12]],
        }


def build_settings(dev):
    with dev.lock:
        out = {"ok": True}
        for section in ("ui", "wifi", "time", "notify", "power", "hw"):
            out[section] = dict(dev.settings[section])
        out["hw"]["hallToLid"] = list(dev.settings["hw"]["hallToLid"])
        for section, field in SECRET_FIELDS:
            out[section][field] = SECRET_PLACEHOLDER if dev.settings[section][field] else ""
        return out


def build_schedule(dev):
    with dev.lock:
        sch = dev.settings["schedule"]
        return {
            "ok": True,
            "mode": sch["mode"],
            "sequentialAnchorDay": int(sch["sequentialAnchorDay"]),
            "windowBeforeMin": int(sch["windowBeforeMin"]),
            "windowAfterMin": int(sch["windowAfterMin"]),
            "missedGraceMin": int(sch["missedGraceMin"]),
            "remindBeforeMin": int(sch["remindBeforeMin"]),
            "lids": [{"lid": lid, "name": sch["lids"][lid]["name"],
                      "doses": [dose_to_json(d) for d in sch["lids"][lid]["doses"]]}
                     for lid in range(LID_COUNT)],
        }


def build_events(dev, limit=64, since=0):
    with dev.lock:
        limit = max(1, min(EVENT_CAPACITY, int(limit)))
        out = []
        for event in dev.events.events:
            if since and event["localEpoch"] <= since:
                continue
            out.append(event_to_json(dev, event))
            if len(out) >= limit:
                break
        return {"ok": True, "events": out}


def build_history(dev, days=14):
    with dev.lock:
        days = max(1, min(60, int(days)))
        cutoff = dev.now_local() - days * SEC_PER_DAY
        records = [record_to_json(r) for r in dev.adherence.records
                   if r["scheduledLocal"] >= cutoff]
        return {"ok": True, "records": records}


def build_info(dev):
    with dev.lock:
        web_used, web_total = dev.web_usage()
        return {
            "ok": True,
            "fw": FW_VERSION,
            "buildDate": dev.build_date,
            "chip": CHIP_NAME,
            "cores": 1,
            "flashMb": 4,
            # Plausible, and it moves, so the GUI footer has something to show.
            "heapFree": 196608 - (dev.events.size() * 96) - random.randint(0, 2048),
            "heapMin": 162000 - (dev.adherence.size() * 24),
            "macAddress": dev.mac,
            "resetReason": "poweron",
            "wakeReason": "powerOn",
            "sdkVersion": "sim-%s (python %d.%d.%d)" % (
                SIM_VERSION, sys.version_info[0], sys.version_info[1],
                sys.version_info[2]),
            "fsUsedBytes": web_used,
            "fsTotalBytes": web_total,
            "simulated": True,
            "uptimeS": dev.uptime_ms() // 1000,
            "timeScale": dev.clock.scale,
        }


FAKE_NETWORKS = (("Home", -54, True), ("Home-5G", -67, True),
                 ("Grandma-WiFi", -72, True), ("TelekomSR-A1B2", -81, True),
                 ("GuestNet", -78, False))


def build_scan(dev):
    nets = [{"ssid": ssid, "rssi": rssi + random.randint(-3, 3), "secure": secure}
            for ssid, rssi, secure in FAKE_NETWORKS]
    with dev.lock:
        stored = dev.settings["wifi"]["ssid"]
    if stored and stored not in [n["ssid"] for n in nets]:
        nets.insert(0, {"ssid": stored, "rssi": dev.rssi, "secure": True})
    nets.sort(key=lambda n: -n["rssi"])
    return {"ok": True, "networks": nets}


def build_sim(dev):
    """The simulator's own view of the fake hardware."""
    with dev.lock:
        return {
            "lids": list(dev.lid_open),
            "openMask": dev.open_mask(),
            "rawMask": dev.raw_mask(),
            "wakeUpHigh": dev.wake_up_high(),
            "vbatMv": int(round(dev.vbat_mv)),
            "percent": lipo_percent(dev.vbat_mv),
            "usb": dev.usb_present,
            "coil": dev.coil_present,
            "charging": dev.charging(),
            "buttonPressed": dev.button_pressed(),
            "poweredOff": dev.powered_off,
            "timeScale": dev.clock.scale,
            "now": dev.now_local(),
            "trueNow": int(dev.clock.true_local()),
            "timeVerified": dev.time_verified,
            "driftPpm": dev.measured_drift_ppm,
            "clockErrorSec": int(dev.clock.device_local() - dev.clock.true_local()),
            "syncCount": dev.sync_count,
            "uptimeS": dev.uptime_ms() // 1000,
            "floorLocal": dev.floor_local,
            "notifyTargets": dev.notifier.targets_configured(dev.settings["notify"]),
            "pushesSent": dev.notifier.sent_count,
            "pushesFailed": dev.notifier.fail_count,
            "demoRunning": dev.demo_running,
        }


def handle_sim(dev, body):
    """POST /api/sim. The firmware 404s this; here it is the hardware."""
    applied = []
    with dev.lock:
        if body.get("reset"):
            dev.sim_reset()
            applied.append("reset")
        for field, opening in (("openLid", True), ("closeLid", False)):
            if field in body:
                lid, bad = _as_int(body[field], 0, 0, LID_COUNT - 1)
                if bad:
                    return False, "%s must be 0..6" % field, applied
                dev.set_lid(lid, opening)
                applied.append("%s=%d" % (field, lid))
        if "toggleLid" in body:
            lid, bad = _as_int(body["toggleLid"], 0, 0, LID_COUNT - 1)
            if bad:
                return False, "toggleLid must be 0..6", applied
            dev.set_lid(lid, not dev.lid_open[lid])
            applied.append("toggleLid=%d" % lid)
        if "pressButton" in body:
            # Either {"pressButton": 800} or {"pressButton": true, "ms": 800}.
            raw = body["pressButton"]
            if isinstance(raw, bool):
                ms, _ = _as_int(body.get("ms", 200), 200, 1, 60000)
            else:
                ms, _ = _as_int(raw, 200, 1, 60000)
            dev.press_button(ms)
            applied.append("pressButton=%dms" % ms)
        if "setVbatMv" in body:
            mv, _ = _as_int(body["setVbatMv"], int(dev.vbat_mv), 2500, 4300)
            dev.set_vbat(mv)
            applied.append("setVbatMv=%d" % mv)
        if "setUsb" in body:
            value, _ = _as_bool(body["setUsb"], dev.usb_present)
            dev.set_rail("usb", value)
            applied.append("setUsb=%s" % value)
        if "setCoil" in body:
            value, _ = _as_bool(body["setCoil"], dev.coil_present)
            dev.set_rail("coil", value)
            applied.append("setCoil=%s" % value)
        if "timeScale" in body:
            try:
                scale = float(body["timeScale"])
            except (TypeError, ValueError):
                return False, "timeScale must be a number", applied
            dev.clock.set_scale(scale)
            applied.append("timeScale=%g" % dev.clock.scale)
        if "setClock" in body:
            try:
                target = int(float(body["setClock"]))
            except (TypeError, ValueError):
                return False, "setClock must be a local epoch", applied
            dev.jump_clock_to(target)
            applied.append("setClock=%d" % target)
        if "advanceMin" in body:
            try:
                mins = float(body["advanceMin"])
            except (TypeError, ValueError):
                return False, "advanceMin must be a number", applied
            mins = max(-525600.0, min(525600.0, mins))
            dev.advance_clock(mins * SEC_PER_MIN)
            applied.append("advanceMin=%g" % mins)
        dev.mark_dirty()
    return True, None, applied


def handle_action(dev, body):
    """POST /api/action. Returns (status_code, response_dict)."""
    action = body.get("action")
    if not isinstance(action, str) or not action:
        return 400, {"ok": False, "error": "action is required"}

    if action == "beep":
        on_ms, _ = _as_int(body.get("onMs", 120), 120, 10, 5000)
        repeats, _ = _as_int(body.get("repeats", 1), 1, 1, 20)
        with dev.lock:
            tone = dev.settings["hw"]["buzzerToneHz"]
        print("[sim] buzzer: %d x %d ms at %d Hz (mux ch %d)"
              % (repeats, on_ms, tone, dev.settings["hw"]["buzzerChannel"]), flush=True)
        return 200, {"ok": True, "result": {"onMs": on_ms, "repeats": repeats,
                                            "toneHz": tone}}

    if action == "led":
        rgb = []
        for key in ("r", "g", "b"):
            value, _ = _as_int(body.get(key, 0), 0, 0, 255)
            rgb.append(value)
        ms, _ = _as_int(body.get("ms", 500), 500, 0, 60000)
        print("[sim] led: r=%d g=%d b=%d for %d ms" % (rgb[0], rgb[1], rgb[2], ms),
              flush=True)
        return 200, {"ok": True, "result": {"r": rgb[0], "g": rgb[1], "b": rgb[2],
                                            "ms": ms}}

    if action == "identifyLed":
        # The real thing drives one channel at a time for 1.5 s. Reporting the
        # order without blocking the handler for 4.5 s is the honest trade.
        with dev.lock:
            hw = dev.settings["hw"]
            mapping = {"red": hw["ledChannelRed"], "green": hw["ledChannelGreen"],
                       "blue": hw["ledChannelBlue"]}
        print("[sim] identifyLed: ch0, ch1, ch2 for 1.5 s each", flush=True)
        return 200, {"ok": True, "result": {"channels": [0, 1, 2], "msEach": 1500,
                                            "configured": mapping}}

    if action == "testNotify":
        target = body.get("target", "all")
        if target not in ("ntfy", "pushover", "webhook", "all"):
            return 400, {"ok": False, "error": "target must be ntfy|pushover|webhook|all"}
        with dev.lock:
            notify = dict(dev.settings["notify"])
            title = dev._push_title()
            message = dev._push_message("test notification from the simulator")
            meta = dev.push_meta()
        # Synchronous on purpose: the GUI wants the real status code.
        outcomes = dev.notifier.send(notify, target, title, message, 3, "test_tube",
                                     meta)
        # "ok" is the verdict on the REQUEST, not on the push: a bad target is
        # the 400 above, and anything that reached a notifier is a successful
        # answer whose payload reports what happened. app.js rejects any body
        # with ok:false and keeps only j.error, so reporting a failed push that
        # way throws httpCode and detail away and prints "the box refused that".
        # notifyTest() reports it in result.sent, so this matches key for key:
        # sent ORs across the targets and httpCode is last-attempted-wins.
        sent = any(o["ok"] for o in outcomes)
        code = outcomes[-1]["httpCode"] if outcomes else 0
        detail = "; ".join("%s %s" % (o["target"], o["detail"]) for o in outcomes)
        with dev.lock:
            dev.vbat_mv -= NOTIFY_COST_MV * max(1, len(outcomes))
        return 200, {"ok": True, "result": {"target": target, "sent": sent,
                                            "httpCode": code,
                                            "detail": detail or "nothing configured",
                                            "results": outcomes}}

    if action == "syncTime":
        return 200, {"ok": True, "result": dev.force_sync()}

    if action in ("markTaken", "markSkipped"):
        scheduled, bad = _as_int(body.get("scheduledAt", 0), 0, 0, 4102444800)
        lid, bad_lid = _as_int(body.get("lid", 0), 0, 0, LID_COUNT - 1)
        dose, bad_dose = _as_int(body.get("dose", 0), 0, 0, MAX_DOSES_PER_LID - 1)
        if bad or bad_lid or bad_dose or scheduled == 0:
            return 400, {"ok": False,
                         "error": "scheduledAt, lid and dose are required"}
        state = ST_TAKEN if action == "markTaken" else ST_SKIPPED
        with dev.lock:
            now = dev.now_local()
            dev.adherence.mark_state(scheduled, lid, dose, state, now)
            record = dev.adherence.find(scheduled, lid, dose)
            # A human override is taken at its word: the state asked for is the
            # state stored, however late it is. Only a real open gets graded.
            if state == ST_TAKEN:
                dev.log_event(EV_DOSE_TAKEN, lid, record["deltaMin"])
            dev.mark_dirty()
            return 200, {"ok": True, "result": record_to_json(record)}

    if action == "clearEvents":
        with dev.lock:
            dev.events.clear()
            dev.mark_dirty()
        return 200, {"ok": True, "result": {"cleared": "events"}}

    if action == "clearHistory":
        with dev.lock:
            dev.adherence.clear()
            dev.mark_dirty()
        return 200, {"ok": True, "result": {"cleared": "history"}}

    if action == "factoryReset":
        if not body.get("confirm"):
            return 400, {"ok": False, "error": "factoryReset needs confirm: true"}
        with dev.lock:
            dev.settings = default_settings()
            dev.events.clear()
            dev.adherence.clear()
            dev.sim_reset()
            dev.mark_dirty()
        return 200, {"ok": True, "result": {"reset": True, "rebooting": True}}

    if action == "reboot":
        dev.reboot()
        return 200, {"ok": True, "result": {"rebooting": True}}

    if action == "powerOff":
        with dev.lock:
            dev.powered_off = True
            dev.log_event(EV_SLEEP_ENTER, notify=False)
        print("[sim] POWER_LATCH dropped. A real box would be dark now; the "
              "simulator keeps serving.", flush=True)
        return 200, {"ok": True, "result": {"latched": False}}

    if action == "calibrateHalls":
        with dev.lock:
            # The firmware reports the raw sensor mask here, not the open mask,
            # because that is the datum the operator checks against: sensors
            # that disagree are a lid left open or a dead sensor, never a
            # polarity to learn. Same wording and same {"force": true} escape.
            raw = dev.raw_mask()
            highs = bin(raw & 0x7F).count("1")
            mixed = highs not in (0, LID_COUNT)
            if mixed and not body.get("force"):
                return 400, {"ok": False,
                             "error": "close every lid first (sensors disagree, "
                                      "raw mask 0x%02X)" % raw}
            dev.settings["hw"]["hallClosedIsLow"] = (highs * 2) < LID_COUNT
            dev.settings["hw"]["hallInvertMask"] = 0
            dev.log_event(EV_CONFIG_CHANGED, notify=False)
            dev.mark_dirty()
            return 200, {"ok": True, "result": {
                "rawMask": raw,
                "hallClosedIsLow": dev.settings["hw"]["hallClosedIsLow"],
                "hallInvertMask": 0,
                "gndRefOk": True}}

    if action == "stayAwake":
        seconds, _ = _as_int(body.get("seconds", 300), 300, 1, 86400)
        with dev.lock:
            dev.stay_awake_until = dev.now_local() + seconds
            return 200, {"ok": True, "result": {"untilS": seconds,
                                                "untilAt": dev.stay_awake_until}}

    return 400, {"ok": False, "error": "unknown action: %s" % action}


def handle_settings_post(dev, body):
    with dev.lock:
        changed, clamped = merge_settings(dev.settings, body)
        if isinstance(body.get("schedule"), dict):
            sch, sch_clamped = parse_schedule(body["schedule"], dev.settings["schedule"])
            dev.settings["schedule"] = sch
            changed = True
            clamped = clamped or sch_clamped
        if changed:
            dev.log_event(EV_CONFIG_CHANGED, notify=False)
            dev.mark_dirty()
        out = build_settings(dev)
        out["changed"] = bool(clamped)
        return out


def handle_schedule_post(dev, body):
    with dev.lock:
        sch, clamped = parse_schedule(body, dev.settings["schedule"])
        dev.settings["schedule"] = sch
        dev.log_event(EV_CONFIG_CHANGED, notify=False)
        dev.mark_dirty()
        out = build_schedule(dev)
        out["changed"] = bool(clamped)
        return out


def handle_bulk_post(dev, body):
    # Same defaults and the same 400 strings as handleScheduleBulk() in
    # web_server.cpp: reject rather than mask, because a client that means 255
    # does not mean 127, and an absent doseIndex must never overwrite slot 0.
    mask, bad_mask = _as_int(body.get("lidMask"), -1, -1, 127)
    if bad_mask or mask < 0:
        return 400, {"ok": False, "error": "lidMask must be 0..127"}
    index, bad_index = _as_int(body.get("doseIndex", 0xFF), 0xFF, 0, 255)
    if bad_index or (index != 0xFF and index >= MAX_DOSES_PER_LID):
        return 400, {"ok": False,
                     "error": "doseIndex must be 0..%d or 255" % (MAX_DOSES_PER_LID - 1)}
    op = body.get("op")
    if op not in ("set", "remove", "clear"):
        return 400, {"ok": False, "error": "op must be set, remove or clear"}
    # 255 is "no particular slot", which only means something for op 'set'.
    # removeDoseFromLids reads it as "clear the lid", so letting it through here
    # would wipe the whole week from the control the GUI leaves on its default.
    if op == "remove" and index >= MAX_DOSES_PER_LID:
        return 400, {"ok": False,
                     "error": "op 'remove' needs doseIndex 0..%d; use op 'clear' "
                              "to wipe a lid" % (MAX_DOSES_PER_LID - 1)}
    with dev.lock:
        sch = dev.settings["schedule"]
        if op == "clear":
            changed = 0
            for lid in range(LID_COUNT):
                if (mask >> lid) & 1:
                    sch["lids"][lid]["doses"] = []
                    changed += 1
        elif op == "remove":
            changed = remove_dose_from_lids(sch, mask, index)
        else:
            if not isinstance(body.get("dose"), dict):
                return 400, {"ok": False, "error": "op set needs a dose object"}
            changed = apply_dose_to_lids(sch, mask, index, parse_dose(body["dose"]))
        if changed:
            dev.log_event(EV_CONFIG_CHANGED, notify=False)
            dev.mark_dirty()
        return 200, {"ok": True, "changed": changed}


# ---------------------------------------------------------------------------
# The HTTP server. Serves the real GUI out of web/ -- the same four files the
# firmware embeds -- so what the browser shows is what the device will show.
# ---------------------------------------------------------------------------

CONTENT_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "application/javascript; charset=utf-8",
    ".json": "application/json",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".ico": "image/x-icon",
    ".woff2": "font/woff2",
    ".txt": "text/plain; charset=utf-8",
    ".map": "application/json",
}

PLACEHOLDER_PAGE = """<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>EasyPill simulator</title>
<style>
 :root{color-scheme:light dark}
 body{font:15px/1.5 system-ui,sans-serif;margin:0;padding:24px;max-width:46rem}
 h1{font-size:1.3rem;margin:0 0 .2rem}
 .warn{background:#fff3cd;color:#5c4400;border:1px solid #e0cc82;
       padding:.7rem .9rem;border-radius:8px;margin:1rem 0}
 @media (prefers-color-scheme:dark){.warn{background:#3a3214;color:#f3e4b0;
       border-color:#6b5c22}}
 table{border-collapse:collapse;width:100%;margin:.5rem 0 1rem}
 td,th{text-align:left;padding:.3rem .5rem;border-bottom:1px solid #8884}
 button{font:inherit;padding:.3rem .6rem;margin:0 .2rem .4rem 0;border-radius:6px}
 code{background:#8882;padding:0 .25rem;border-radius:3px}
 .muted{opacity:.65}
</style></head><body>
<h1>EasyPill simulator</h1>
<p class="muted">Simulated hardware, real HTTP API. This is not the box.</p>
<div class="warn"><strong>The real GUI is not here yet.</strong><br>
No <code>index.html</code> was found in <code>__WEBDIR__</code>, so this
fallback page is being served instead. Drop the GUI's four files
(<code>index.html</code>, <code>app.css</code>, <code>app.js</code>,
<code>favicon.svg</code>) in there and reload - the simulator reads them from
disk on every request, so no restart is needed.</div>
<h2>Live state</h2>
<table id="lids"><thead><tr><th>Lid</th><th>Today</th><th>State</th>
<th>Next dose</th><th></th></tr></thead><tbody></tbody></table>
<p id="summary" class="muted">loading...</p>
<h2>API</h2>
<p class="muted"><code>/api/state</code> <code>/api/settings</code>
<code>/api/schedule</code> <code>/api/schedule/bulk</code>
<code>/api/events</code> <code>/api/history</code> <code>/api/action</code>
<code>/api/info</code> <code>/api/scan</code> <code>/api/wifi</code>
<code>/api/sim</code></p>
<script>
const fmt = s => s ? new Date(s*1000).toISOString().slice(0,16).replace('T',' ') : '-';
async function post(path, body){
  await fetch(path,{method:'POST',headers:{'Content-Type':'application/json'},
                    body:JSON.stringify(body)});
  tick();
}
async function tick(){
  let s;
  try { s = await (await fetch('/api/state')).json(); } catch(e){ return; }
  const body = document.querySelector('#lids tbody');
  body.innerHTML = '';
  for (const lid of s.lids){
    const tr = document.createElement('tr');
    tr.innerHTML = `<td>${lid.name}</td><td>${lid.activeToday?'yes':''}</td>`+
      `<td>${lid.open?'OPEN':'closed'} / ${lid.todayState}</td>`+
      `<td>${fmt(lid.nextAt)}</td><td></td>`;
    const b = document.createElement('button');
    b.textContent = lid.open ? 'close' : 'open';
    b.onclick = () => post('/api/sim', {toggleLid: lid.lid});
    tr.lastChild.appendChild(b);
    body.appendChild(tr);
  }
  const a = s.adherence, p = s.power;
  document.getElementById('summary').textContent =
    `${fmt(s.now)} (${s.timeVerified?'synced':'clock unverified'}) - next: `+
    `${s.nextDose.valid? s.nextDose.lidName+' '+s.nextDose.label+' in '+s.nextDose.inMin+' min':'nothing scheduled'}`+
    ` - today ${a.takenToday}/${a.dueToday} taken, ${a.missedToday} missed`+
    ` - 7d ${a.pct7d}% - battery ${p.percent}% (${p.vbatMv} mV)`+
    `${p.charging?' charging':''} - uptime ${s.uptimeS}s`;
}
tick(); setInterval(tick, 1000);
</script></body></html>
"""


class EasyPillHandler(http.server.BaseHTTPRequestHandler):
    server_version = "EasyPillSim/" + SIM_VERSION
    protocol_version = "HTTP/1.1"

    # -- plumbing -----------------------------------------------------------

    @property
    def dev(self):
        return self.server.device

    def log_message(self, fmt, *args):
        if self.server.verbose:
            sys.stderr.write("[http] %s %s\n" % (self.address_string(), fmt % args))

    def _send(self, code, payload, ctype, extra=None, allow_gzip=False):
        if isinstance(payload, str):
            payload = payload.encode("utf-8")
        headers = {"Content-Type": ctype, "Cache-Control": "no-store",
                   "Access-Control-Allow-Origin": "*"}
        if extra:
            headers.update(extra)
        if allow_gzip and len(payload) > 256 and \
                "gzip" in (self.headers.get("Accept-Encoding") or ""):
            # The firmware serves the GUI gzipped out of flash; do the same so
            # the browser sees the same bytes it will see from the device.
            payload = gzip.compress(payload, 6)
            headers["Content-Encoding"] = "gzip"
        self.send_response(code)
        for name, value in headers.items():
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        if self.command != "HEAD":
            try:
                self.wfile.write(payload)
            except (BrokenPipeError, ConnectionResetError):
                pass

    def _send_json(self, obj, code=200):
        self._send(code, json.dumps(obj), "application/json")

    def _read_json(self):
        """Returns (body_dict, error_string)."""
        length = self.headers.get("Content-Length")
        try:
            size = int(length or 0)
        except ValueError:
            return None, "bad Content-Length"
        if size > 65536:
            return None, "body too large"
        raw = self.rfile.read(size) if size else b""
        if not raw:
            return {}, None
        try:
            body = json.loads(raw.decode("utf-8"))
        except (ValueError, UnicodeDecodeError) as err:
            return None, "invalid JSON: %s" % err
        if not isinstance(body, dict):
            return None, "expected a JSON object"
        return body, None

    def do_OPTIONS(self):
        self._send(204, b"", "text/plain",
                   {"Access-Control-Allow-Methods": "GET, POST, OPTIONS",
                    "Access-Control-Allow-Headers": "Content-Type"})

    def do_HEAD(self):
        self.do_GET()

    # -- routing ------------------------------------------------------------

    def do_GET(self):
        self.dev.note_request()
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        query = urllib.parse.parse_qs(parsed.query)

        if path == "/api/state":
            return self._send_json(build_state(self.dev))
        if path == "/api/settings":
            return self._send_json(build_settings(self.dev))
        if path == "/api/schedule":
            return self._send_json(build_schedule(self.dev))
        if path == "/api/events":
            limit = _first_int(query, "limit", 64)
            since = _first_int(query, "since", 0)
            return self._send_json(build_events(self.dev, limit, since))
        if path == "/api/history":
            return self._send_json(build_history(self.dev, _first_int(query, "days", 14)))
        if path == "/api/info":
            return self._send_json(build_info(self.dev))
        if path == "/api/scan":
            return self._send_json(build_scan(self.dev))
        if path == "/api/sim":
            return self._send_json({"ok": True, "sim": build_sim(self.dev)})
        if path.startswith("/api/"):
            return self._send_json({"ok": False, "error": "no such endpoint"}, 404)
        return self._serve_static(path)

    def do_POST(self):
        self.dev.note_request()
        path = urllib.parse.urlparse(self.path).path
        if not path.startswith("/api/"):
            return self._send_json({"ok": False, "error": "no such endpoint"}, 404)
        body, err = self._read_json()
        if err:
            return self._send_json({"ok": False, "error": err}, 400)

        if path == "/api/settings":
            return self._send_json(handle_settings_post(self.dev, body))
        if path == "/api/schedule":
            return self._send_json(handle_schedule_post(self.dev, body))
        if path == "/api/schedule/bulk":
            code, out = handle_bulk_post(self.dev, body)
            return self._send_json(out, code)
        if path == "/api/action":
            code, out = handle_action(self.dev, body)
            return self._send_json(out, code)
        if path == "/api/wifi":
            ssid = body.get("ssid")
            if not isinstance(ssid, str) or not ssid:
                return self._send_json({"ok": False, "error": "ssid is required"}, 400)
            self.dev.set_credentials(ssid, body.get("pass", ""))
            return self._send_json({"ok": True, "connecting": True})
        if path == "/api/sim":
            ok, err, applied = handle_sim(self.dev, body)
            if not ok:
                return self._send_json({"ok": False, "error": err}, 400)
            return self._send_json({"ok": True, "applied": applied,
                                    "sim": build_sim(self.dev)})
        return self._send_json({"ok": False, "error": "no such endpoint"}, 404)

    # -- the GUI ------------------------------------------------------------

    def _serve_static(self, path):
        web_dir = self.server.web_dir
        rel = posixpath.normpath(urllib.parse.unquote(path)).lstrip("/")
        if rel in ("", "."):
            rel = "index.html"
        full = os.path.normpath(os.path.join(web_dir, rel))
        # Normalised and re-checked: a request must not climb out of web/.
        if not (full == web_dir or full.startswith(web_dir + os.sep)):
            return self._send(403, "forbidden\n", "text/plain; charset=utf-8")
        if os.path.isfile(full):
            try:
                with open(full, "rb") as handle:
                    data = handle.read()
            except OSError as err:
                return self._send(500, "cannot read file: %s\n" % err,
                                  "text/plain; charset=utf-8")
            ctype = CONTENT_TYPES.get(os.path.splitext(full)[1].lower(),
                                      "application/octet-stream")
            return self._send(200, data, ctype, allow_gzip=True)
        if rel == "index.html":
            page = PLACEHOLDER_PAGE.replace("__WEBDIR__", _html_escape(web_dir))
            return self._send(200, page, "text/html; charset=utf-8", allow_gzip=True)
        if rel == "favicon.svg":
            return self._send(200, FALLBACK_FAVICON, "image/svg+xml", allow_gzip=True)
        return self._send(404, "not found: /%s\n" % rel, "text/plain; charset=utf-8")


FALLBACK_FAVICON = (
    '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32">'
    '<rect width="32" height="32" rx="7" fill="#2f7d6e"/>'
    '<rect x="7" y="9" width="18" height="14" rx="3" fill="#fff"/>'
    '<rect x="7" y="9" width="18" height="4" rx="2" fill="#c9e6df"/></svg>')


def _html_escape(text):
    return (text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))


def _first_int(query, name, fallback):
    values = query.get(name)
    if not values:
        return fallback
    try:
        return int(float(values[0]))
    except (TypeError, ValueError):
        return fallback


class SimServer(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address, handler, device, web_dir, verbose):
        self.device = device
        self.web_dir = web_dir
        self.verbose = verbose
        super().__init__(address, handler)


# ---------------------------------------------------------------------------
# The service thread: what loop() would have been doing.
# ---------------------------------------------------------------------------

def ticker(device, stop_event, period=0.25):
    while not stop_event.is_set():
        try:
            device.service()
        except Exception as err:               # a sim bug must not kill the API
            print("[sim] service error: %s" % err, flush=True)
        stop_event.wait(period)


# ---------------------------------------------------------------------------
# --demo: a week in a couple of minutes, with commentary. Most doses on time,
# one 90 minutes late, one missed outright, one taken from the wrong lid on the
# right day, then the battery goes flat and a charger turns up. Enough to shoot
# a video, and enough to sanity-check the adherence maths without waiting a
# week for it.
# ---------------------------------------------------------------------------

DEMO_MIN_SCALE = 120.0       # 1 real second = 2 simulated minutes

DEMO_PILLS_MORNING = [{"name": "Metformin", "dose": "500 mg", "count": 1},
                      {"name": "Ramipril", "dose": "5 mg", "count": 1}]
DEMO_PILLS_EVENING = [{"name": "Metformin", "dose": "500 mg", "count": 1}]


def _say(text):
    print("  %s" % text, flush=True)


def _stamp(device):
    return format_local(device.now_local())


def _demo_at(device, day_idx, minute, settle=1.6):
    """Move the simulated world to a wall-clock moment and let it breathe, so
    a GUI polling once a second actually shows the step."""
    device.jump_clock_to(local_epoch_for(day_idx, minute))
    time.sleep(settle)


def _demo_open(device, lid, hold=1.4):
    device.set_lid(lid, True)
    time.sleep(hold)
    device.set_lid(lid, False)
    time.sleep(0.6)


def run_demo(device, stop_event):
    device.demo_running = True
    try:
        _run_demo_body(device, stop_event)
    except Exception as err:
        print("[demo] stopped: %s" % err, flush=True)
    finally:
        device.demo_running = False


def _run_demo_body(device, stop_event):
    print("\n--- demo: one week of EasyPill, at %gx ---" % max(DEMO_MIN_SCALE,
                                                               device.clock.scale),
          flush=True)
    device.sim_reset()
    if device.clock.scale < DEMO_MIN_SCALE:
        device.clock.set_scale(DEMO_MIN_SCALE)

    # Monday of the current week, 07:30, so the seven days land on the seven
    # lids in the obvious order.
    today = day_index(host_local_now())
    monday = today - weekday_mon0_from_day(today)
    device.jump_clock_to(local_epoch_for(monday, 7 * 60 + 30))
    device.force_sync()
    device.floor_local = device.now_local()

    # A plausible prescription on every lid, through the same bulk endpoint the
    # GUI's "set this pill on every day" button uses.
    handle_bulk_post(device, {"lidMask": 127, "doseIndex": 0, "op": "set",
                              "dose": {"enabled": True, "time": "08:00",
                                       "label": "Morning",
                                       "pills": DEMO_PILLS_MORNING}})
    handle_bulk_post(device, {"lidMask": 127, "doseIndex": 1, "op": "set",
                              "dose": {"enabled": True, "time": "20:00",
                                       "label": "Evening",
                                       "pills": DEMO_PILLS_EVENING}})
    _say("%s  schedule set: Metformin + Ramipril at 08:00, Metformin at 20:00, "
         "every day" % _stamp(device))
    targets = device.notifier.targets_configured(device.settings["notify"])
    _say("%s  notifications: %s"
         % (_stamp(device), ", ".join(targets) if targets else
            "none configured (nothing will be pushed)"))
    time.sleep(1.5)

    def check():
        if stop_event.is_set():
            raise RuntimeError("interrupted")

    # --- Monday: the way it is supposed to go ---------------------------
    check()
    _demo_at(device, monday, 7 * 60 + 58)
    _say("%s  Monday morning dose comes due" % _stamp(device))
    _demo_at(device, monday, 8 * 60 + 3)
    _demo_open(device, 0)
    _say("%s  Monday lid opened - on time" % _stamp(device))
    _demo_at(device, monday, 20 * 60 + 2)
    _demo_open(device, 0)
    _say("%s  Monday evening dose taken" % _stamp(device))

    # --- Tuesday: one late dose ----------------------------------------
    check()
    _demo_at(device, monday + 1, 8 * 60 + 6)
    _demo_open(device, 1)
    _say("%s  Tuesday morning taken, six minutes late - still on time"
         % _stamp(device))
    _demo_at(device, monday + 1, 19 * 60 + 58)
    _say("%s  Tuesday evening dose due, and nobody comes" % _stamp(device))
    _demo_at(device, monday + 1, 21 * 60 + 30)
    _demo_open(device, 1)
    _say("%s  Tuesday evening taken 90 minutes late - inside the 120 minute "
         "window, so it counts, but it is recorded as late" % _stamp(device))

    # --- Wednesday: a dose nobody ever takes ---------------------------
    check()
    _demo_at(device, monday + 2, 8 * 60 + 4)
    _demo_open(device, 2)
    _say("%s  Wednesday morning taken" % _stamp(device))
    _demo_at(device, monday + 2, 19 * 60 + 58)
    _say("%s  Wednesday evening dose due" % _stamp(device))
    _demo_at(device, monday + 2, 22 * 60 + 20, settle=2.2)
    _say("%s  window closed at 22:00 plus 15 minutes of grace and the lid never "
         "moved: missed" % _stamp(device))

    # --- Thursday: right time, wrong compartment -----------------------
    check()
    _demo_at(device, monday + 3, 8 * 60 + 2)
    _demo_open(device, 4)
    _say("%s  Friday's lid opened on a Thursday - the box cannot see which pill "
         "came out, only which lid moved, so this is the signal that matters"
         % _stamp(device))
    _demo_at(device, monday + 3, 8 * 60 + 12)
    _demo_open(device, 3)
    _say("%s  Thursday's own lid opened too - Thursday is covered"
         % _stamp(device))
    _demo_at(device, monday + 3, 20 * 60 + 1)
    _demo_open(device, 3)
    _say("%s  Thursday evening taken" % _stamp(device))

    # --- Friday: the battery runs down, then a charger turns up --------
    check()
    _demo_at(device, monday + 4, 8 * 60 + 1)
    _demo_open(device, 4)
    _say("%s  Friday morning taken" % _stamp(device))
    device.set_vbat(3480)
    _say("%s  battery down to 3480 mV (%d%%) - below the 3500 mV warning"
         % (_stamp(device), lipo_percent(3480)))
    time.sleep(2.0)
    _demo_at(device, monday + 4, 20 * 60 + 2)
    _demo_open(device, 4)
    _say("%s  Friday evening taken" % _stamp(device))
    device.set_rail("usb", True)
    _say("%s  someone plugs the USB cable in - charging, and the fresh network "
         "connection re-checks the clock" % _stamp(device))
    time.sleep(1.5)
    # An hour of charging, so the recovery is actually visible on the GUI
    # rather than the 25 mV a couple of real seconds would buy.
    device.advance_clock(60 * SEC_PER_MIN)
    time.sleep(1.2)
    with device.lock:
        _say("%s  an hour later: %d mV (%d%%) and climbing"
             % (_stamp(device), int(device.vbat_mv), lipo_percent(device.vbat_mv)))

    # --- the weekend: uneventful, which is the goal ---------------------
    check()
    for offset, lid, label in ((5, 5, "Saturday"), (6, 6, "Sunday")):
        _demo_at(device, monday + offset, 8 * 60 + 3)
        _demo_open(device, lid)
        _demo_at(device, monday + offset, 20 * 60 + 4)
        _demo_open(device, lid)
        _say("%s  %s: both doses taken on time" % (_stamp(device), label))

    _demo_at(device, monday + 7, 7 * 60, settle=2.0)

    state = build_state(device)
    adherence = state["adherence"]
    history = build_history(device, 14)["records"]
    counts = {}
    for record in history:
        counts[record["state"]] = counts.get(record["state"], 0) + 1
    print("\n--- demo over: one simulated week, %s ---" % _stamp(device), flush=True)
    _say("7-day adherence %d%%, 30-day %d%%"
         % (adherence["pct7d"], adherence["pct30d"]))
    _say("dose records: " + ", ".join("%d %s" % (count, state)
                                      for state, count in sorted(counts.items())))
    _say("the GUI is still live; the clock is parked on %s" % _stamp(device))
    _say("POST /api/sim {\"timeScale\": 1} to put it back to real time, or "
         "{\"reset\": true} to start over")
    print(flush=True)


# ---------------------------------------------------------------------------
# Startup
# ---------------------------------------------------------------------------

def print_banner(device, url, web_dir, web_files, state_file, demo):
    notify = device.settings["notify"]
    targets = device.notifier.targets_configured(notify)
    scale = device.clock.scale
    if web_files:
        web_note = "%d file(s) found" % web_files
    else:
        web_note = "empty or missing -> serving a placeholder page"
    print("")
    print("  EasyPill V0 simulator %s - simulated hardware, real API." % SIM_VERSION)
    print("  This is NOT the box. Nothing here proves the firmware works.")
    print("")
    print("    open this     %s" % url)
    print("    GUI files     %s  (%s)" % (web_dir, web_note))
    print("    clock         %s   (device believes; unverified until the first sync)"
          % format_local(device.now_local()))
    print("    time scale    %gx  (%s)"
          % (scale, "real time" if abs(scale - 1.0) < 1e-9 else
             "1 real second = %g simulated minutes" % (scale / 60.0)))
    print("    notifications %s" % (", ".join(targets) if targets else
                                    "not configured - set them in the GUI, or "
                                    "pass --ntfy <topic>"))
    print("    state file    %s" % (state_file or "not persisted (--no-persist)"))
    if demo:
        print("    demo          running: a week of doses, with commentary")
    print("")
    print("  Ctrl-C to stop.")
    print("", flush=True)


def build_arg_parser():
    parser = argparse.ArgumentParser(
        description="EasyPill V0 host simulator: the GUI and the whole "
                    "workflow, with no hardware on the desk.")
    here = os.path.dirname(os.path.abspath(__file__))
    parser.add_argument("--port", type=int, default=8080,
                        help="TCP port to listen on (default 8080)")
    parser.add_argument("--host", default="127.0.0.1",
                        help="address to bind (default 127.0.0.1; use 0.0.0.0 "
                             "to reach it from a phone or an iPad)")
    parser.add_argument("--web", default=os.path.join(here, "..", "web"),
                        help="directory holding the real GUI (default ../web)")
    parser.add_argument("--time-scale", type=float, default=1.0,
                        help="simulated seconds per real second, 0.1..3600 "
                             "(3600 = a week of schedule in three minutes)")
    parser.add_argument("--state-file", default=os.path.join(here, "sim_state.json"),
                        help="where to persist between runs")
    parser.add_argument("--no-persist", action="store_true",
                        help="do not read or write the state file")
    parser.add_argument("--demo", action="store_true",
                        help="drive a scripted week of doses and print a "
                             "running commentary")
    parser.add_argument("--ntfy", metavar="TOPIC", default=None,
                        help="enable ntfy on this topic so pushes actually "
                             "land on a phone or an iPad")
    parser.add_argument("--ntfy-server", metavar="URL", default=None,
                        help="self-hosted ntfy base URL (default https://ntfy.sh)")
    parser.add_argument("--drift-ppm", type=float, default=-1840.0,
                        help="how fast the simulated RC oscillator runs wrong; "
                             "there is no RTC on this board")
    parser.add_argument("--verbose", action="store_true",
                        help="log every event, push and HTTP request")
    return parser


def main(argv=None):
    args = build_arg_parser().parse_args(argv)

    web_dir = os.path.normpath(os.path.abspath(args.web))
    state_file = None if args.no_persist else os.path.abspath(args.state_file)
    scale = max(0.1, min(3600.0, args.time_scale))

    clock = SimClock(scale, host_local_now(), drift_ppm=args.drift_ppm)
    notifier = Notifier(verbose=args.verbose)
    device = Device(clock, notifier, state_file=state_file, verbose=args.verbose)
    device.web_dir = web_dir
    device.ip_address = args.host if _looks_like_ip(args.host) else "127.0.0.1"

    if state_file:
        device.load()
    if args.ntfy:
        device.settings["notify"]["ntfyEnabled"] = True
        device.settings["notify"]["ntfyTopic"] = args.ntfy[:47]
    if args.ntfy_server:
        device.settings["notify"]["ntfyServer"] = args.ntfy_server[:63]

    # POWER_LATCH would have gone high before any of this; the equivalent here
    # is that the device exists at all. Boot is the first thing in the log.
    device.log_event(EV_BOOT)

    try:
        server = SimServer((args.host, args.port), EasyPillHandler, device,
                           web_dir, args.verbose)
    except OSError as err:
        print("cannot listen on %s:%d - %s" % (args.host, args.port, err),
              file=sys.stderr)
        return 2

    shown_host = "127.0.0.1" if args.host in ("0.0.0.0", "::", "") else args.host
    url = "http://%s:%d/" % (shown_host, server.server_address[1])
    web_files = 0
    if os.path.isdir(web_dir):
        web_files = len([n for n in os.listdir(web_dir)
                         if os.path.isfile(os.path.join(web_dir, n))])

    notifier.start()
    stop_event = threading.Event()
    tick_thread = threading.Thread(target=ticker, args=(device, stop_event),
                                   name="tick", daemon=True)
    tick_thread.start()

    print_banner(device, url, web_dir, web_files, state_file, args.demo)

    if args.demo:
        threading.Thread(target=run_demo, args=(device, stop_event),
                         name="demo", daemon=True).start()

    def shutdown(signum, frame):
        raise KeyboardInterrupt

    try:
        signal.signal(signal.SIGTERM, shutdown)
    except (ValueError, OSError):
        pass                      # not the main thread, or no signals here

    try:
        server.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        pass
    finally:
        stop_event.set()
        notifier.stop()
        server.server_close()
        if state_file:
            device.save()
            print("\n  state saved to %s" % state_file, flush=True)
        else:
            print("", flush=True)
        print("  simulator stopped.", flush=True)
    return 0


def _looks_like_ip(text):
    try:
        socket.inet_aton(text)
        return True
    except OSError:
        return False


if __name__ == "__main__":
    sys.exit(main())
