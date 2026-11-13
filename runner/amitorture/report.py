"""Results: REPORT.md (start here), summary.json, failures.jsonl, environment.json.

Kept as small steps so later milestones can slot in:
  collect_failures()  every failed case / infra problem, with full context
  group_problems()    the same failure across entries -> one problem, with a
                      pattern hint (JIT-only, CPU-only, ...); this is the
                      start of narrowing
  repro()             what is needed to reproduce one problem (repro packs
                      will be built from this)
  render_markdown()   the report for an AI reader
"""
from __future__ import annotations

import json
import os
import shlex

from . import records as recmod

STATUS_ORDER = ["crash", "hang", "fail", "timeout", "incomplete", "error", "pass", "skip"]
BAD = {"crash", "hang", "fail", "timeout", "incomplete", "error"}
CONTEXT_RECORDS = 6


# ---------------------------------------------------------------------------
# failures

def _entry_ref(r):
    return {"entry": r["id"], "jit": r.get("jit"), "twin_of": r.get("twin_of"), "machine": r.get("machine"),
            "rom": (r.get("rom") or {}).get("name"), "config": (r.get("config") or {}).get("file")}


def _norm_cpu(s):
    return str(s or "").upper().replace("MC", "")


def probe_mismatches(r, merged):
    """What the guest probe saw vs what the config asked for."""
    if r.get("machine_from_probe") or r.get("selftest"):
        return []
    hello = merged.by_type("hello")
    if not hello:
        return []
    h = hello[-1]
    probe = h.get("probe") or {}
    m = r.get("machine") or {}
    out = []
    checks = [("cpu", _norm_cpu(m.get("cpu")), _norm_cpu(probe.get("cpu", h.get("cpu")))),
              ("fpu", str(m.get("fpu") or "").lower(), str(probe.get("fpu", h.get("fpu")) or "").lower()),
              ("chipset", str(m.get("chipset") or "").upper(), str(probe.get("chipset") or "").upper())]
    assumed = [x for x in probe.get("how", []) if isinstance(x, str) and "assume" in x]
    no_mmu = str((r.get("options") or {}).get("mmu_model", "")) == "0"
    for what, want, got in checks:
        if what == "cpu" and no_mmu and got == "68EC" + want[2:] and want in ("68030", "68040", "68060"):
            # Configured without MMU emulation (mmu_model=0): the guest seeing
            # an EC part may be intended; kept as a note, since a real
            # 680x0 (non-EC) has an MMU that software can detect.
            out.append({"what": what, "configured": want, "probe": got, "confidence": "low",
                        "note": f"config has mmu_model=0 (no MMU emulation), probe mmu={probe.get('mmu')}; "
                                "a real non-EC CPU has an MMU software can detect"})
            continue
        if (what == "cpu" and no_mmu and want in ("68EC040", "68EC060")
                and got == "68LC0" + want[5:]):
            # Amiberry keeps the 040/060 TC register bits even with
            # mmu_model=0 (no MMU emulation), so the probe's TC write/read-back
            # sees an MMU and names an EC part LC. The faithful EC config is
            # mmu_model=68ec040/68ec060 (the *-mmuec entries); with the JIT,
            # mmu_model is dropped, so these entries can't use it.
            out.append({"what": what, "configured": want, "probe": got, "confidence": "low",
                        "note": "mmu_model=0: Amiberry still stores the TC bits, so an EC part reads as LC; "
                                "use mmu_model=68ec040/68ec060 for a faithful EC (not possible with the JIT)"})
            continue
        if want and got and want != got:
            mm = {"what": what, "configured": want, "probe": got, "confidence": "high"}
            if what == "cpu" and got.replace("EC", "").replace("LC", "") == want and assumed:
                # e.g. 68EC020 vs 68020 when the probe could only assume 24-bit
                # addressing (no memory above 16 MB to test with).
                mm["confidence"] = "low"
                mm["note"] = f"the probe only assumed it: {', '.join(assumed)}"
            out.append(mm)
    return out


def _tone_failure(items, dref, m, hello_sum):
    """A failed host.tone_<tag> pseudo case (cases.tone_case) as a case_fail
    item: expected and measured frequency, output channel and the Paula/
    board channel, the tolerance, and the WAV to listen to."""
    from . import cases as casemod
    hr = m["rec"]
    v = casemod.tone_case(hr)
    if v is None or v["st"] != "fail":
        return
    res = hr.get("result") or {}
    items.append({
        "kind": "case_fail", **dref, "mod": "host", "case": casemod.tone_name(hr.get("tag") or hr.get("id")),
        "expect": v["expect"], "got": v["got"], "why": v["why"],
        "ctx": {"wav": res.get("wav"), "segment_s": (res.get("analysis") or {}).get("seconds")},
        "in": {"request": hr.get("id"), "tag": hr.get("tag")}, "seq": hr.get("req_seq"), "boot": m["boot"],
        "sources": ["host"], "record": hr, "preceding": [], "hello": hello_sum,
    })


def _speed_failures(items, dref, sub, merged, hello_sum):
    """Each failed host.speed_<insn> pseudo case (cases.speed_case) as a
    case_fail item: the real-time factor with its noise, the frame rates,
    the host CPU cost per instruction, and the host it was measured on."""
    from . import cases as casemod
    for b in casemod.speed_blocks(sub, merged):
        v = casemod.speed_case(b, sub)
        if v is None or v["st"] != "fail":
            continue
        items.append({
            "kind": "case_fail", **dref, "mod": "host", "case": casemod.speed_name(b["insn"]),
            "expect": v["expect"], "got": v["got"], "why": v["why"],
            "ctx": {"host": sub.get("host_context"), "block": {k: b.get(k) for k in ("insn", "cls", "pos", "n",
                                                                                   "timing", "dropped")},
                    "samples": "raw result.json hostspeed.samples"},
            "in": {"insn": b["insn"], "pos": b.get("pos")}, "seq": None, "boot": None,
            "sources": ["host"], "record": None, "preceding": [], "hello": hello_sum,
        })


def collect_failures(results):
    """One item per failed case or infra problem, each self-contained. With
    a disk set, cases, record integrity and crash/hang/timeout are per disk
    (items carry "disk"); the probe is checked once, plus consistency across
    the disks (probe_inconsistent)."""
    items = []
    for r in results:
        ref = _entry_ref(r)
        if r["status"] == "skip":
            continue
        subs = r.get("disks") or [r]
        probe_done = False
        for sub in subs:
            merged = sub.get("_merged")
            dn = sub.get("disk_name")
            dref = dict(ref, disk=dn) if dn else ref
            if dn:
                dref["raw"] = sub.get("raw")
                dref["config"] = (sub.get("config") or {}).get("file") or ref.get("config")
            cfg = sub.get("config") or {}
            if cfg.get("dump_ok") is False and sub["status"] != "error":
                from . import amiberry_cfg
                items.append({"kind": "error", **dref,
                              "why": amiberry_cfg.verification_error(dict(cfg, ok=False))})
            if merged is not None:
                recs = merged.records
                hello = merged.by_type("hello")
                hello_sum = None
                if hello:
                    h = hello[-1]
                    p = h.get("probe") or {}
                    hello_sum = {k: h.get(k) for k in ("build", "exec", "cpu", "fpu", "mmu", "mode", "seed")}
                    hello_sum.update({k: p.get(k) for k in ("chipset", "video", "kick", "agnus_id", "denise_id") if k in p})
                for i, m in enumerate(recs):
                    rec = m["rec"]
                    if rec.get("t") == "case" and rec.get("st") == "fail":
                        before = [x["rec"] for x in recs[max(0, i - CONTEXT_RECORDS):i]]
                        items.append({
                            "kind": "case_fail", **dref, "mod": rec.get("mod"), "case": rec.get("case"),
                            "expect": rec.get("expect"), "got": rec.get("got"), "why": rec.get("why"),
                            "ctx": rec.get("ctx"), "in": rec.get("in"), "seq": rec.get("seq"), "boot": m["boot"],
                            "sources": m["sources"], "record": rec, "preceding": before, "hello": hello_sum,
                        })
                    elif m.get("host"):
                        _tone_failure(items, dref, m, hello_sum)
                _speed_failures(items, dref, sub, merged, hello_sum)
                if hello and not probe_done:
                    probe_done = True
                    probe_src = dict(r, _merged=merged)
                    for mm in probe_mismatches(probe_src, merged):
                        items.append({"kind": "probe_mismatch", **dref, **mm,
                                      "why": f"config asks for {mm['what']} {mm['configured']}, the guest probe saw "
                                             f"{mm['probe']}" + (f" (low confidence: {mm['note']})"
                                                                 if mm["confidence"] == "low" else ""),
                                      "hello": hello_sum})
                for g in merged.gaps:
                    items.append({"kind": "records_missing", **dref, **g,
                                  "why": f"seq {g['from']}..{g['to']} never arrived on any transport"})
                lost = recmod.bad_unrecovered(merged)
                if lost:
                    items.append({"kind": "records_corrupt", **dref, "count": len(lost),
                                  "samples": lost[:5], "why": "lines with a bad CRC or unparsable JSON, and the "
                                  "record did not arrive intact on the other transport"})
                elif merged.bad:
                    items.append({"kind": "transport_corrupt_recovered", **dref, "count": len(merged.bad),
                                  "samples": merged.bad[:3], "severity": "low",
                                  "why": "a transport delivered corrupt line(s) (e.g. serial byte duplication, "
                                  "Amiberry finding D3); every record arrived intact on the other transport"})
                for c in merged.conflicts[:10]:
                    items.append({"kind": "records_conflict", **dref, **c,
                                  "why": "serial and mailbox delivered different content for the same seq"})
                for sp in merged.schema_problems[:10]:
                    items.append({"kind": "schema", **dref, **sp, "why": "; ".join(sp["problems"])})
                if sub.get("ended") != "selftest":
                    lifecycle = recmod.module_problems(merged)
                    if lifecycle:
                        items.append({"kind": "schema", **dref, "problems": lifecycle,
                                      "why": "module lifecycle: " + "; ".join(lifecycle)})
            if sub["status"] in ("crash", "hang", "timeout", "error", "incomplete"):
                items.append({
                    "kind": sub["status"], **dref,
                    "why": (f"disk {dn}: " if dn else "") + str(sub.get("reason")),
                    "last_records": [m["rec"] for m in (merged.records[-CONTEXT_RECORDS:] if merged else [])],
                    "mailbox": (sub.get("mailbox") or {}).get("last_header"), "hang": sub.get("hang"),
                    "exit": (sub.get("process") or {}).get("exit_before_stop"),
                    "log_tail": (sub.get("process") or {}).get("log_tail", [])[-40:],
                })
        pc = r.get("probe_consistency")
        if pc and not pc.get("ok"):
            items.append({"kind": "probe_inconsistent", **ref, "reference_disk": pc.get("reference_disk"),
                          "differences": pc.get("differences"),
                          "why": "the disks of this entry saw different machines in their probe: "
                                 + "; ".join(f"{d['disk']} {d['path']}: {_j(d['reference'], 60)} vs {_j(d['got'], 60)}"
                                             for d in pc["differences"][:4])})
        cfg = r.get("config") or {}
        if cfg.get("differences") or cfg.get("unknown_options"):
            items.append({"kind": "config_not_applied", **ref, "differences": cfg.get("differences"),
                          "unknown_options": cfg.get("unknown_options"),
                          "why": "Amiberry resolved the config differently from what was written (--dump-config)"})
    for n, it in enumerate(items):
        it["id"] = f"F{n + 1:03d}"
    return items


