"""Case records -> fingerprints (what baselines, parity and narrowing compare).

A case is identified by ``<mod>.<case>`` (CONTRACT §10 rule 5: stable ids).
Its *fingerprint* is the whole case record minus the transport fields (v,
seq, t) and minus every timing field: any key named ``timing`` or ``ticks``
or starting with ``latency``, at any depth. Timing values are kept apart,
by path, so they can be compared with tolerance bands. ``perf`` records are
never part of a fingerprint; they are collected as perf values.

The probe result (hello.probe without the disk build id) is fingerprinted
too, as the pseudo case ``probe.hello``: a JIT twin or a later binary that
sees a different machine is a difference worth reporting.
"""
from __future__ import annotations

import json

TRANSPORT_KEYS = ("v", "seq", "t")
PROBE_KEY = "probe.hello"


def is_timing_key(k):
    k = str(k).lower()
    return k in ("timing", "ticks") or k.startswith("latency")


# Keys that record which of several architecturally allowed results happened
# (smc: self-modifying code without a flush may run the old or the new code).
# Either is correct, so they are not part of the fingerprint; they stay in the
# record (and in the report).
NONDET_KEYS = ("undef_observed", "undef_changed_at")


def is_excluded_key(k):
    """NONDET keys: the exact names above, and by convention any key
    starting with "undef_" (a value the architecture leaves undefined)."""
    k = str(k).lower()
    return k in NONDET_KEYS or k.startswith("undef_")


def is_excluded_path(path):
    """True if any component of a dotted/indexed path is a timing or NONDET
    key (used to re-apply today's rules to records stored earlier)."""
    import re
    for part in re.split(r"[.\[\]]+", str(path)):
        if part and (is_timing_key(part) or is_excluded_key(part)):
            return True
    return False


def strip_timing(obj, path="", excluded=None):
    """Returns (obj without timing and NONDET keys, {path: timing value}).
    NONDET values go to `excluded` ({path: value}) when a dict is given."""
    timing = {}

    def walk(o, p):
        if isinstance(o, dict):
            out = {}
            for k, v in o.items():
                kp = f"{p}.{k}" if p else str(k)
                if is_timing_key(k):
                    timing[kp] = v
                    continue
                if is_excluded_key(k):
                    if excluded is not None:
                        excluded[kp] = v
                    continue
                out[k] = walk(v, kp)
            return out
        if isinstance(o, list):
            return [walk(v, f"{p}[{i}]") for i, v in enumerate(o)]
        return o
    return walk(obj, path), timing


def canonical(obj):
    return json.dumps(obj, sort_keys=True, separators=(",", ":"))


def case_key(rec):
    return f"{rec.get('mod')}.{rec.get('case')}"


def numeric_leaves(v, path=""):
    """{path: number} for every number inside a timing value."""
    out = {}
    if isinstance(v, bool):
        return out
    if isinstance(v, (int, float)):
        out[path] = v
    elif isinstance(v, str):
        try:
            out[path] = int(v, 16) if path.endswith("_hex") else float(v)
        except ValueError:
            pass
    elif isinstance(v, dict):
        for k, x in v.items():
            out.update(numeric_leaves(x, f"{path}.{k}" if path else str(k)))
    elif isinstance(v, list):
        for i, x in enumerate(v):
            out.update(numeric_leaves(x, f"{path}[{i}]"))
    return out


def extract_records(recs):
    """From a list of record dicts (one boot, in seq order) to
    {"cases": {key: {...}}, "perf": {key: {...}}, "modules": {mod: id}}."""
    cases, perf, modules = {}, {}, {}
    seen = {}
    for rec in recs:
        t = rec.get("t")
        if t == "mod" and isinstance(rec.get("id"), int):
            modules[rec.get("mod")] = rec["id"]
        elif t == "case":
            key = case_key(rec)
            seen[key] = seen.get(key, 0) + 1
            if seen[key] > 1:
                key = f"{key}#{seen[key]}"
            body = {k: v for k, v in rec.items() if k not in TRANSPORT_KEYS}
            excl = {}
            fp_obj, timing = strip_timing(body, excluded=excl)
            cases[key] = {"mod": rec.get("mod"), "case": rec.get("case"), "st": rec.get("st"),
                          "fp": canonical(fp_obj), "rec": fp_obj, "timing": timing, "seq": rec.get("seq")}
            if excl:
                cases[key]["excluded"] = excl
        elif t == "perf":
            name = rec.get("name") or "perf"
            key = f"{rec.get('mod')}.{name}"
            seen[key] = seen.get(key, 0) + 1
            if seen[key] > 1:
                key = f"{key}#{seen[key]}"
            ops, ticks = rec.get("ops"), rec.get("ticks")
            val = None
            if isinstance(ticks, (int, float)):
                val = ticks / ops if isinstance(ops, (int, float)) and ops else float(ticks)
            perf[key] = {"mod": rec.get("mod"), "name": name, "ops": ops, "ticks": ticks,
                         "tick_hz": rec.get("tick_hz"), "value": val}
        elif t == "hello":
            probe = dict(rec.get("probe") or {})
            body = {k: v for k, v in rec.items() if k not in TRANSPORT_KEYS + ("build", "probe")}
            body["probe"] = probe
            fp_obj, timing = strip_timing(body)
            cases[PROBE_KEY] = {"mod": "probe", "case": "hello", "st": "pass", "fp": canonical(fp_obj),
                                "rec": fp_obj, "timing": timing, "seq": rec.get("seq"), "pseudo": True}
    return {"cases": cases, "perf": perf, "modules": modules}


