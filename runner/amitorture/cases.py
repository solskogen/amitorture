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
        _speed_cases(out, [(None, result, result.get("_merged"))])
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
    _speed_cases(out, [(sub.get("disk_name"), sub, sub.get("_merged")) for sub in subs])
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
    # side "both" (a mono source on left and right): measured on the loudest
    # output; side_ok says whether the other one carries it too
    ch = (side if side != "both" else None) or an.get("loudest") or (sorted(chans)[0] if chans else None)
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
                                         (f"; the tone is not on both outputs alike (side_amp {an.get('side_amp')})" if side == "both" else
                                          f"; the tone is not on {side} alone (side_amp {an.get('side_amp')})")))
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


SPEED_PREFIX = "host.speed_"
# The verdict of host.speed_<insn> (docs/cases/hostspeed.md). rtf (real-time
# factor) = host seconds per emulated second while the block ran: the
# emulated frame rate divided by the frame rate the host saw. A block fails
# when the emulator ran more than 1 % slower than real time, beyond the
# measurement noise: rtf > SPEED_RTF_LIMIT + 3 sigma. When 3 sigma itself is
# above SPEED_MAX_NOISE the block is untestable (never fail on noise).
SPEED_RTF_LIMIT = 1.01
SPEED_MAX_NOISE = 0.005
# A real slowdown keeps the emulation thread busy: it fell behind because the
# instruction costs too much host time. If the thread was mostly idle while
# the emulator still lagged (host scheduling, another process), the lag is not
# the instruction's: fail only when busy = CPU s per host s >= SPEED_CPU_BOUND.
SPEED_CPU_BOUND = 0.9
SPEED_MIN_SAMPLES = 3          # samples inside one block (outliers are only seen from 3 on)
SPEED_MIN_SPAN_S = 0.1         # host seconds between the first and the last sample
SPEED_SIGMA_FLOOR_S = 20e-6    # per-sample host clock noise floor
# Amiberry serves IPC between frames and then waits up to 2 ms for a follow-
# up request (osdep/amiberry_ipc_socket.cpp IPC_FOLLOWUP_WAIT) on the
# emulation thread, whose next frame deadline is already set: each sample
# read can add up to that much to a frame that is already at its limit.
SPEED_IPC_WAIT_S = 0.002
# the emulated frame rate: colour clock / (lines x colour clocks per line),
# long frames (PAL 313 x 227, NTSC 263 x 227.5): Amiberry custom.cpp vblank_hz_lof
CCK_HZ = {"PAL": 3546895, "NTSC": 3579545}
FRAME_HZ = {"PAL": 3546895 / (313 * 227), "NTSC": 3579545 / (263 * 227.5)}
SPEED_REFS = {"int": "add_l", "fpu": "fadd"}


def _fit(pts):
    """Least squares t = a + b * frame over [(frame, t)]: (b, residuals, Sxx)."""
    n = len(pts)
    mf = sum(f for f, _ in pts) / n
    mt = sum(t for _, t in pts) / n
    sxx = sum((f - mf) ** 2 for f, _ in pts)
    if sxx <= 0:
        return None, [], 0.0
    b = sum((f - mf) * (t - mt) for f, t in pts) / sxx
    return b, [t - (mt + b * (f - mf)) for f, t in pts], sxx


def _rms(res, dof):
    return (sum(x * x for x in res) / dof) ** 0.5 if dof > 0 else None


def _outlier(pts):
    """(index, residual ms) of one sample far off the line through the others
    (a late IPC reply or a descheduled runner), or None. Leave-one-out: the
    sample whose residual against the fit of the rest is largest, if that is
    over 1 ms and over 8x the RMS residual of the rest."""
    best = None
    for i in range(len(pts)):
        rest = pts[:i] + pts[i + 1:]
        fit, res, sxx = _fit([(d["heartbeat"], d["t"]) for d in rest])
        if fit is None:
            continue
        mf = sum(d["heartbeat"] for d in rest) / len(rest)
        mt = sum(d["t"] for d in rest) / len(rest)
        r = pts[i]["t"] - (mt + fit * (pts[i]["heartbeat"] - mf))
        spread = max(_rms(res, len(rest) - 2) or 0.0, SPEED_SIGMA_FLOOR_S)
        if abs(r) > 0.001 and abs(r) > 8 * spread and (best is None or abs(r) / spread > best[2]):
            best = (i, round(r * 1e3, 3), abs(r) / spread)
    return None if best is None else best[:2]