# ---------------------------------------------------------------------------
# grouping / narrowing

def _key(it):
    if it["kind"] == "case_fail":
        return ("case_fail", it.get("mod"), it.get("case"))
    if it["kind"] == "probe_mismatch":
        return ("probe_mismatch", it.get("what"), it.get("probe"))
    return (it["kind"],)


def pattern_hint(failing, results):
    """Says what the failing entries have in common, e.g. JIT-only."""
    ran = [r for r in results if r["status"] != "skip"]
    fail_ids = {f["entry"] for f in failing}
    fails = [r for r in ran if r["id"] in fail_ids]
    passes = [r for r in ran if r["id"] not in fail_ids]
    if not passes:
        return "fails on every entry that ran"
    hints = []
    twins = {r["id"]: r for r in ran}
    jit_only = all(r.get("jit") for r in fails) and all(
        (r.get("twin_of") in twins and twins[r["twin_of"]]["id"] not in fail_ids) for r in fails)
    if jit_only:
        hints.append("JIT only: every failing entry is a JIT twin whose non-JIT entry passes")
    elif all(not r.get("jit") for r in fails) and all(
            (r["id"] + "-jit") in twins and (r["id"] + "-jit") not in fail_ids for r in fails):
        hints.append("interpreter only: every failing entry has a JIT twin that passes")
    for k in ("cpu", "chipset", "fpu"):
        vals = {r["machine"].get(k) for r in fails}
        others = {r["machine"].get(k) for r in passes}
        if len(vals) == 1 and not (vals & others):
            hints.append(f"only {k}={vals.pop()}")
    roms = {(r.get("rom") or {}).get("name") for r in fails}
    if len(roms) == 1 and not (roms & {(r.get("rom") or {}).get("name") for r in passes}):
        hints.append(f"only with ROM {roms.pop()}")
    return "; ".join(hints) or "no single common factor"


def group_problems(items, results):
    groups = {}
    for it in items:
        groups.setdefault(_key(it), []).append(it)
    problems = []
    for n, (k, its) in enumerate(sorted(groups.items(), key=lambda kv: (-len(kv[1]), str(kv[0])))):
        entries = sorted({i["entry"] for i in its})
        title = {
            "case_fail": lambda: f"case {k[1]}/{k[2]} fails",
            "probe_mismatch": lambda: f"probe sees {k[1]}={k[2]} where the config asked otherwise",
        }.get(k[0], lambda: f"{k[0]}")()
        low = all(i.get("confidence") == "low" for i in its)
        problems.append({"id": f"P{n + 1:02d}", "title": title, "kind": k[0], "key": list(k), "entries": entries,
                         "severity": "low" if low else "normal",
                         "passes_on": sorted({r["id"] for r in results if r["status"] not in ("skip",)} - set(entries)),
                         "pattern": pattern_hint(its, results), "failures": [i["id"] for i in its],
                         "repro": repro(its[0], results)})
    return problems


def repro(item, results):
    """How to re-run the smallest thing that shows this problem."""
    r = next((x for x in results if x["id"] == item["entry"]), None)
    if r is None or "command" not in r:
        return None
    cmd = r["command"]
    env = " ".join(f"{k}={shlex.quote(v)}" for k, v in cmd["env"].items())
    return {"entry": r["id"], "config": (r.get("config") or {}).get("file"),
            "shell": f"{env} {' '.join(shlex.quote(a) for a in cmd['argv'])}",
            "amitorture": f"python3 -m amitorture run --config {r['id']} ..."}


# ---------------------------------------------------------------------------
# outputs

def _jsonable(r):
    out = {k: v for k, v in r.items() if not k.startswith("_")}
    if isinstance(out.get("disks"), list):
        out["disks"] = [{k: v for k, v in d.items() if not k.startswith("_")} for d in out["disks"]]
    return out


def entry_counts(r):
    m = r.get("_merged")
    c = {"pass": 0, "fail": 0, "skip": 0, "untestable": 0}
    if m is None:
        return c
    for rec in m.by_type("case"):
        st = rec.get("st")
        if st in c:
            c[st] += 1
    return c


def _interrupted_prefix(environment):
    it = environment.get("interrupted")
    if not it:
        return ""
    if it.get("during") == "analysis":
        return "INTERRUPTED (all entries finished; parity, narrowing and repro packs not done): "
    return f"INTERRUPTED ({it.get('finished')} of {it.get('selected')} entries finished): "


def verdict(results, isolation_ok, problems=()):
    if not isolation_ok:
        return "ISOLATION VIOLATED"
    ran = [r for r in results if r["status"] != "skip"]
    if not ran:
        return "NOTHING RAN"
    bad = [r for r in ran if r["status"] in BAD]
    problems = [p for p in problems if p.get("severity") != "low"]
    adjusted = [r for r in ran if r.get("adjusted")]
    tail = f", {len(adjusted)} config(s) adjusted by the emulator" if adjusted else ""
    if bad:
        return f"PROBLEMS ({len(bad)} of {len(ran)} entries not passing, {len(problems)} problem(s){tail})"
    if problems:
        return f"PROBLEMS ({len(problems)} problem(s), all entries passed their cases{tail})"
    return "PASS" + (f" ({tail[2:]})" if tail else "")


def _minimal_analysis(results, binary=None):
    """For `import` (no Context, no narrowing): fingerprints, failures, problems."""
    from . import analysis as an
    from . import cases as casemod
    from . import problems as pm
    from . import xfail as xfailmod
    fps = {r["id"]: casemod.extract(r) for r in results}
    registry = xfailmod.load()
    items, infos = an.failure_items(results, fps, registry)
    xf, fixed = xfailmod.classify(registry, fps, results)
    probs = pm.group(items, results, None)
    from . import codemap
    sp = codemap.find_src(binary)
    src = codemap.Source(sp) if sp else None
    items_by_id = {i["id"]: i for i in items}
    by_res = {r["id"]: r for r in results}
    for p in probs:
        rep = items_by_id.get(p["representative"]) or {}
        r = by_res.get(rep.get("entry")) or {}
        p["pointers"] = pm.pointers_for(rep, r, src) if p["kind"] in pm.GROUPED and r.get("options") else []
        p.setdefault("docs", {})
        for k in p["case_keys"][:50]:
            mod, _, case = k.partition(".")
            d = pm.case_doc(mod, case.split("#")[0])
            if d:
                p["docs"][k] = d
    return {"fps": fps, "items": items, "parity": infos, "xfail": xf, "fixed": fixed, "problems": probs,
            "narrowing": None}


def entry_case_counts(fp, items, eid):
    c = {"pass": 0, "fail": 0, "skip": 0, "untestable": 0, "xfail": 0, "engine_mismatch": 0, "unstable": 0}
    if fp:
        for key, x in fp["cases"].items():
            if x.get("pseudo"):
                continue
            if x["st"] in c:
                c[x["st"]] += 1
    for it in items:
        if it["entry"] != eid:
            continue
        if it["kind"] == "engine_mismatch":
            c["engine_mismatch"] += 1
        if it["kind"] == "case_fail" and it.get("xfail"):
            c["xfail"] += 1
    return c


def module_catalogue(results, fps, items, narrowing):
    """{mod: counts and where it failed / was missing}."""
    mods = {}
    ran = [r for r in results if r["status"] not in ("skip",)]
    unstable = set()
    for g in (narrowing or {}).get("groups", []):
        for k, v in g.get("cases", {}).items():
            if v.get("determinism") == "intermittent":
                unstable.add((g["entry"], k))
    for r in ran:
        fp = fps.get(r["id"]) or {"cases": {}}
        seen = set()
        for key, c in fp["cases"].items():
            if c.get("pseudo"):
                continue
            m = mods.setdefault(c["mod"], {"id": (fp.get("modules") or {}).get(c["mod"]), "entries": set(),
                                           "pass": 0, "fail": 0, "skip": 0, "untestable": 0, "xfail": 0,
                                           "engine_mismatch": 0, "unstable": 0, "failing_on": set(), "cases": set()})
            if m["id"] is None:
                m["id"] = (fp.get("modules") or {}).get(c["mod"])
            m["entries"].add(r["id"])
            m["cases"].add(key)
            seen.add(c["mod"])
            if c["st"] in m:
                m[c["st"]] += 1
            if c["st"] == "fail":
                m["failing_on"].add(r["id"])
            if (r["id"], key) in unstable:
                m["unstable"] += 1
    for it in items:
        m = mods.get(it.get("mod"))
        if m is None:
            continue
        if it["kind"] == "engine_mismatch":
            m["engine_mismatch"] += 1
        if it["kind"] == "case_fail" and it.get("xfail"):
            m["xfail"] += 1
    ran_ids = {r["id"] for r in ran if r["status"] in ("pass", "fail", "incomplete")}
    for name, m in mods.items():
        m["missing_on"] = sorted(ran_ids - m["entries"])
    return mods