def _extract_merged(merged):
    """The last boot of one disk's records (a reboot restarts seq; the last
    boot is the complete one)."""
    if merged is None:
        return {"cases": {}, "perf": {}, "modules": {}}
    boots = [m["boot"] for m in merged.records]
    last = boots[-1] if boots else 0
    return extract_records([m["rec"] for m in merged.records if m["boot"] == last])


def extract(result):
    """Fingerprints of one entry result (runner.run_entry). With a disk set,
    every disk's records are used; cases are keyed (mod, case) as before
    (every module is on exactly one disk); the probe pseudo-case comes from
    the first disk (runner.probe_consistency compares the others)."""
    out = {"status": result.get("status"), "reason": result.get("reason"), "cases": {}, "perf": {}, "modules": {}}
    subs = result.get("disks")
    if not subs:
        out.update(_extract_merged(result.get("_merged")))
        _screenshot_cases(out, result, [(None, result.get("_merged"))])
        return out
    for sub in subs:
        e = _extract_merged(sub.get("_merged"))
        dn = sub.get("disk_name")
        for k, c in e["cases"].items():
            if k == PROBE_KEY and PROBE_KEY in out["cases"]:
                continue
            key = k if k not in out["cases"] else f"{k}@{dn}"
            out["cases"][key] = dict(c, disk=dn)
        for k, p in e["perf"].items():
            out["perf"][k if k not in out["perf"] else f"{k}@{dn}"] = dict(p, disk=dn)
        out["modules"].update(e["modules"])
    _screenshot_cases(out, result, [(sub.get("disk_name"), sub.get("_merged")) for sub in subs])
    return out


SHOT_PREFIX = "host.shot_"


def _screenshot_cases(out, result, merged_by_disk):
    """CONTRACT §13: on cycle-exact entries the hash of every acknowledged
    screenshot is fingerprinted as the pseudo case host.shot_<tag> (the
    picture is then part of baselines and checks). Elsewhere the timing of
    a frame is not exact, so screenshots are kept but not compared."""
    if str((result.get("options") or {}).get("cycle_exact", "false")).lower() != "true":
        return
    for dn, m in merged_by_disk:
        if m is None:
            continue
        for x in m.records:
            r = x.get("rec") or {}
            if not x.get("host") or r.get("what") != "screenshot" or not r.get("acked"):
                continue
            sha = (r.get("result") or {}).get("sha256")
            if not sha:
                continue
            key = SHOT_PREFIX + str(r.get("tag") or r.get("id"))
            if key in out["cases"]:
                key = f"{key}@{dn}" if dn else f"{key}#{r.get('id')}"
            body = {"mod": "host", "case": key[len("host."):], "st": "pass", "sha256": sha}
            out["cases"][key] = {"mod": "host", "case": body["case"], "st": "pass", "fp": canonical(body),
                                 "rec": body, "timing": {}, "seq": r.get("req_seq"), "pseudo": True, "disk": dn}


def diff_paths(a, b, path=""):
    """Where two JSON values differ: [(path, a_value, b_value)], leaves first.
    A missing key shows as the string '<absent>'."""
    if type(a) is not type(b):
        return [(path or "$", a, b)]
    if isinstance(a, dict):
        out = []
        for k in sorted(set(a) | set(b), key=str):
            kp = f"{path}.{k}" if path else str(k)
            if k not in a:
                out.append((kp, "<absent>", b[k]))
            elif k not in b:
                out.append((kp, a[k], "<absent>"))
            else:
                out.extend(diff_paths(a[k], b[k], kp))
        return out
    if isinstance(a, list):
        out = []
        for i in range(max(len(a), len(b))):
            ip = f"{path}[{i}]"
            if i >= len(a):
                out.append((ip, "<absent>", b[i]))
            elif i >= len(b):
                out.append((ip, a[i], "<absent>"))
            else:
                out.extend(diff_paths(a[i], b[i], ip))
        return out
    return [] if a == b else [(path or "$", a, b)]


def module_ids_from_source(repo_root):
    """{module name: id} from the AMT_MODULE lines in disk/modules/*.c, with
    the CONTRACT §10 table as fallback."""
    import os
    import re
    ids = {"takeover": 0, "rt": 1, "cpu_basic": 2, "flags": 3, "exceptions": 4, "smc": 5, "interrupts": 6, "cia": 7,
           "blitter": 8, "fpu": 9, "timing": 10, "devio": 11}
    d = os.path.join(repo_root, "disk", "modules")
    pat = re.compile(r"^AMT_MODULE\(\s*([A-Za-z_]\w*)\s*,\s*(\d+)\s*,")
    if os.path.isdir(d):
        for f in sorted(os.listdir(d)):
            if not f.endswith(".c"):
                continue
            try:
                with open(os.path.join(d, f), encoding="utf-8", errors="replace") as fh:
                    for line in fh:
                        m = pat.match(line)
                        if m:
                            ids[m.group(1)] = int(m.group(2))
            except OSError:
                pass
    return ids
