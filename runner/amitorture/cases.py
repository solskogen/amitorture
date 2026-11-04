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
        _tone_cases(out, [(None, result.get("_merged"))])
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
    _tone_cases(out, [(sub.get("disk_name"), sub.get("_merged")) for sub in subs])
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


TONE_PREFIX = "host.tone_"
TONE_DUR_TOL = 0.2        # the tone's active length may differ this much (fraction) from what the guest played


def tone_name(tag):
    """host.tone_<tag> without a doubled prefix: tag "tone_aud0" -> "tone_aud0"."""
    t = str(tag)
    return "tone_" + (t[5:] if t.startswith("tone_") else t)


def tone_case(h):
    """One host record -> the host.tone_<tag> verdict, or None.

    Only the stop mark of an acknowledged audio_mark whose analysis has an
    expected frequency (the guest described the tone: expect_hz, freq_mhz
    or clock/period/samples) makes one. The tone is measured on the channel
    the guest named (`side`), else on the loudest one: its FFT peak (zero
    crossings if the FFT has none).

    st: "pass" when the measured frequency is within the tolerance (and the
    tone is on the named side); "fail" when it is not, or when that channel
    is silent; "untestable" when there is no audio in the segment, or when a
    mismatch comes from a capture that cannot be trusted: the tone's active
    length is not what the guest played (`ms`, +-20 %). The capture is SDL's
    real-time output, so an emulator slower than real time on a loaded host
    stretches the sound, leaves gaps or repeats old buffers.

    Returns {"st", "fp": fingerprint body, "expect", "got", "why"}; the
    fingerprint holds the measured frequency rounded to the tolerance band:
    expect_hz itself while the tone is within the tolerance, else
    expect_hz + k * 2 * tol_hz, so a baseline compares it stably."""
    if h.get("what") != "audio_mark" or not h.get("acked"):
        return None
    res = h.get("result") or {}
    an = res.get("analysis")
    if not isinstance(an, dict) or not an.get("expect_hz"):
        return None
    try:
        e = float(an["expect_hz"])
        tol_pct = float(an.get("tol_pct") or 3.0)
    except (TypeError, ValueError):
        return None
    tol_hz = e * tol_pct / 100.0
    chans = an.get("channels") or {}
    side = an.get("expect_side")
    ch = side or an.get("loudest") or (sorted(chans)[0] if chans else None)
    c = chans.get(ch) or {}
    src = an.get("source_ch")
    where = f"output {ch}" + (f", source channel {src}" if src is not None else "")
    expect = {"freq_hz": round(e, 3), "tol_pct": tol_pct, "tol_hz": round(tol_hz, 3), "channel": ch}
    if src is not None:
        expect["source_ch"] = src
    if an.get("expect_ms"):
        expect["active_s"] = round(float(an["expect_ms"]) / 1000.0, 3)
    m = c.get("freq_fft_hz") or c.get("freq_zc_hz")
    got = {"freq_hz": m, "freq_fft_hz": c.get("freq_fft_hz"), "freq_zc_hz": c.get("freq_zc_hz"),
           "channel": ch, "active_s": c.get("active_s"), "dropout_fraction": c.get("dropout_fraction"),
           "loudest": an.get("loudest"), "per_channel_hz": {k: (v or {}).get("freq_fft_hz") for k, v in chans.items()}}
    if side:
        got["side_ok"] = an.get("side_ok")
    fp = {"expect_hz": round(e, 3), "tol_pct": tol_pct, "channel": ch}
    if an.get("error") or not chans:
        st, why = "untestable", f"no audio in the capture segment ({an.get('error') or 'no channels'})"
    elif c.get("silent", True) or m is None:
        st, why = "fail", (f"no tone on {where}: the guest played {e:.6g} Hz there"
                           + (" (it came out on the other channel)" if side and not an.get("silent") else ""))
    else:
        dur, want = c.get("active_s"), expect.get("active_s")
        k = int(round((m - e) / (2 * tol_hz))) if tol_hz else 0
        off = (m - e) / e * 100.0
        freq_ok = abs(m - e) <= tol_hz
        side_ok = an.get("side_ok") is not False
        # (side_ok compares the expected frequency's level on both sides:
        # meaningless when the tone is not at that frequency)
        why = (f"{where}: measured {m} Hz, expected {e:.6g} Hz +-{tol_pct:g} % (+-{tol_hz:.3g} Hz), "
               f"off by {off:+.2f} %" + ("" if side_ok or not freq_ok else
                                         f"; the tone is not on {side} alone (side_amp {an.get('side_amp')})"))
        if freq_ok and side_ok:
            st = "pass"
            fp["freq_hz"] = round(e, 3)
        elif want and isinstance(dur, (int, float)) and abs(dur - want) > TONE_DUR_TOL * want:
            st = "untestable"
            why = (f"the capture does not hold the tone as played: {dur} s of sound on {where} for {want} s "
                   f"played (+-{TONE_DUR_TOL:.0%}); SDL records in real time, so an emulator slower than "
                   f"real time stretches or repeats the sound. Not judged: " + why)
        else:
            st = "fail"
            fp["freq_hz"] = round(e + k * 2 * tol_hz, 3)
    fp["st"] = st
    return {"st": st, "fp": fp, "expect": expect, "got": got, "why": why}


def _tone_cases(out, merged_by_disk):
    """CONTRACT §13: every analysed tone with an expected frequency becomes
    the pseudo case host.tone_<tag> on every entry that captured audio (not
    only cycle-exact ones: Paula's and the sound cards' rates come from the
    emulated clocks, measured within 0.02 % on fast, JIT and cycle-exact
    cores, against a 2-3 % tolerance)."""
    for dn, m in merged_by_disk:
        if m is None:
            continue
        for x in m.records:
            r = x.get("rec") or {}
            if not x.get("host"):
                continue
            v = tone_case(r)
            if v is None:
                continue
            key = "host." + tone_name(r.get("tag") or r.get("id"))
            if key in out["cases"]:
                key = f"{key}@{dn}" if dn else f"{key}#{r.get('id')}"
            body = dict({"mod": "host", "case": key[len("host."):]}, **v["fp"])
            out["cases"][key] = {"mod": "host", "case": body["case"], "st": v["st"], "fp": canonical(body),
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