def catalogue_summary(results, fps, parity_info=None):
    """The coverage catalogue (docs/catalogue.yaml) evaluated on this run."""
    from . import catalogue
    try:
        items = catalogue.load()
    except ValueError as e:
        return {"error": str(e)}
    if items is None:
        return None
    rows = catalogue.coverage(items, results, fps, parity_info)
    return {"file": "docs/catalogue.yaml", "totals": catalogue.totals(rows), "rows": rows}


def _host_records(r):
    out = []
    for sub in (r.get("disks") or [r]):
        m = sub.get("_merged")
        if m is None:
            continue
        for x in m.records:
            if x.get("host"):
                out.append(dict(x["rec"], disk=sub.get("disk_name")))
    return out


def host_summary(results):
    """Per entry: the host side of the request channel (CONTRACT §13)."""
    out = []
    for r in results:
        hs = _host_records(r)
        if not hs:
            continue
        e = {"entry": r["id"], "requests": sum(1 for h in hs if h.get("req_t") == "request"),
             "acked": sum(1 for h in hs if h.get("acked")), "not_acked": [], "screenshots": [], "audio": []}
        for h in hs:
            res = h.get("result") or {}
            if h.get("req_t") == "request" and not h.get("acked"):
                e["not_acked"].append({"id": h.get("id"), "what": h.get("what"), "tag": h.get("tag"),
                                       "disk": h.get("disk"), "why": res.get("error") or res.get("ack_error")})
            if h.get("what") == "screenshot" and res.get("path"):
                e["screenshots"].append({"id": h.get("id"), "tag": h.get("tag"), "path": res["path"],
                                         "sha256": res.get("sha256"), "disk": h.get("disk")})
            an = res.get("analysis")
            if h.get("what") == "audio_mark" and an:
                e["audio"].append({"id": h.get("id"), "tag": h.get("tag"), "disk": h.get("disk"),
                                   "seconds": an.get("seconds"), "dominant_hz": an.get("dominant_hz"),
                                   "expect_hz": an.get("expect_hz"), "freq_ok": an.get("freq_ok"),
                                   "expect_side": an.get("expect_side"), "side_ok": an.get("side_ok"),
                                   "rms": {k: c.get("active_rms", c.get("rms")) for k, c in
                                           (an.get("channels") or {}).items()},
                                   "freq": {k: c.get("freq_fft_hz") for k, c in (an.get("channels") or {}).items()},
                                   "wav": res.get("wav")})
                from . import cases as casemod
                v = casemod.tone_case(h)
                if v is not None:
                    e["audio"][-1]["tone"] = {"case": "host." + casemod.tone_name(h.get("tag") or h.get("id")),
                                              "st": v["st"], "expect": v["expect"],
                                              "measured_hz": v["got"]["freq_hz"], "why": v["why"]}
        out.append(e)
    return out


SPEED_KEYS = ("insn", "cls", "st", "benchmark", "rtf", "sigma", "fps", "frame_hz", "samples", "cpu_per_emu_s",
              "cpu_total_per_emu_s", "over_idle_per_emu_s", "ops_per_emu_s", "ops_per_host_s", "ns_per_op",
              "ns_per_iter", "ratio_ref", "ref", "why", "quiet_thread_share", "flood_thread_share", "quiet_samples")


def speed_summary(results):
    """Per entry and disk: the hostspeed blocks (cases.speed_blocks) with
    their verdicts, real-time factors and host cost per instruction."""
    from . import cases as casemod
    out = []

    def thread_key(r):
        options = tuple(sorted((k, v) for k, v in (r.get("options") or {}).items() if k != "cpu_threaded"))
        return r.get("binary"), json.dumps(r.get("params") or {}, sort_keys=True), options

    def requested(r):
        return str((r.get("options") or {}).get("cpu_threaded", "false")).lower() == "true"

    refs = {thread_key(r): r["id"] for r in results if not requested(r) and not casemod.cpu_thread_active(r)}
    for r in results:
        for sub in r.get("disks") or [r]:
            bl = casemod.speed_blocks(sub, sub.get("_merged"))
            if not bl:
                continue
            out.append({"entry": r["id"], "disk": sub.get("disk_name"), "jit": r.get("jit"),
                        "cpu_threaded": casemod.cpu_thread_active(sub if sub.get("adjusted") else r),
                        "cpu_threaded_requested": requested(r),
                        "thread_reference": refs.get(thread_key(r)),
                        "cpu_speed": (r.get("options") or {}).get("cpu_speed"), "host": sub.get("host_context"),
                        # the threaded CPU's request-flood fallback, from the emulator log
                        "flood_holds": (sub.get("engine") or r.get("engine") or {}).get("cpu_thread_flood_holds"),
                        "blocks": [{k: b.get(k) for k in SPEED_KEYS if b.get(k) is not None} for b in bl]})
    return out


def _speed_lines(speeds):
    L = ["### Host speed per instruction (hostspeed, docs/cases/hostspeed.md)", "",
         "Each block runs one instruction for about 1 s of emulated time. **rtf** = host seconds per emulated "
         "second (from the frame counter the runner reads with the host clock; > 1: the emulator fell behind "
         "real time), +- 3 sigma. **CPU/s**: host CPU seconds of the emulation thread per emulated second (1.0 "
         "= no headroom left). **ns/op**: host CPU per instruction, net of the emulator's own cost (the STOP "
         "block) and of the loop (the empty block); **x ref**: that cost relative to ADD.L / FADD. The verdict "
         "depends on the host: compare it only with runs on the same host. Benchmark-only disks report "
         "**measured** instead of pass/fail; unsupported workloads are **skip** with a reason. The frame "
         "blocks (stop, vbl_poll, chipset_load, stop_timer) idle for a number of frames: their measure is CPU/s, "
         "and **x ref** shows their CPU/s over stop's.", ""]
    for e in speeds:
        h = e.get("host") or {}
        from . import cases as casemod
        L.append(f"- `{e['entry']}`" + (f" [{e['disk']}]" if e.get("disk") else "")
                 + f", cpu_speed={e.get('cpu_speed')}, JIT {'on' if e.get('jit') else 'off'}, "
                 + f"threaded CPU {'on' if e.get('cpu_threaded') else 'off'}; "
                 + f"host {casemod.host_desc(h)}")
        L.append("")
        L.append("  | insn | verdict | rtf (+-3 sigma) | CPU/s | ns/op | x ref |")
        L.append("  |---|---|---|---|---|---|")
        for b in e["blocks"]:
            if b.get("st") == "skip":
                L.append(f"  | {b['insn']} | skip | {b.get('why') or ''} | | | |")
                continue
            rtf = f"{b['rtf']:.4f} +- {3 * b.get('sigma', 0):.4f}" if b.get("rtf") is not None else "-"
            ns = b.get("ns_per_op", b.get("ns_per_iter"))
            xref = b.get("ratio_ref", "")
            if b.get("over_idle_per_emu_s") is not None:
                xref = f"{b['over_idle_per_emu_s']:+.4f} CPU/s vs stop"
            L.append(f"  | {b['insn']} | {'**FAIL**' if b['st'] == 'fail' else b['st']} | {rtf} | "
                     f"{b.get('cpu_per_emu_s', '-')} | {'-' if ns is None else f'{ns:.1f}'} | "
                     f"{xref} |")
        L.append("")
    L.extend(_threaded_speed_lines(speeds))
    return L


def _threaded_speed_lines(speeds):
    """Only compare like-for-like benchmark entries: CPU threading is the sole option difference."""
    from . import cases as casemod

    def value(block, key):
        n = block.get(key)
        return "-" if n is None else f"{n:.3g}"

    by_entry = {(e["entry"], e.get("disk")): e for e in speeds}
    L = []
    inactive = [on for on in speeds if on.get("cpu_threaded_requested") and not on.get("cpu_threaded")]
    for on in speeds:
        off = by_entry.get((on.get("thread_reference"), on.get("disk")))
        if not on.get("cpu_threaded") or off is None:
            continue
        refs = {b["insn"]: b for b in off["blocks"] if b.get("benchmark")}
        blocks = [(refs[b["insn"]], b) for b in on["blocks"] if b.get("benchmark") and b["insn"] in refs]
        if not blocks:
            continue
        if not L:
            L.extend(["### Threaded CPU off/on benchmarks", "",
                      "Each pair differs only in `cpu_threaded`, including the same JIT setting. Values are "
                      "**off / on**. Throughput on/off > 1 is faster; lower ns/op and total CPU/s are cheaper. "
                      "Host ops/s includes frame pacing; empty counts loop iterations. The frame blocks (stop, vbl_poll, "
                      "chipset_load, stop_timer) count frames, so only their total CPU/s means anything. A threaded "
                      "entry behind real time where its twin keeps up, or an idle (stop, stop_timer) one costing "
                      "much more, is a `thread_speed` problem (docs/cases/hostspeed.md). "
                      "Treat a single run as a sample, not proof of a speedup.", ""])
        L.extend([f"`{off['entry']}` / `{on['entry']}`" + (f" [{on['disk']}]" if on.get("disk") else ""), "",
                  "| workload | host ops/s off / on | throughput on/off | ns/op off / on | total CPU/s off / on |",
                  "|---|---|---|---|---|"])
        for a, b in blocks:
            if a["st"] == "skip" or b["st"] == "skip":
                skipped = a if a["st"] == "skip" else b
                L.append(f"| {b['insn']} | skip: {skipped.get('why') or ''} | | | |")
                continue
            if a["st"] != "measured" or b["st"] != "measured":
                L.append(f"| {b['insn']} | {a['st']} / {b['st']} | - | - | - |")
                continue
            rate_a, rate_b = a.get("ops_per_host_s"), b.get("ops_per_host_s")
            ratio = f"{rate_b / rate_a:.3f}x" if rate_a and rate_b is not None else "-"
            if b["insn"] in casemod.FRAME_BLOCKS:
                ratio = "- (frames)"
            L.append(f"| {b['insn']} | {value(a, 'ops_per_host_s')} / {value(b, 'ops_per_host_s')} | {ratio} | "
                     f"{value(a, 'ns_per_op')} / {value(b, 'ns_per_op')} | "
                     f"{value(a, 'cpu_total_per_emu_s')} / {value(b, 'cpu_total_per_emu_s')} |")
        L.append("")
    if inactive:
        L.extend(["### Threaded CPU requested but not active", "",
                  "These entries asked for `cpu_threaded=true`, but the emulator log says the CPU ran on the "
                  "main thread (see the entry's adjusted options), so they are left out of the off/on "
                  "comparison:", ""]
                 + [f"- `{e['entry']}`" + (f" [{e['disk']}]" if e.get("disk") else "") for e in inactive] + [""])
    return L


