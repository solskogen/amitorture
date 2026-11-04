"""Known emulator bugs (xfail registry, runner/amitorture/xfail.json).

Format: {"<mod>.<case>": {"ref": "...", "why": "...", "configs": "glob",
                          "when": {"option": "value", ...}, "tbd": true}}
  key      case key; fnmatch patterns are allowed ("timing.ce_*")
  configs  fnmatch glob on the entry id (default "*"); several globs may be
           given separated by spaces or as a list
  when     optional: every listed Amiberry option must have that value ("!v": any value but v) in
           the entry's config (e.g. {"fpu_softfloat": "false"})
  tbd      the case id is a placeholder until the module lands

A key may hold a list of such entries when one case has several independent
known causes (e.g. one bug on 68000 machines, another on PAL): the first
entry whose configs/when match is the one that applies.

Keys starting with "_" are comments. A failing case that matches is
`xfail` (expected, reported but not a problem); a passing case that matches
is `fixed` (the bug seems gone: remove the entry after confirming).
"""
from __future__ import annotations

import fnmatch
import json
import os

DEFAULT_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "xfail.json")


def load(path=None):
    path = path or DEFAULT_PATH
    if not os.path.exists(path):
        return {}
    with open(path, encoding="utf-8") as fh:
        doc = json.load(fh)
    return {k: v for k, v in doc.items() if not k.startswith("_") and isinstance(v, (dict, list))}


def _globs(v):
    if v is None:
        return ["*"]
    if isinstance(v, str):
        return v.split() or ["*"]
    return list(v) or ["*"]


def match(registry, key, entry_id, options=None):
    """The registry entry (with its key as "key") for this case, or None."""
    options = options or {}
    base_key = key.split("#", 1)[0]
    def ok(o, v):
        have, want = str(options.get(o, "")).lower(), str(v).lower()
        if want.startswith("!"):           # "!0": any value except 0
            return have != want[1:] and not (want[1:] == "0" and have in ("", "false"))
        return have == want
    for k, xs in registry.items():
        if not (k == base_key or fnmatch.fnmatchcase(base_key, k)):
            continue
        for x in (xs if isinstance(xs, list) else [xs]):
            if not isinstance(x, dict):
                continue
            if not any(fnmatch.fnmatchcase(entry_id, g) for g in _globs(x.get("configs"))):
                continue
            if not all(ok(o, v) for o, v in (x.get("when") or {}).items()):
                continue
            return dict(x, key=k)
    return None


def classify(registry, fps_by_entry, results):
    """Marks xfail / fixed cases. Returns (xfails, fixed):
    lists of {entry, case_key, mod, case, st, xfail}."""
    by_id = {r["id"]: r for r in results}
    xf, fixed = [], []
    for eid, fp in fps_by_entry.items():
        opts = (by_id.get(eid) or {}).get("options") or {}
        for key, c in fp["cases"].items():
            m = match(registry, key, eid, opts)
            if not m:
                continue
            item = {"entry": eid, "case_key": key, "mod": c["mod"], "case": c["case"], "st": c["st"], "xfail": m}
            if c["st"] == "fail":
                xf.append(item)
            elif c["st"] == "pass":
                fixed.append(item)
    return xf, fixed
