"""Compare RTC relationships without comparing launch wall-clock dates.

Raw records and baseline fingerprints are unchanged. Sampling failures and
untestable/pass differences are compared normally; there are no polling
coverage exemptions.
"""
from __future__ import annotations

import copy
import datetime
import re


def _time(value):
    m = re.fullmatch(r"y(\d\d)-(\d\d)-(\d\d) (\d\d):(\d\d):(\d\d) w([0-6])(?: leap([0-3]))?",
                     str(value))
    if not m:
        return None
    y, mo, d, h, mi, s, w, leap = m.groups()
    try:
        dt = datetime.datetime(2000 + int(y), int(mo), int(d), int(h), int(mi), int(s))
    except ValueError:
        return None
    return dt, int(w), None if leap is None else int(leap)


def _digits(value):
    if not re.fullmatch(r"\d{13}", str(value)):
        return None
    s = str(value)
    return _time(f"y{s[11]}{s[10]}-{s[9]}{s[8]}-{s[7]}{s[6]} "
                 f"{s[5]}{s[4]}:{s[3]}{s[2]}:{s[1]}{s[0]} w{s[12]}")


def _clock_observation(value, origin):
    dt, weekday, leap = value
    return {"unchanged": dt == origin[0], "weekday_offset": (weekday - dt.weekday() - 1) % 7,
            "leap": leap}


def normalize(c, all_cases):
    """Compare RTC assertions/relationships, not the launches' calendar dates.

    A deterministic written target is never removed. Its readback is treated
    as host time only when it is demonstrably the contemporaneous running
    clock (within ten seconds of this launch's rtc_stop observation). Other
    readback errors, malformed dates/digits and additional fields stay exact.
    """
    body = copy.deepcopy(c["rec"])
    paths = []
    if c["mod"] != "misc_hw":
        return body, paths
    expect, got = body.get("expect"), body.get("got")
    if not isinstance(expect, dict) or not isinstance(got, dict):
        return body, paths
    name = c["case"]
    if name == "rtc_stop":
        origin, actual = _time(expect.get("time")), _time(got.get("time"))
        if origin and actual and abs((actual[0] - origin[0]).total_seconds()) <= 10:
            expect["time"] = {"source": "launch-relative stopped clock",
                              "weekday_valid": origin[1] == (origin[0].weekday() + 1) % 7,
                              "leap": origin[2]}
            got["time"] = _clock_observation(actual, origin)
            paths.extend(("expect.time", "got.time"))
            if _digits(got.get("digits")) and _digits(got["digits"])[:2] == actual[:2]:
                got["digits"] = "consistent with observed time"
                paths.append("got.digits")
    elif name == "rtc_hold":
        origin = _digits(expect.get("held_later"))
        release = _time(expect.get("after_release"))
        held, actual = _digits(got.get("held_later")), _digits(got.get("after_release"))
        if (origin and release and held and actual
                and abs((held[0] - origin[0]).total_seconds()) <= 10
                and abs((actual[0] - release[0]).total_seconds()) <= 10):
            expect["held_later"] = "launch-relative held clock"
            expect["after_release"] = {"advance_seconds": (release[0] - origin[0]).total_seconds(),
                                       "weekday_valid": release[1] == (release[0].weekday() + 1) % 7}
            got["held_later"] = _clock_observation(held, origin)
            got["after_release"] = _clock_observation(actual, release)
            paths.extend(("expect.held_later", "expect.after_release", "got.held_later", "got.after_release"))
    elif name == "rtc_write_readback":
        stop = all_cases.get("misc_hw.rtc_stop", {}).get("rec", {})
        origin = _time((stop.get("expect") or {}).get("time"))
        actual = _time(got.get("time"))
        target = _time(expect.get("time"))
        if (origin and actual and target and actual[0] != target[0]
                and abs((actual[0] - origin[0]).total_seconds()) <= 10):
            got["time"] = {"source": "contemporaneous running clock, not written target",
                           "weekday_offset": (actual[1] - actual[0].weekday() - 1) % 7,
                           "leap": actual[2]}
            paths.append("got.time")
            if _digits(got.get("digits")) and _digits(got["digits"])[:2] == actual[:2]:
                got["digits"] = "consistent with observed time"
                paths.append("got.digits")
    return body, paths