def write_all(outdir, environment, results, source="run", analysis=None):
    from .runner import integrate_host_verdicts
    for r in results:
        integrate_host_verdicts(r)
    an = analysis if analysis is not None else _minimal_analysis(
        results, (environment.get("amiberry") or {}).get("path"))
    items, problems, fps = an["items"], an["problems"], an["fps"]
    iso = environment.get("isolation", {"ok": True, "changes": []})
    for p in problems:
        if p.get("passes_diff") is None:       # diagnose.passes_diff: entries where the cases really passed
            ran = {r["id"] for r in results if r["status"] != "skip"}
            p["passes_on"] = sorted(ran - set(p["entries"]))
    summ = {
        "run_id": environment.get("run_id"),
        "source": source,
        "verdict": _interrupted_prefix(environment) + verdict(results, iso.get("ok", True), problems),
        "interrupted": environment.get("interrupted"),
        "status_counts": {s: sum(1 for r in results if r["status"] == s) for s in STATUS_ORDER
                          if any(r["status"] == s for r in results)},
        "entries": [{"id": r["id"], "title": r.get("title"), "status": r["status"], "reason": r.get("reason"),
                     "jit": r.get("jit"), "twin_of": r.get("twin_of"), "twin_kind": r.get("twin_kind"),
                     "machine": r.get("machine"), "rom": (r.get("rom") or {}).get("name"),
                     "cases": entry_case_counts(fps.get(r["id"]), items, r["id"]),
                     "as_designed": not r.get("adjusted") and all(
                         (s.get("config") or {}).get("dump_ok") is True and
                         ("warp" not in s or s["warp"].get("verified") is True)
                         for s in (r.get("disks") or [r])),
                     "adjusted": r.get("adjusted"), "warp": r.get("warp"),
                     "records": (r.get("records") or {}).get("merged"), "duration_s": r.get("duration_s"),
                     "ended": r.get("ended")} for r in results],
        "modules": {k: {kk: (sorted(vv) if isinstance(vv, set) else vv) for kk, vv in v.items() if kk != "cases"}
                    | {"cases": len(v["cases"])}
                    for k, v in module_catalogue(results, fps, items, an.get("narrowing")).items()},
        "parity": an.get("parity"),
        "xfail": [{k: x[k] for k in ("entry", "case_key", "st")} | {"ref": x["xfail"].get("ref")} for x in an["xfail"]],
        "fixed": [{k: x[k] for k in ("entry", "case_key", "st")} | {"ref": x["xfail"].get("ref")} for x in an["fixed"]],
        "problems": [{k: p.get(k) for k in ("id", "title", "kind", "mod", "severity", "entries", "passes_on", "cases",
                                            "case_keys", "failures", "representative", "signature", "pointers",
                                            "repro", "history", "fields", "case_table", "config_flags",
                                            "passes_diff", "emulator_log")}
                     | {"narrowing": (p.get("narrowing") or {}).get("statement")} for p in problems],
        "narrowing": None if not an.get("narrowing") else {k: an["narrowing"].get(k) for k in (
            "budget_s", "used_s", "launches", "exhausted", "skipped")},
        "isolation": {"ok": iso.get("ok", True), "changes": iso.get("changes", []),
                      "user_amiberry_seen": iso.get("user_amiberry_seen", [])},
        "catalogue": catalogue_summary(results, fps, an.get("parity")),
        "host_requests": host_summary(results),
        "hostspeed": speed_summary(results),
        "files": {"report": "REPORT.md", "failures": "failures.jsonl", "environment": "environment.json",
                  "configs": "configs/", "raw": "raw/", "narrowing": "narrowing.json", "repro": "repro/"},
    }
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, "summary.json"), "w", encoding="utf-8") as fh:
        json.dump(summ, fh, indent=1, default=_default)
    with open(os.path.join(outdir, "environment.json"), "w", encoding="utf-8") as fh:
        json.dump(environment, fh, indent=1, default=_default)
    with open(os.path.join(outdir, "failures.jsonl"), "w", encoding="utf-8") as fh:
        for it in items:
            fh.write(json.dumps(it, default=_default) + "\n")
    for r in results:
        d = os.path.join(outdir, (r.get("raw") or f"raw/{r['id']}/").rstrip("/"))
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "result.json"), "w", encoding="utf-8") as fh:
            json.dump(_jsonable(r), fh, indent=1, default=_default)
    with open(os.path.join(outdir, "REPORT.md"), "w", encoding="utf-8") as fh:
        fh.write(render_markdown(summ, environment, results, items, problems, an))
    return summ


def _default(o):
    if isinstance(o, set):
        return sorted(o)
    return str(o)


def _j(v, limit=600):
    s = json.dumps(v, separators=(",", ":"), default=_default)
    return s if limit is None or len(s) <= limit else s[:limit] + "…"


def _cell(s):
    return str(s if s is not None else "").replace("|", "\\|").replace("\n", " ")


def _engine(e):
    k = e.get("twin_kind")
    if not k:
        return "reference" if e.get("jit") is False else ""
    return f"{k} twin of {e.get('twin_of')}"