def speed_blocks(sub, merged):
    """The hostspeed blocks of one disk launch: [{insn, cls, pos, n, timing,
    samples, fps, rtf, sigma, st, why, cpu_*}], in the guest's order, or []
    when the launch ran no hostspeed.

    Per block the runner's samples (runner.EntryRun.speed_sample: host time,
    mailbox heartbeat = frame count, thread CPU ns) taken while the mailbox
    said the block was running are fitted with host time = a + b * frame;
    rtf = b * the emulated frame rate. One sample far off the line (a late
    IPC reply or a descheduled runner) is dropped when 4 or more are left.
    sigma = the per-sample noise (the larger of the block's own residual RMS
    and the run's reference noise, the median residual RMS of the empty,
    add_l and fadd blocks) / sqrt(Sxx) x the frame rate."""
    if merged is None:
        return []
    recs = [x.get("rec") or {} for x in merged.records if not x.get("host")]
    hello = next((r for r in recs if r.get("t") == "hello"), {})
    video = (hello.get("probe") or {}).get("video") or "PAL"
    fhz, cck_hz = FRAME_HZ.get(video, FRAME_HZ["PAL"]), CCK_HZ.get(video, CCK_HZ["PAL"])
    blocks, by_pos = [], {}
    for r in recs:
        if r.get("t") != "speed":
            continue
        ev = r.get("ev")
        if ev == "skip":
            blocks.append({"insn": r.get("insn"), "cls": r.get("cls"), "pos": r.get("pos"), "st": "skip",
                           "why": r.get("why")})
        elif ev == "begin":
            b = {"insn": r.get("insn"), "cls": r.get("cls"), "pos": r.get("pos"), "st": None, "ended": False}
            blocks.append(b)
            by_pos[r.get("pos")] = b
        elif ev == "end" and r.get("pos") in by_pos:
            b = by_pos[r.get("pos")]
            b.update(ended=True, n=r.get("n"), timing=r.get("timing") or {})
    if not blocks:
        return []
    hs = (sub or {}).get("hostspeed") or {}
    fields = hs.get("fields") or ["t", "heartbeat", "case", "cpu_main_ns", "cpu_total_ns", "cpu_top_ns"]
    samples = {}
    for s in hs.get("samples") or []:
        d = dict(zip(fields, s))
        lst = samples.setdefault(d.get("case"), [])
        if not lst or lst[-1]["heartbeat"] != d.get("heartbeat"):   # one per frame
            lst.append(d)
    opts = (sub or {}).get("options") or {}
    threaded = str(opts.get("cpu_threaded", "false")).lower() == "true"

    def emu_cpu(d):
        if d.get("cpu_main_ns") is None:
            return None
        return d["cpu_main_ns"] + ((d.get("cpu_top_ns") or 0) if threaded else 0)

    # 1. per block: fit, own residual noise
    for b in blocks:
        if b["st"] == "skip":
            continue
        pts = [d for d in samples.get(b["pos"], []) if isinstance(d.get("t"), (int, float))]
        b["samples"] = len(pts)
        b["_pts"] = pts
        if len(pts) >= 2:
            fit, res, sxx = _fit([(d["heartbeat"], d["t"]) for d in pts])
            drop = _outlier(pts) if fit is not None and len(pts) >= 4 else None
            if drop is not None:
                i, r_ms = drop
                b["dropped"] = {"t": pts[i]["t"], "heartbeat": pts[i]["heartbeat"], "residual_ms": r_ms}
                pts = pts[:i] + pts[i + 1:]
                b["_pts"] = pts
                fit, res, sxx = _fit([(d["heartbeat"], d["t"]) for d in pts])
            b["_fit"], b["_sxx"] = fit, sxx
            b["_own"] = _rms(res, len(pts) - 2)
    refs = sorted(b["_own"] for b in blocks if b.get("_own") is not None
                  and (b["insn"] in SPEED_REFS.values() or str(b["insn"]).startswith("empty")))
    ref_noise = max(refs[len(refs) // 2], SPEED_SIGMA_FLOOR_S) if refs else None
    # 2. rates and verdicts
    for b in blocks:
        if b["st"] == "skip":
            continue
        pts, fit = b.pop("_pts", []), b.pop("_fit", None)
        sxx, own = b.pop("_sxx", 0.0), b.pop("_own", None)
        b["ref_noise_ms"] = None if ref_noise is None else round(ref_noise * 1e3, 4)
        if not b.get("ended"):
            b.update(st="untestable", why="the block did not finish (no end record)")
            continue
        if not hs.get("samples"):
            b.update(st="untestable", why="no host samples of the frame counter: the runner did not read the "
                                          "mailbox during the module (mailbox off or not found, or not this runner)")
            continue
        span = (pts[-1]["t"] - pts[0]["t"]) if len(pts) >= 2 else 0.0
        b["span_s"] = round(span, 3)
        if fit is None or len(pts) < SPEED_MIN_SAMPLES or span < SPEED_MIN_SPAN_S:
            b.update(st="untestable", why=f"{len(pts)} host sample(s) over {span:.3f} s inside the block: need "
                                          f"{SPEED_MIN_SAMPLES} over {SPEED_MIN_SPAN_S} s")
            continue
        noise = max(x for x in (own, ref_noise, SPEED_SIGMA_FLOOR_S) if x is not None)
        sigma = noise / sxx ** 0.5 * fhz
        rtf = fit * fhz
        polls = len(pts) - 1
        b.update(fps=round(1.0 / fit, 4), frame_hz=round(fhz, 4), rtf=round(rtf, 5), sigma=round(sigma, 6),
                 own_noise_ms=None if own is None else round(own * 1e3, 4),
                 ipc_bound=round(polls * SPEED_IPC_WAIT_S / span, 5) if span else None)
        if rtf > SPEED_RTF_LIMIT + 3 * sigma:
            b.update(st="fail", why=f"the emulator ran at {1 / rtf:.2f}x real time (rtf {rtf:.3f})")
        elif rtf + 3 * sigma <= SPEED_RTF_LIMIT or 3 * sigma <= SPEED_MAX_NOISE:
            b.update(st="pass", why=f"rtf {rtf:.4f} +- {3 * sigma:.3%} (3 sigma): not above {SPEED_RTF_LIMIT} "
                                    f"+ 3 sigma")
        else:
            b.update(st="untestable", why=f"timing noise too large to tell: rtf {rtf:.4f} +- {3 * sigma:.2%} "
                                          f"(3 sigma, limit {SPEED_MAX_NOISE:.1%}; per-sample noise "
                                          f"{noise * 1e3:.2f} ms, {len(pts)} samples over {span:.2f} s)")
        # host CPU of the emulation thread(s) per emulated second
        c0, c1 = emu_cpu(pts[0]), emu_cpu(pts[-1])
        df = pts[-1]["heartbeat"] - pts[0]["heartbeat"]
        if c0 is not None and c1 is not None and df > 0:
            emu_s = df / fhz
            b["cpu_per_emu_s"] = round((c1 - c0) / 1e9 / emu_s, 4)
            if pts[0].get("cpu_total_ns") is not None:
                b["cpu_total_per_emu_s"] = round((pts[-1]["cpu_total_ns"] - pts[0]["cpu_total_ns"]) / 1e9 / emu_s, 4)
            b["busy"] = round(b["cpu_per_emu_s"] / rtf, 4) if rtf else None
            if b["st"] == "fail" and b["busy"] is not None and b["busy"] < SPEED_CPU_BOUND:
                b.update(st="untestable",
                         why=f"the emulator lagged (rtf {rtf:.3f}) while its emulation thread was only "
                             f"{b['busy']:.0%} busy: the lag came from the host (scheduling, another process), "
                             f"not from the instruction's cost (a real slowdown keeps the thread at least "
                             f"{SPEED_CPU_BOUND:.0%} busy)")
        t = b.get("timing") or {}
        if b.get("n") and t.get("cck"):
            b["ops_per_emu_s"] = b["n"] * cck_hz / t["cck"]
    # 3. host cost per instruction: CPU per emulated second minus the
    # emulator's own (the STOP block: chipset, display, frame pacing), per
    # instruction, minus the share of the loop (the empty block's cost per
    # DBF iteration, 16 iterations per unit)
    def mean(xs):
        xs = [x for x in xs if x is not None]
        return sum(xs) / len(xs) if xs else None
    idle = mean([b.get("cpu_per_emu_s") for b in blocks if b.get("insn") == "stop"])
    loop_ns = mean([(b["cpu_per_emu_s"] - idle) * 1e9 / b["ops_per_emu_s"] for b in blocks
                    if str(b.get("insn")).startswith("empty") and b.get("cpu_per_emu_s") is not None
                    and b.get("ops_per_emu_s")] if idle is not None else [])
    for b in blocks:
        if b.get("cpu_per_emu_s") is None or not b.get("ops_per_emu_s") or idle is None:
            continue
        ns = (b["cpu_per_emu_s"] - idle) * 1e9 / b["ops_per_emu_s"]
        if b["insn"] == "stop":
            continue
        if str(b["insn"]).startswith("empty"):
            b["ns_per_iter"] = round(ns, 3)
            continue
        units = (b.get("timing") or {}).get("units")
        b["gross_ns_per_op"] = round(ns, 3)
        net = ns - (loop_ns * 16 * units / b["n"] if loop_ns is not None and units else 0)
        if net < 0:     # cheaper than the loop it sits in, within the resolution (JIT: compiled together)
            b["ns_note"] = f"below the measurement resolution (net {net:.1f} ns)"
            net = 0.0
        b["ns_per_op"] = round(net, 3)
    refs = {b["insn"]: b for b in blocks if b.get("insn") in SPEED_REFS.values()}
    for b in blocks:
        ref = refs.get(SPEED_REFS.get(b.get("cls")))
        if b.get("ns_per_op") is None or ref is None or ref is b:
            continue
        rn = ref.get("ns_per_op")
        b["ref"] = ref["insn"]
        if rn and rn > 0 and b["ns_per_op"] > 0:
            b["ratio_ref"] = round(b["ns_per_op"] / rn, 2)
    for b in blocks:
        b.pop("ended", None)
    return blocks


def speed_name(insn):
    return "speed_" + str(insn)


def host_desc(ctx):
    """'Cortex-A76 x4 (Raspberry Pi 500), --jobs 1, load 0.97': the host a
    host.speed_* verdict was measured on (the verdict is host-relative)."""
    ctx = ctx or {}
    cpu = ctx.get("cpu_model") or "unknown CPU"
    s = f"{cpu} x{ctx.get('cpus') or '?'}" + (f" ({ctx['board']})" if ctx.get("board") else "")
    s += f", --jobs {ctx.get('jobs') or 1}"
    la = ctx.get("loadavg_start")
    if la:
        s += f", load average {la[0]} at the start of the launch"
    return s


def speed_case(b, sub):
    """One block -> {"st", "fp", "expect", "got", "why", "timing"} (None for
    a skipped block): the host.speed_<insn> pseudo case."""
    if b.get("st") in (None, "skip"):
        return None
    st = b["st"]
    timing = {k: b[k] for k in ("rtf", "sigma", "fps", "cpu_per_emu_s", "cpu_total_per_emu_s", "ns_per_op",
                                "gross_ns_per_op", "ns_per_iter", "ratio_ref") if b.get(k) is not None}
    why = b.get("why") or ""
    if st == "fail":
        ctx = (sub or {}).get("host_context")
        cost = ""
        if b.get("ns_per_op") is not None:
            ns = b["ns_per_op"]
            cost = f"; {ns / 1000:.3g} us host per instruction" if ns >= 1000 else f"; {ns:.3g} ns host per instruction"
            if b.get("ratio_ref"):
                cost += f", {b['ratio_ref']:g}x {str(b['ref']).upper().replace('_', '.')}"
        why = (f"{str(b['insn']).upper()}: the emulator ran at {1 / b['rtf']:.2f}x real time while executing it "
               f"(rtf {b['rtf']:.3f}, limit {SPEED_RTF_LIMIT} + 3 sigma = {SPEED_RTF_LIMIT + 3 * b['sigma']:.4f})"
               f"{cost}. Measured on {host_desc(ctx)}: the verdict is relative to this host")
    expect = {"rtf_max": round(SPEED_RTF_LIMIT + 3 * (b.get("sigma") or 0), 5), "limit": SPEED_RTF_LIMIT}
    got = {k: b.get(k) for k in ("rtf", "sigma", "fps", "frame_hz", "samples", "span_s", "cpu_per_emu_s",
                                 "ns_per_op", "ratio_ref", "ipc_bound") if b.get(k) is not None}
    return {"st": st, "fp": {"st": st}, "expect": expect, "got": got, "why": why, "timing": timing}


def _speed_cases(out, subs):
    """docs/cases/hostspeed.md: every timed hostspeed block becomes the
    pseudo case host.speed_<insn>, fingerprinted {st} only (the real-time
    factor depends on the host); rtf, CPU time and ns per instruction are
    its timing values."""
    for dn, sub, m in subs:
        for b in speed_blocks(sub, m):
            v = speed_case(b, sub)
            if v is None:
                continue
            key = SPEED_PREFIX + str(b["insn"])
            if key in out["cases"]:
                key = f"{key}@{dn}" if dn else f"{key}#{b.get('pos')}"
            body = dict({"mod": "host", "case": key[len("host."):]}, **v["fp"])
            out["cases"][key] = {"mod": "host", "case": body["case"], "st": v["st"], "fp": canonical(body),
                                 "rec": body, "timing": v["timing"], "seq": None, "pseudo": True, "disk": dn,
                                 "engine_dependent": True}


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
