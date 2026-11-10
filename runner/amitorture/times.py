"""Host time per disk and module of a results dir: where a run spends its time.

The runner records, for each disk launch, when the serial bytes arrived
(``serial-times.json``: [host seconds since launch, bytes received so far])
next to the stream itself (``serial.bin``). A record's host time is the arrival
of its first byte. Per disk this gives the time to the first record (the
emulator start, Kickstart, the loader and the probe) and, from each module's
begin and end records, the host seconds the module took.
"""
import bisect
import fnmatch
import json
import os

from . import records


def _arrival(times):
    """Host time at which the byte at stream offset ``off`` had arrived."""
    ts = [t for t, _ in times]
    counts = [n for _, n in times]

    def at(off):
        i = bisect.bisect_right(counts, off)
        return ts[min(i, len(ts) - 1)] if ts else None
    return at


def disk_times(disk_dir):
    """{"disk", "first_record_s", "last_record_s", "modules": {mod: host s}} for one
    raw/<entry>/<disk>/ directory, or None without serial data. A module that
    began but did not end (an abort, a hang) counts up to the last record."""
    sp = os.path.join(disk_dir, "serial.bin")
    tp = os.path.join(disk_dir, "serial-times.json")
    if not (os.path.exists(sp) and os.path.exists(tp)):
        return None
    with open(tp) as fh:
        at = _arrival(json.load(fh))
    with open(sp, "rb") as fh:
        parser = records.parse_bytes(fh.read(), "serial")
    first = last = None
    begun, mods = {}, {}
    for r in parser.records:
        if not r.crc_ok or r.obj is None:
            continue
        t = at(r.offset)
        if t is None:
            continue
        first = t if first is None else first
        last = t
        o = r.obj
        if o.get("t") == "mod" and o.get("ev") == "begin":
            begun[o.get("mod")] = t
        elif o.get("t") == "mod" and o.get("ev") == "end" and o.get("mod") in begun:
            mods[o["mod"]] = mods.get(o["mod"], 0.0) + t - begun.pop(o["mod"])
    for m, t0 in begun.items():
        mods[m] = mods.get(m, 0.0) + (last - t0)
    return {"disk": os.path.basename(os.path.normpath(disk_dir)), "first_record_s": first,
            "last_record_s": last, "modules": {m: round(s, 3) for m, s in mods.items()}}


def run_times(results_dir, entries=None):
    """{entry: [disk_times...]} for a results dir; ``entries``: fnmatch patterns."""
    raw = os.path.join(results_dir, "raw")
    out = {}
    for e in sorted(os.listdir(raw)) if os.path.isdir(raw) else []:
        if entries and not any(fnmatch.fnmatch(e, p) for p in entries):
            continue
        disks = []
        for d in sorted(os.listdir(os.path.join(raw, e))):
            dt = disk_times(os.path.join(raw, e, d))
            if dt:
                disks.append(dt)
        if disks:
            out[e] = disks
    return out


def format_times(times, top=8):
    lines, total = [], {}
    for e, disks in times.items():
        lines.append(e)
        for d in disks:
            mods = sorted(d["modules"].items(), key=lambda kv: -kv[1])
            for m, s in mods:
                total[m] = total.get(m, 0.0) + s
            span = (d["last_record_s"] or 0) - (d["first_record_s"] or 0)
            lines.append(f"  {d['disk']}: first record at {d['first_record_s'] or 0:.0f} s, records over {span:.0f} s; "
                         + ", ".join(f"{m} {s:.0f}" for m, s in mods[:top]))
    if total:
        lines.append("total by module: " + ", ".join(f"{m} {s:.0f} s" for m, s in
                                                     sorted(total.items(), key=lambda kv: -kv[1])))
    return "\n".join(lines)