def render_markdown(summ, env, results, items, problems, an):
    from . import matrix as matrixmod
    L = []
    a = env.get("amiberry") or {}
    items_by_id = {i["id"]: i for i in items}
    res_by_id = {r["id"]: r for r in results}
    L.append(f"# amitorture report: {summ['verdict']}")
    L.append("")
    it = env.get("interrupted")
    if it:
        if it.get("during") == "analysis":
            L.append("**Interrupted (Ctrl+C) after every entry had run:** JIT/engine parity, narrowing and repro "
                     "packs were not done. Rebuild them with `python3 -m amitorture report <this dir>`; narrowing "
                     "needs a new run.")
        else:
            nr = it.get("not_run") or []
            L.append(f"**Interrupted (Ctrl+C):** {it.get('finished')} of {it.get('selected')} entries finished and are "
                     f"reported below; parity and narrowing were not done. Not run: "
                     + (", ".join(f"`{x}`" for x in nr[:40]) + (" …" if len(nr) > 40 else "") if nr else "none")
                     + ". An entry whose disks were only partly run is left out.")
        L.append("")
    L.append(f"Run `{summ.get('run_id')}` ({summ['source']}), emulator `{_cell(a.get('version'))}` "
             f"(sha256 `{_cell((a.get('sha256') or '')[:16])}`), test disk build "
             f"`{_cell((env.get('disk_source') or {}).get('build_id'))}`. Written for an AI reader: every statement "
             "below is backed by files in this directory (`summary.json`, `failures.jsonl`, `narrowing.json`, "
             "`raw/<entry>/`, `configs/`, `repro/`); field meanings are in docs/schema.md of the amitorture repo, "
             "case intents in docs/cases/<module>.md, emulator code locations in docs/emulator-map/amiberry.md.")
    L.append("")
    L.append("Terms: an *entry* is one emulated machine (model, CPU, FPU, chipset, core mode, ROM). A *twin* "
             "differs from its reference entry only in engine options (JIT / threaded CPU / softfloat), so any "
             "difference in its case records is an `engine_mismatch` (softfloat: `fpu_mode_difference`, "
             "informational). A failing case is the guest's own verdict, CRC-checked over two transports.")
    L.append("")
    # ---------------- coverage
    L.append("## Coverage")
    L.append("")
    mods = summ.get("modules") or {}
    if mods:
        L.append("### Modules × entries")
        L.append("")
        L.append("| Module | id | Cases | Entries ran | pass | fail | skip | untestable | xfail | engine_mismatch "
                 "| unstable | Fails on | Missing on |")
        L.append("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
        for name in sorted(mods, key=lambda m: (mods[m].get("id") if mods[m].get("id") is not None else 99, m)):
            m = mods[name]
            fo = m.get("failing_on") or []
            mo = m.get("missing_on") or []
            L.append(f"| {name} | {_cell(m.get('id'))} | {m.get('cases')} | {len(m.get('entries') or [])} | "
                     f"{m['pass']} | {m['fail']} | {m['skip']} | {m['untestable']} | {m['xfail']} | "
                     f"{m['engine_mismatch']} | {m['unstable']} | {_cell(', '.join(fo[:8]) + (' …' if len(fo) > 8 else ''))} | "
                     f"{_cell(', '.join(mo[:8]) + (' …' if len(mo) > 8 else ''))} |")
        L.append("")
        L.append("Counts are case results summed over entries. *Missing on*: entries that ran but never reported "
                 "the module (aborted before it, or a crash). *unstable*: intermittent in the narrowing reruns.")
        L.append("")
    cat = summ.get("catalogue") or {}
    if cat.get("rows"):
        t = cat["totals"]
        L.append("### Capability catalogue (docs/catalogue.yaml)")
        L.append("")
        L.append(f"{t.get('covered', 0)} covered, {t.get('partial', 0)} partial, {t.get('not_covered', 0)} not covered "
                 f"(of {len(cat['rows'])}). *covered*: a matching case passed or failed on a matching entry in this "
                 "run; *partial*: only skip/untestable, or the catalogue says only part of it is reachable; "
                 "*not_covered*: no matching case ran, or no test can reach it (reason given).")
        L.append("")
        L.append("| Capability | Area | Status | pass/fail/skip/untestable | Entries | Why |")
        L.append("|---|---|---|---|---|---|")
        for row in cat["rows"]:
            st = row["status"] if row["status"] == "covered" else f"**{row['status']}**"
            ents = row.get("entries") or []
            L.append(f"| `{row['id']}` {_cell(row.get('what'))} | {_cell(row.get('area'))} | {st} | "
                     f"{row['pass']}/{row['fail']}/{row['skip']}/{row['untestable']} | "
                     f"{len(ents)} | {_cell(row.get('why'))} |")
        L.append("")
    elif cat.get("error"):
        L.append(f"Capability catalogue: docs/catalogue.yaml could not be read: {cat['error']}")
        L.append("")
    L.append("### Entries")
    L.append("")
    L.append("| Entry | Machine | ROM | Engine | Status | As designed | pass/fail/skip/untestable | xfail | "
             "engine_mismatch | Records | Why |")
    L.append("|---|---|---|---|---|---|---|---|---|---|---|")
    for e in summ["entries"]:
        m = e.get("machine") or {}
        c = e["cases"]
        st = e["status"] if e["status"] not in BAD else f"**{e['status']}**"
        mach = " ".join(x for x in (m.get("model"), m.get("cpu"), "+" + m["fpu"] if m.get("fpu") not in ("none", None)
                                    else "", m.get("chipset"), m.get("video"), m.get("core")) if x)
        des = "yes" if e.get("as_designed") else ("**no: adjusted**" if e.get("adjusted") else "**unverified**")
        if e["status"] == "skip":
            des = "-"
        L.append(f"| {e['id']} | {_cell(mach)} | {_cell(e.get('rom'))} | {_cell(_engine(e))} | {st} | {des} | "
                 f"{c['pass']}/{c['fail']}/{c['skip']}/{c['untestable']} | {c['xfail']} | {c['engine_mismatch']} | "
                 f"{_cell(e.get('records'))} | {_cell(e.get('reason'))} |")
    L.append("")
    skips = [e for e in summ["entries"] if e["status"] == "skip"]
    if skips:
        L.append("### Skipped entries")
        L.append("")
        for e in skips:
            L.append(f"- `{e['id']}`: {e['reason']}")
        L.append("")
    adj = [r for r in results if r.get("adjusted")]
    if adj:
        L.append("### Configs adjusted by the emulator (not counted as run as designed)")
        L.append("")
        L.append("Amiberry resolved these options differently from what the config wrote (`--dump-config`, "
                 "src/main.cpp fixup_cpu/fixup_prefs). The entry ran, but not as the machine it was meant to be: its "
                 "results describe the adjusted machine.")
        L.append("")
        for r in adj:
            ch = "; ".join(f"`{d['option']}` {d['wrote']} -> {d['resolved']}"
                           + (f" (because: {' / '.join(d['because'])})" if d.get("because") else "")
                           for d in r["adjusted"])
            L.append(f"- `{r['id']}`: config adjusted by emulator: {ch}")
        L.append("")
    par = an.get("parity") or []
    if par:
        L.append("### Engine parity (twin vs reference)")
        L.append("")
        L.append("| Twin | Reference | Kind | Result |")
        L.append("|---|---|---|---|")
        for p in par:
            res = (f"{p.get('compared', 0)} cases compared, {p.get('mismatches', 0)} differ"
                   if p.get("status") == "compared" else f"not compared: {p.get('why')}")
            if p.get("not_comparable"):
                res += f"; {len(p['not_comparable'])} inconclusive observations (not parity coverage)"
            L.append(f"| {p['twin']} | {p['ref']} | {p.get('twin_kind')} | {_cell(res)} |")
        L.append("")
        for p in par:
            for c in p.get("not_comparable", []):
                L.append(f"- `{p['twin']}` / `{p['ref']}`, `{c['case_key']}`: "
                         f"{c['ref_st']} / {c['twin_st']}; {c['why']} (not compared).")
            for c in p.get("observations", []):
                fields = ", ".join(sorted(set(c["ref_fields"] + c["twin_fields"])))
                L.append(f"- `{p['twin']}` / `{p['ref']}`, `{c['case_key']}`: {c['why']}; "
                         f"normalized fields: {fields}.")
        L.append("")
    hosts = summ.get("host_requests") or []
    if hosts:
        L.append("### Host requests (CONTRACT §13)")
        L.append("")
        L.append("Requests the guest made of the host (screenshots, audio marks, input injection). A request the "
                 "host could not do is not acknowledged, and the guest reports the dependent case `untestable`. "
                 "The host records are in `records.jsonl` (`\"host\": true`) and `raw/<entry>/<disk>/host.jsonl`.")
        L.append("")
        for e in hosts:
            L.append(f"- `{e['entry']}`: {e['requests']} request(s), {e['acked']} acknowledged")
            for x in e["not_acked"][:10]:
                L.append(f"  - not acked: #{x['id']} `{x['what']}` {x['tag']!r}"
                         + (f" [{x['disk']}]" if x.get("disk") else "") + f": {x['why']}")
            for x in e["screenshots"][:10]:
                L.append(f"  - screenshot #{x['id']} {x['tag']!r}: `{x['path']}` sha256 `{(x.get('sha256') or '')[:16]}`")
            for x in e["audio"][:10]:
                ok = "" if x.get("freq_ok") is None else (" **ok**" if x["freq_ok"] else " **MISMATCH**")
                L.append(f"  - audio #{x['id']} {x['tag']!r}: {x.get('seconds')} s, dominant {x.get('dominant_hz')} Hz"
                         + (f" (expected {x['expect_hz']:.6g}){ok}" if x.get("expect_hz") else "")
                         + (f", side {x['expect_side']} {'ok' if x.get('side_ok') else '**WRONG**'}"
                            if x.get("expect_side") else "")
                         + f", per channel Hz {_j(x.get('freq'))} RMS {_j(x.get('rms'))}, `{x.get('wav')}`")
                t = x.get("tone")
                if t:
                    L.append(f"    - `{t['case']}` **{t['st']}**: {t['why']}")
        L.append("")
    if summ.get("hostspeed"):
        L.extend(_speed_lines(summ["hostspeed"]))
    if an.get("xfail") or an.get("fixed"):
        L.append("### Known bugs (xfail registry)")
        L.append("")
        for x in an.get("xfail", []):
            L.append(f"- xfail `{x['case_key']}` on `{x['entry']}`: {x['xfail'].get('ref')}: {x['xfail'].get('why')}")
        for x in an.get("fixed", []):
            L.append(f"- **fixed** `{x['case_key']}` passes on `{x['entry']}` although registered as "
                     f"{x['xfail'].get('ref')}: confirm and remove it from runner/amitorture/xfail.json")
        L.append("")
    # ---------------- problems
    normal = [p for p in problems if p.get("severity") != "low"]
    low = [p for p in problems if p.get("severity") == "low"]
    L.append("## Problems")
    L.append("")
    if not normal:
        L.append("None.")
        L.append("")
    for p in normal:
        L.extend(_render_problem(p, items_by_id, res_by_id, matrixmod))
    if low:
        L.append("## Notes (low confidence or informational, not in the verdict)")
        L.append("")
        for p in low:
            L.extend(_render_problem(p, items_by_id, res_by_id, matrixmod))
    nar = an.get("narrowing")
    if nar and (nar.get("exhausted") or nar.get("skipped")):
        L.append(f"Narrowing: {nar.get('skipped') or 'budget exhausted'} (used {nar.get('used_s')} s of "
                 f"{nar.get('budget_s')} s, {nar.get('launches')} launches; untried variants are listed per problem).")
        L.append("")
    paced = [(r["id"], s) for r in results for s in (r.get("disks") or [r]) if s.get("warp")]
    if paced:
        L.extend(["## Warp policy", "", "Pacing is selected before each fresh boot; no tests are removed.", "",
                  "| Entry | Disk | Policy | Warp | Runtime verified | Reason |",
                  "|---|---|---|---|---|---|"])
        for eid, sub in paced:
            w = sub["warp"]
            L.append(f"| {_cell(eid)} | {_cell(sub.get('disk_name'))} | {w['requested']} | "
                     f"{'on' if w['effective'] else 'off'} | {w.get('verified', False)} | {_cell(w['reason'])} |")
        L.append("")
    # ---------------- environment
    L.append("## Environment")
    L.append("")
    L.append("| | |")
    L.append("|---|---|")
    if a:
        L.append(f"| Emulator | `{_cell(a.get('version'))}` |")
        L.append(f"| Binary | `{_cell(a.get('path'))}` sha256 `{_cell(a.get('sha256'))}` |")
    if an.get("amiberry_src"):
        L.append(f"| Amiberry source (code pointers) | `{an['amiberry_src']}` |")
    h = env.get("host") or {}
    if h:
        L.append(f"| Host | {_cell(h.get('os'))} {_cell(h.get('release'))} {_cell(h.get('machine'))}"
                 f"{' (' + _cell(h.get('board')) + ')' if h.get('board') else ''}"
                 f"{', ' + _cell(h.get('distro')) if h.get('distro') else ''} |")
        L.append(f"| Python | {_cell(h.get('python'))} |")
    t = env.get("amitorture") or {}
    if t:
        L.append(f"| amitorture | rev `{_cell(t.get('rev'))}`{' (dirty)' if t.get('dirty') else ''} |")
    d = env.get("disk_source")
    if d:
        L.append(f"| Test disk | `{_cell(d.get('path'))}` build `{_cell(d.get('build_id'))}` sha256 `{_cell(d.get('sha256'))}` |")
    elif env.get("options", {}).get("no_disk"):
        L.append("| Test disk | none (--no-disk" + (", --selftest" if env["options"].get("selftest") else "") + ") |")
    if env.get("params"):
        L.append(f"| Run params | `{_cell(_j(env['params']))}` |")
    o = env.get("options") or {}
    if o.get("matrix"):
        L.append(f"| Matrix | `{o.get('matrix')}`" + (f", --config {o.get('configs')}" if o.get("configs") else "")
                 + (f", --set {o.get('sets')}" if o.get("sets") else "") + " |")
    pl = env.get("plan")
    if pl:
        L.append(f"| Plan | {pl.get('plan_entries')} of {pl.get('entries')} entries, {pl.get('launches_plan')} disk launches "
                 f"(old-style {pl.get('launches_old')}), each of {pl.get('keys')} coverage keys once "
                 f"(runner/amitorture/plan.py) |")
    if o.get("module_warp") == "auto":
        L.append("| Pacing | per module (runner/amitorture/pacing.py): see each disk's warp.modules |")
    iso = summ["isolation"]
    L.append(f"| Isolation | {'OK: user Amiberry dirs unchanged' if iso['ok'] else '**VIOLATED**'} |")
    L.append("")
    roms = [r for r in env.get("roms", []) if r.get("status") != "not a rom (size)"]
    if roms:
        L.append("ROMs found (identified by CRC32/SHA1, not by name):")
        L.append("")
        L.append("| File | Status | Identified as | CRC32 |")
        L.append("|---|---|---|---|")
        for r in roms:
            name = (r.get("rom") or {}).get("name") or (
                f"header says {r['header']['version']}" if r.get("header") else "")
            L.append(f"| {_cell(r.get('file'))} | {_cell(r.get('status'))} | {_cell(name)} | `{_cell(r.get('crc32'))}` |")
        L.append("")
    if not iso["ok"]:
        L.append("## ISOLATION VIOLATED")
        L.append("")
        L.append("The user's real Amiberry directories changed during the run. The runner never writes there, so "
                 "either the emulator ignored the isolated environment, or the user ran Amiberry meanwhile:")
        L.append("")
        for c in iso["changes"]:
            if c["severity"] == "fail":
                L.append(f"- {c['change']}: `{c['path']}`")
        L.append("")
        seen = iso.get("user_amiberry_seen") or []
        if seen:
            L.append("Likely cause: while the run was going, these Amiberry processes ran on the user's real "
                     "environment (HOME is the user's home, so they use the real config). amitorture's own emulators "
                     "run with HOME inside the work dir:")
            L.append("")
            for u in seen:
                L.append(f"- pid {u['pid']}, seen {u['first_seen']}-{u.get('last_seen')}: `{_cell(u['cmd'])}`")
            L.append("")
        else:
            L.append("No Amiberry on the user's own environment was seen during the run (sampled every 5 s), so the "
                     "emulator under test is the likely cause.")
            L.append("")
    # transport details
    L.append("## Transports and process (per entry)")
    L.append("")
    L.append("| Entry | Serial | Mailbox | IPC | Stopped by | Exit |")
    L.append("|---|---|---|---|---|---|")
    rows = []
    for r in results:
        for sub in (r.get("disks") or [r]):
            rows.append((r["id"] + (f" [{sub['disk_name']}]" if sub.get("disk_name") else ""), sub))
    for rid, r in rows:
        if r["status"] == "skip" or "process" not in r:
            continue
        s, mb, ipc, pr = r.get("serial", {}), r.get("mailbox", {}), r.get("ipc", {}), r.get("process", {})
        ser = (f"port {s.get('port')}, connected {'yes' if s.get('connected_after_s') is not None else 'NO'}"
               f"{' (accepted in log)' if s.get('accepted_in_log') else ''}, {s.get('bytes')} B")
        mbs = (f"{'at ' + mb['found_at'] if mb.get('found_at') else 'not found'}, {mb.get('scans')} scan(s) "
               f"{mb.get('scan_reads')} reads {mb.get('scan_seconds')} s, {mb.get('bytes')} B, lost {mb.get('lost_bytes')}")
        noise = ((r.get("records") or {}).get("noise") or {}).get("serial") or {}
        if noise.get("lines"):
            first = (noise.get("samples") or [{}])[0].get("text", "")
            ser += f", {noise['lines']} non-record line(s) e.g. {first[:60]!r}"
        ex = pr.get("exit_before_stop", {})
        exs = "running" if ex.get("running") else (ex.get("signal") or f"code {ex.get('returncode')}")
        L.append(f"| {rid} | {_cell(ser)} | {_cell(mbs)} | `{_cell(ipc.get('endpoint'))}` {ipc.get('requests', '')} req "
                 f"| {_cell(pr.get('stop_method'))} | {_cell(exs)} before stop |")
    L.append("")
    if any(r.get("selftest") for r in results):
        L.append("Selftest: a synthetic mailbox was written into guest chip RAM over IPC (emulation paused) and read "
                 "back through the normal scan/ring path, in two steps so that the ring wraps:")
        L.append("")
        for r in results:
            if r.get("selftest"):
                L.append(f"- {r['id']}: `{_j(r['selftest'])}`")
        L.append("")
    L.append("## Files")
    L.append("")
    L.append("- `summary.json`: verdict, per-entry status/counts, module catalogue, parity, problems")
    L.append("- `failures.jsonl`: one failure per line with full context (records before it, hello, config)")
    L.append("- `narrowing.json`: every narrowing launch and the per-case flip/hold analysis")
    L.append("- `repro/<problem>/`: case.adf, config.uae, run.sh, gdb.cmd, host-gdb.cmd, pack.json")
    L.append("- `environment.json`: binary version/sha256, ROM identities, host, amitorture rev, isolation check")
    L.append("- `configs/<entry>.uae`: the config as written; `configs/<entry>.resolved.uae`: as Amiberry resolved it")
    L.append("- `raw/<entry>/`: `amiberry.log` (timestamped console), `serial.bin` (raw serial bytes), "
             "`mailbox.jsonl` (scans and ring reads, base64), `records.jsonl` (merged records), `result.json`")
    L.append("")
    return "\n".join(L)


def _render_problem(p, items_by_id, res_by_id, matrixmod):
    L = [f"### {p['id']}: {p['title']}", ""]
    rep = items_by_id.get(p["representative"]) or {}
    r = res_by_id.get(rep.get("entry")) or {}
    L.append(f"- kind: `{p['kind']}`; module: `{p.get('mod')}`; severity: {p.get('severity')}")
    if p.get("cases"):
        ks = p["case_keys"]
        L.append(f"- scope: {p['cases']} case(s) on {len(p['entries'])} entr{'y' if len(p['entries']) == 1 else 'ies'}: "
                 f"cases {', '.join('`' + k + '`' for k in ks[:20])}{' …' if len(ks) > 20 else ''}")
    L.append(f"- fails on: {', '.join(p['entries'])}")
    if p.get("engine_mismatch_also"):
        L.append(f"- the same case(s) also differ from the twin's reference entry (engine_mismatch "
                 f"{', '.join(p['engine_mismatch_also'][:10])} in failures.jsonl): the failure is engine-specific")
    L.append(f"- passes on: {', '.join(p.get('passes_on') or []) or '(none)'}")
    pdf = p.get("passes_diff") or {}
    if pdf.get("not_run_or_skipped_on"):
        L.append(f"- not run or skipped on: {', '.join(pdf['not_run_or_skipped_on'])}")
    if r.get("options"):
        L.append(f"- representative config ({rep.get('entry')}): {matrixmod.summary_line(r['options'])}; "
                 f"file `{(r.get('config') or {}).get('file')}`")
    if r.get("adjusted"):
        L.append(f"- NOTE: this entry's config was adjusted by the emulator: "
                 + "; ".join(f"{d['option']} {d['wrote']}->{d['resolved']}" for d in r["adjusted"]))
    L.append("")
    L.extend(_render_diagnosis(p))
    if rep:
        L.append("**Representative failure**")
        L.append("")
        L.extend(_render_case_item(rep, p))
    nar = p.get("narrowing")
    if nar:
        L.append("**Narrowing** (only this module rerun, same seed, one setting changed at a time)")
        L.append("")
        L.append(f"- result: {nar['statement']}")
        L.append(f"- determinism: {nar['determinism']} (fails in {nar['fail_rate']} reruns of the same config)")
        if nar.get("flips"):
            L.append("- flips to pass: " + ", ".join(f"`{x['axis']}={x['label']}`" for x in nar["flips"]))
        if nar.get("holds"):
            L.append("- still fails: " + ", ".join(f"`{x['axis']}={x['label']}`" for x in nar["holds"]))
        if nar.get("skipped_noop"):
            L.append("- not launched (no effect on this machine): " + ", ".join(f"`{x}`" for x in nar["skipped_noop"]))
        if nar.get("inconclusive"):
            L.append("- inconclusive: " + ", ".join(f"`{x['axis']}={x['label']}` ({x.get('why')})"
                                                   for x in nar["inconclusive"]))
        others = {k: v for k, v in (p.get("narrowing_by_case") or {}).items() if v and v is not nar}
        if len(others) > 1:
            L.append("- per case:")
            for k, v in list(others.items())[:30]:
                L.append(f"  - `{k}`: {v['statement']} [{v['determinism']}, {v['fail_rate']}]")
        L.append("")
    if p.get("pointers"):
        L.append("**Where to look in the emulator** (hints from the config and opcode; docs/emulator-map/amiberry.md)")
        L.append("")
        if p.get("core") and p["core"].get("table_name"):
            c = p["core"]
            L.append(f"- CPU core: `{c['table_name']}` ({c['mode_name']}, cputbls[{c['lvl']}][{c['mode']}] in "
                     f"src/newcpu.cpp build_cpufunctbl), handlers in `src/{c['file']}`"
                     + (f"; JIT compiled code: `src/{c['jit_compiled']['file']}`" if c.get("jit_compiled") else ""))
        for ptr in p["pointers"]:
            loc = f"`{ptr.get('file')}`" + (f":{ptr['line']}" if ptr.get("line") else "")
            fn = f" `{ptr['function']}`" if ptr.get("function") else ""
            L.append(f"- {loc}{fn}: {ptr.get('why')} [{ptr.get('confidence')}]")
        L.append("")
    rp = p.get("repro")
    if rp:
        L.append("**Repro pack**")
        L.append("")
        if rp.get("error"):
            L.append(f"- could not be written: {rp['error']}")
        else:
            L.append(f"- `{rp['dir']}`: {', '.join(rp['files'])}")
            L.append(f"- rerun: `{rp['run']}` (prints the case result; exit 0 = passes now). Guest debugging: "
                     "`./run.sh --gdb` then `m68k-amigaos-gdb -x gdb.cmd`; host debugging: `gdb -x host-gdb.cmd`.")
            if rp.get("adf_error"):
                L.append(f"- case.adf: {rp['adf_error']}")
        L.append("")
    if p.get("history"):
        hs = [h for h in p["history"] if h.get("baseline") or h.get("xfail")]
        if hs:
            L.append("**History**")
            L.append("")
            for h in hs[:20]:
                L.append(f"- `{h['case_key']}` on {h['entry']}: "
                         + "; ".join(x for x in (h.get("baseline"), f"xfail {h['xfail']['ref']}" if h.get("xfail")
                                                 else None) if x))
            L.append("")
    rest = [f for f in p["failures"] if f != p["representative"]]
    if rest:
        L.append(f"**Other failures in this problem** ({len(rest)})")
        L.append("")
        for fid in rest[:100]:
            it = items_by_id.get(fid) or {}
            L.append(f"- {fid} `{it.get('case_key') or it.get('kind')}` on {it.get('entry')}: {_cell(it.get('why'))}"
                     + (f" expect `{_j(it.get('expect'), 160)}` got `{_j(it.get('got'), 160)}`"
                        if it.get("kind") == "case_fail" else ""))
        if len(rest) > 100:
            L.append(f"- … {len(rest) - 100} more in failures.jsonl")
        L.append("")
    return L


def _render_diagnosis(p):
    """Cases with their docs, config settings, pass/fail separation, emulator log."""
    L = []
    ct = p.get("case_table") or []
    if ct:
        L.append("**Cases in this problem** (docs/cases/<module>.md: intent, rule, what a failure means)")
        L.append("")
        undefined = {}
        for c in ct:
            ents = ", ".join(c["entries"][:6]) + (" …" if len(c["entries"]) > 6 else "")
            doc = c.get("doc") or "(no line for this case in docs/cases; ask the module owner)"
            L.append(f"- `{c['case_key']}` on {ents}: {doc}" + (f" [{c['doc_where']}]" if c.get("doc_where") else ""))
            L.append(f"  - evidence: {c['evidence']}" if c.get("evidence") else
                     "  - evidence: none given (no real-hardware evidence recorded for this expectation; module docs "
                     "may add an `evidence:` field)")
            for f in c.get("undefined_fields") or []:
                undefined.setdefault(f, []).append(c["case_key"])
        defined = {}
        for c in ct:
            defined.update(c.get("defined_fields") or {})
        if defined:
            L.append("- fields explained in docs/cases: " + "; ".join(f"`{f}` ({w})" for f, w in sorted(defined.items())[:15]))
        if undefined:
            L.append("- fields of these records that docs/cases does not define (module owners: please document): "
                     + "; ".join(f"`{f}` ({', '.join(ks[:3])}{' …' if len(ks) > 3 else ''})"
                                 for f, ks in sorted(undefined.items())))
        L.append("")
    cf = p.get("config_flags") or {}
    if cf.get("per_entry"):
        L.append(f"**Settings that change what `{p.get('mod')}` cases may expect** (resolved config of each failing "
                 "entry)")
        L.append("")
        names = cf["options"]
        L.append("| Entry | " + " | ".join(f"`{n}`" for n in names) + " |")
        L.append("|---|" + "---|" * len(names))
        for e in cf["per_entry"]:
            L.append(f"| {e['entry']} | " + " | ".join(_cell(e["values"].get(n)) for n in names) + " |")
        L.append("")
        for c in cf.get("conflicts") or []:
            ks = c["cases"]
            head = (f"{len(ks)} case(s) ({', '.join('`' + k + '`' for k in ks[:4])}{' …' if len(ks) > 4 else ''}) "
                    f"name `{c['option']}={c['case_names']}` in their why/docs; {', '.join(c['entries'])} ran with "
                    f"`{c['option']}={c['config_has']}`")
            if c.get("precondition"):
                L.append(f"- **config contradicts the case's precondition**: {head}. With this setting the case is "
                         "expected to fail: a precondition of the test, not necessarily an emulator bug "
                         f"(try `--set {c['option']}={c['case_names']}`)")
            else:
                L.append(f"- **the case's own explanation does not fit this config**: {head}, so the text does not "
                         "explain the failure here; trust the pass/fail pattern below over the note")
        if cf.get("conflicts"):
            L.append("")
    pdf = p.get("passes_diff") or {}
    if pdf.get("passes_on"):
        L.append("**Where the same cases pass, and what differs**")
        L.append("")
        if pdf.get("separating"):
            for s in pdf["separating"]:
                L.append(f"- `{s['option']}`{' (changes this module)' if s.get('affects_module') else ''}: every "
                         f"failing entry has {', '.join(s['failing'])}; every passing entry has {', '.join(s['passing'])}")
        else:
            L.append("- no single setting separates all failing entries from all passing ones")
        for w in pdf.get("within") or []:
            L.append(f"- among the passing entries with `{w['among']}` ({', '.join(w['passing'])}): "
                     + ("; ".join(f"`{x['option']}` {', '.join(x['failing'])} (fail) vs {', '.join(x['passing'])} "
                                  "(pass)" for x in w["separating"]) or "no setting separates them"))
        for n in pdf.get("nearest") or []:
            L.append(f"- nearest passing entry `{n['entry']}` vs `{n['vs']}` ({n['n']} differences): "
                     + ", ".join(f"`{d['option']}` {d['failing']}→{d['passing']}" for d in n["differences"]))
        L.append("")
    elog = p.get("emulator_log") or []
    if elog:
        L.append("**Emulator log while the module ran** (amiberry.log lines classified by runner/amitorture/logmine.py)")
        L.append("")
        for x in elog:
            how = ("exact module window (serial arrival times)" if x.get("exact") else
                   f"approximate window from {x.get('time_source')} reads"
                   + ("; some lines may belong to a neighbouring module" if x.get("ambiguous") else ""))
            L.append(f"- `{x['entry']}`" + (f" [{x['disk']}]" if x.get("disk") else "") + f" ({how}; `{x['raw']}`): "
                     + ", ".join(f"{k} {v}" for k, v in sorted(x["counts"].items())))
            for g in x["groups"]:
                L.append(f"  - {g['count']}× [{g['category']}] `{g['pattern']}`"
                         + (f" addresses {', '.join(g['addresses'][:6])}" if g.get("addresses") else "")
                         + (f" (modules {', '.join(g['modules'])})" if len(g.get("modules") or []) > 1 else ""))
                L.append(f"    e.g. `{g['examples'][0]}`")
            if x.get("more"):
                L.append(f"  - … {x['more']} more line patterns")
        L.append("")
    return L


def _render_case_item(it, p):
    if it.get("kind") not in ("case_fail", "engine_mismatch", "fpu_mode_difference"):
        return _render_item(it)
    L = [f"- {it['id']} on `{it['entry']}`: case `{it.get('case_key')}`"]
    doc = (p.get("docs") or {}).get(it.get("case_key"))
    if doc and doc.get("text"):
        L.append(f"- intent / rule ({doc['file']}:{doc['line']}): {doc['text']}")
    elif doc:
        L.append(f"- intent / rule: no line for this case in {doc['file']}")
    else:
        L.append(f"- intent / rule: docs/cases/{it.get('mod')}.md not found")
    L.append(f"- why: {it.get('why')}")
    if it["kind"] == "case_fail":
        if it.get("in") is not None:
            L.append(f"- in: `{_j(it['in'], None)}`")
        L.append(f"- expect: `{_j(it.get('expect'), None)}`")
        L.append(f"- got: `{_j(it.get('got'), None)}`")
        if it.get("ctx") is not None:
            L.append(f"- ctx: `{_j(it['ctx'], None)}`")
        L.append(f"- record: `{_j(it.get('record'), None)}` (seq {it.get('seq')}, boot {it.get('boot')}, via "
                 f"{', '.join(it.get('sources') or [])})")
        if it.get("hello"):
            L.append(f"- guest: `{_j(it['hello'])}`")
        if it.get("preceding"):
            L.append("- records before it:")
            L.append("")
            L.append("  ```")
            for x in it["preceding"]:
                L.append("  " + _j(x, 300))
            L.append("  ```")
    else:
        L.append(f"- {it.get('ref_entry')} (reference): `{_j(it.get('ref'), None)}`")
        L.append(f"- {it.get('entry')} (twin): `{_j(it.get('twin'), None)}`")
        if it.get("paths"):
            L.append("- differing fields: " + "; ".join(f"`{x['path']}` {_j(x['ref'], 80)} vs {_j(x['twin'], 80)}"
                                                        for x in it["paths"][:12]))
        if it.get("differing_options"):
            L.append("- the two entries differ only in: " + ", ".join(
                f"`{d['option']}` {d['ref']} vs {d['twin']}" for d in it["differing_options"]))
    L.append(f"- config: `{it.get('config')}`; raw: `raw/{it['entry']}/`")
    L.append("")
    return L


def _render_item(it):
    L = [f"#### {it['id']} `{it['kind']}` on {it['entry']}"]
    L.append("")
    if it.get("why"):
        L.append(f"- why: {it['why']}")
    if it["kind"] == "case_fail":
        L.append(f"- case: `{it['mod']}/{it['case']}` (seq {it['seq']}, boot {it['boot']}, via {', '.join(it['sources'])})")
        L.append(f"- expect: `{_j(it.get('expect'))}`")
        L.append(f"- got: `{_j(it.get('got'))}`")
        if it.get("in") is not None:
            L.append(f"- in: `{_j(it['in'])}`")
        if it.get("ctx") is not None:
            L.append(f"- ctx: `{_j(it['ctx'])}`")
        if it.get("hello"):
            L.append(f"- guest: `{_j(it['hello'])}`")
        if it.get("preceding"):
            L.append("- records before it:")
            L.append("")
            L.append("  ```")
            for p in it["preceding"]:
                L.append("  " + _j(p, 300))
            L.append("  ```")
    elif it["kind"] in ("crash", "hang", "timeout", "error", "incomplete"):
        if it.get("exit"):
            L.append(f"- process: `{_j(it['exit'])}`")
        if it.get("mailbox"):
            L.append(f"- last mailbox header: `{_j(it['mailbox'])}`")
        if it.get("hang"):
            L.append(f"- hang: `{_j(it['hang'])}`")
        if it.get("last_records"):
            L.append("- last records:")
            L.append("")
            L.append("  ```")
            for p in it["last_records"]:
                L.append("  " + _j(p, 300))
            L.append("  ```")
        if it.get("log_tail"):
            L.append("- emulator log tail:")
            L.append("")
            L.append("  ```")
            for line in it["log_tail"][-25:]:
                L.append("  " + line[:300])
            L.append("  ```")
    elif it["kind"] == "thread_speed":
        L.append(f"- block: `{it.get('case_key')}`" + (f" [{it['disk']}]" if it.get("disk") else ""))
        L.append(f"- {it.get('ref_entry')} (reference, unthreaded): `{_j(it.get('ref'))}`")
        L.append(f"- {it.get('entry')} (threaded twin): `{_j(it.get('twin'))}`")
        L.append("- the two entries differ only in: `cpu_threaded` false vs true; same binary, run and host")
        if it.get("host"):
            L.append(f"- host: {it['host']}")
    else:
        rest = {k: v for k, v in it.items() if k not in ("id", "kind", "entry", "why", "jit", "twin_of", "machine",
                                                          "rom", "config")}
        L.append(f"- details: `{_j(rest, 1200)}`")
    L.append(f"- config: `{it.get('config')}`; raw: `raw/{it['entry']}/`")
    L.append("")
    return L


# ---------------------------------------------------------------------------
# import (captures made elsewhere: real hardware, FPGA)

def import_files(paths, outdir, label="import"):
    """Builds the same report from serial captures and/or mailbox dumps.

    Accepted inputs:
      * a raw serial capture (bytes; non-record lines are noise),
      * a mailbox.jsonl written by `run` (events with data_b64),
      * a raw ring/text dump (treated like serial).
    All files describe one machine. Each boot in a file is one disk run
    (one long capture across several disks works); runs of the same disk
    found in several files (serial + mailbox of one boot) are merged as
    transports; different disks are combined like `run` combines a set.
    """
    import base64
    streams = {}
    env = {"run_id": label, "imported": [os.path.abspath(p) for p in paths]}
    parsers = []
    for p in paths:
        with open(p, "rb") as fh:
            data = fh.read()
        name = os.path.basename(p)
        src = f"import:{name}"
        if src in streams:            # several captures with the same file name
            src = f"import:{p}"
        k = 2
        while src in streams:
            src, k = f"import:{p}#{k}", k + 1
        sp = recmod.StreamParser(src)
        if _is_mailbox_jsonl(data):
            for line in data.splitlines():
                try:
                    ev = json.loads(line)
                except ValueError:
                    continue
                if ev.get("event") == "data":
                    if ev.get("lost"):
                        sp.lose(ev["lost"], ev.get("lost_at", 0), "ring overrun")
                    sp.feed(base64.b64decode(ev.get("data_b64", "")))
                elif ev.get("event") == "vanished":
                    sp.lose(0, sp.splitter.base, "mailbox vanished")
        else:
            sp.feed(data)
        sp.close()
        streams[src] = sp.records
        parsers.append(sp)
    from . import runner as runmod
    groups = _import_groups(streams)
    if not groups:
        groups = [("capture", {}, [])]
    noise = {sp.source: {"lines": sp.noise_lines, "samples": sp.noise_samples[:10]} for sp in parsers}
    subs = []
    for index, (name, gstreams, alignment_issues) in enumerate(groups, 1):
        m = recmod.merge(gstreams)
        m.schema_problems.extend(alignment_issues)
        d = m.by_type("done")
        sub = {"disk_name": name, "_merged": m, "records": recmod.summarize(m)}
        if len(groups) > 1:
            sub["raw"] = f"raw/imported/disks/{index:03d}/"
            rawdir = os.path.join(outdir, sub["raw"])
            os.makedirs(rawdir, exist_ok=True)
            with open(os.path.join(rawdir, "records.jsonl"), "w", encoding="utf-8") as fh:
                for rec in m.records:
                    fh.write(json.dumps(rec) + "\n")
        sub["status"], sub["reason"] = runmod.classify(sub, m, "done" if d else "timeout")
        if not d:
            # a capture has no emulator that timed out: the capture stopped,
            # or the machine hung, before this disk finished
            sub["reason"] = ("no done record: the capture stopped, or the machine hung, before this disk finished"
                             if m.records else "no records in the capture (did the disk boot? baud rate?)")
        sub["ended"] = "done" if d else "capture_ended"
        subs.append(sub)
    merged = runmod.merge_disks(subs) if len(subs) > 1 else subs[0]["_merged"]
    hellos = [(s["disk_name"], s["records"]["hello"]) for s in subs]
    hello = [h for _, h in hellos if h]
    probe = (hello[0].get("probe") if hello else None) or {}
    r = {"id": "imported", "title": "imported capture: " + ", ".join(os.path.basename(p) for p in paths),
         "jit": False, "twin_of": None,
         "machine": {"cpu": probe.get("cpu") or (hello[0].get("cpu") if hello else None),
                     "fpu": probe.get("fpu") or (hello[0].get("fpu") if hello else None),
                     "chipset": probe.get("chipset")},
         "rom": {"name": f"kick {probe.get('kick')}" if probe.get("kick") else None},
         "records": recmod.summarize(merged) | {
                     "per_disk": {s["disk_name"]: s["records"]["merged"] for s in subs},
                     "noise": noise},
         "_merged": merged, "raw": "raw/imported/"}
    if len(subs) > 1:
        r["disks"] = subs
        r["probe_consistency"] = runmod.probe_consistency(hellos)
        worst = min(subs, key=lambda s: runmod.STATUS_RANK.get(s["status"], 3))
        r["status"] = worst["status"]
        bad = [s for s in subs if s["status"] != "pass"]
        r["reason"] = ("; ".join(f"{s['disk_name']}: {s['status']} ({s['reason']})" for s in bad)
                       if bad else f"done, no failures ({len(subs)} disks)")
        r["ended"] = worst["ended"]
    else:
        r["status"], r["reason"], r["ended"] = subs[0]["status"], subs[0]["reason"], subs[0]["ended"]
    # The probe cannot be checked against a config here: drop that comparison.
    r["machine_from_probe"] = True
    os.makedirs(os.path.join(outdir, "raw", "imported"), exist_ok=True)
    with open(os.path.join(outdir, "raw", "imported", "records.jsonl"), "w", encoding="utf-8") as fh:
        for m in merged.records:
            fh.write(json.dumps(m) + "\n")
    return write_all(outdir, env, [r], source="import")


def _import_groups(streams):
    """Split by disk build, then reconcile each disk's source-local boot histories."""
    from dataclasses import replace
    groups = {}
    for src, recs in streams.items():
        by_boot = {}
        for rec in recs:
            by_boot.setdefault(rec.boot, []).append(rec)
        for boot in sorted(by_boot):
            brecs = by_boot[boot]
            build = next((x.obj.get("build") for x in brecs
                          if x.crc_ok and x.obj and x.obj.get("t") == "hello"), None)
            if build is None:
                key = ("", src, boot)
            else:
                key = (build,)
            groups.setdefault(key, {}).setdefault(src, []).extend(brecs)
    out, names = [], set()
    for key, gstreams in groups.items():
        maps, issues = recmod.boot_alignment(gstreams)
        boots = {}
        for src, recs in gstreams.items():
            for r in recs:
                boot = maps[src][r.boot]
                boots.setdefault(boot, {}).setdefault(src, []).append(replace(r, boot=0))
        for index, boot in enumerate(sorted(boots)):
            if key[0] == "":
                base = f"{key[1].split(':', 1)[-1]}#{key[2] + 1}"
            else:
                words = str(key[0]).split()
                base = (words[-1] if words else "disk") + (f"#{index + 1}" if index else "")
            name, k = base, 2
            while name in names:
                name, k = f"{base}-{k}", k + 1
            names.add(name)
            out.append((name, boots[boot], [dict(p, boot=0) for p in issues if p["boot"] == boot]))
    return out


def _is_mailbox_jsonl(data):
    first = data.lstrip()[:200]
    return first.startswith(b"{\"t\"") and b"\"event\"" in first
