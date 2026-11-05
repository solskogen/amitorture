"""The coverage catalogue (docs/catalogue.yaml): every capability of every
Amiga model and expansion, mapped to the cases that test it, and its status
in one run (covered / partial / not_covered), so gaps are as visible in
REPORT.md as failures.

The file is a small YAML subset (stdlib only, no PyYAML), a list of flat
mappings:

    - id: copper.danger_bit
      area: Copper
      what: the danger bit (COPCON CDANG)
      cases: [copper.*dang*]          # fnmatch globs over "<mod>.<case>" keys
      entries: [*-rtg-picasso4*]      # optional: only cases from these entries count
      parity: jit                    # optional: needs a completed engine comparison
      partial: why only partly        # optional: covered at best partly
      not_covered: why                # optional: no test can cover it (stated reason)
      needs: what hardware/config it needs (text)

Scalars may be quoted ('...' or "..."); lists are [a, b] on one line;
'#' starts a comment outside quotes.

Status per run:
  not_covered  the catalogue says so (reason), or no case matching ``cases``
               was reported by a matching entry in this run
  partial      matching cases ran but all were skip/untestable, or the
               catalogue marks the capability ``partial``
  covered      at least one matching case passed or failed (= was tested)
"""
from __future__ import annotations

import fnmatch
import os

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT = os.path.abspath(os.path.join(HERE, "..", "..", "docs", "catalogue.yaml"))
FIELDS = ("id", "area", "what", "cases", "entries", "partial", "not_covered", "needs", "parity")


def _strip_comment(line):
    out, q = [], None
    for ch in line:
        if q:
            if ch == q:
                q = None
        elif ch in "'\"":
            q = ch
        elif ch == "#":
            break
        out.append(ch)
    return "".join(out).rstrip()


def _scalar(v):
    v = v.strip()
    if len(v) >= 2 and v[0] == v[-1] and v[0] in "'\"":
        return v[1:-1]
    return v


def _value(v):
    v = v.strip()
    if v.startswith("[") and v.endswith("]"):
        inner = v[1:-1].strip()
        if not inner:
            return []
        parts, cur, q = [], [], None
        for ch in inner:
            if q:
                if ch == q:
                    q = None
                cur.append(ch)
            elif ch in "'\"":
                q = ch
                cur.append(ch)
            elif ch == ",":
                parts.append("".join(cur))
                cur = []
            else:
                cur.append(ch)
        parts.append("".join(cur))
        return [_scalar(p) for p in parts if p.strip()]
    return _scalar(v)


def parse(text):
    """The catalogue items (list of dicts). Raises ValueError on bad lines."""
    items, cur = [], None
    for n, raw in enumerate(text.splitlines(), 1):
        line = _strip_comment(raw)
        if not line.strip():
            continue
        s = line.lstrip()
        if s.startswith("- "):
            cur = {}
            items.append(cur)
            s = s[2:]
        elif cur is None:
            raise ValueError(f"catalogue line {n}: expected '- id: ...'")
        if ":" not in s:
            raise ValueError(f"catalogue line {n}: expected 'key: value'")
        k, v = s.split(":", 1)
        k = k.strip()
        if k not in FIELDS:
            raise ValueError(f"catalogue line {n}: unknown key {k!r}")
        cur[k] = _value(v)
    ids = set()
    for it in items:
        if "id" not in it:
            raise ValueError(f"catalogue item without id: {it}")
        if it["id"] in ids:
            raise ValueError(f"catalogue id {it['id']} twice")
        ids.add(it["id"])
        for k in ("cases", "entries"):
            if k in it and isinstance(it[k], str):
                it[k] = [it[k]]
        if not it.get("cases") and not it.get("not_covered"):
            raise ValueError(f"catalogue item {it['id']}: needs cases or not_covered")
        if "parity" in it and it["parity"] not in ("jit", "threaded", "softfloat"):
            raise ValueError(f"catalogue item {it['id']}: unknown parity kind {it['parity']!r}")
    return items


def load(path=None):
    p = path or DEFAULT
    if not os.path.isfile(p):
        return None
    with open(p, encoding="utf-8") as fh:
        return parse(fh.read())


def _match(key, pats):
    return any(fnmatch.fnmatchcase(key, p) for p in pats)


def coverage(items, results, fps, parity_info=None):
    """[{id, area, what, status, why, tested, pass, fail, skip, untestable,
    entries, cases}] for one run. results: entry results; fps: {entry id:
    cases.extract(result)}."""
    ran = [r for r in results if r.get("status") not in ("skip",)]
    if any(it.get("parity") for it in items) and parity_info is None:
        from . import parity
        _, parity_info = parity.check_all(results, fps)
    out = []
    for it in items:
        row = {"id": it["id"], "area": it.get("area"), "what": it.get("what"), "pass": 0, "fail": 0, "skip": 0,
               "untestable": 0, "entries": [], "cases": 0}
        if it.get("not_covered"):
            row.update(status="not_covered", why=it["not_covered"])
            out.append(row)
            continue
        pats = it.get("cases") or []
        epats = it.get("entries")
        ents = [r for r in ran if not epats or _match(r["id"], epats)]
        paired = {}
        if it.get("parity"):
            for p in parity_info or []:
                if p.get("status") != "compared" or p.get("twin_kind") != it["parity"]:
                    continue
                rc = (fps.get(p["ref"]) or {}).get("cases", {})
                tc = (fps.get(p["twin"]) or {}).get("cases", {})
                paired[p["twin"]] = {k for k in rc.keys() & tc.keys()
                                     if not rc[k].get("engine_dependent") and not tc[k].get("engine_dependent")}
        keys = set()
        for r in ents:
            fp = fps.get(r["id"]) or {"cases": {}}
            hit = False
            for k, c in fp["cases"].items():
                if _match(k, pats) and (not it.get("parity") or k in paired.get(r["id"], ())):
                    hit = True
                    keys.add(k)
                    if c.get("st") in row:
                        row[c["st"]] += 1
            if hit:
                row["entries"].append(r["id"])
        row["cases"] = len(keys)
        tested = row["pass"] + row["fail"]
        if tested and not it.get("partial"):
            row.update(status="covered", why="")
        elif tested:
            row.update(status="partial", why=it["partial"])
        elif keys:
            row.update(status="partial", why="only skip/untestable results in this run"
                       + (f"; {it['partial']}" if it.get("partial") else ""))
        elif it.get("parity"):
            row.update(status="not_covered", why=f"no matching cases in a completed {it['parity']} parity comparison")
        elif epats and not ents:
            row.update(status="not_covered", why=f"no entry matching {', '.join(epats)} ran"
                       + (f" (needs {it['needs']})" if it.get("needs") else ""))
        else:
            mods = sorted({p.split(".", 1)[0] for p in pats})
            row.update(status="not_covered", why=f"no case matching {', '.join(pats)} reported "
                       f"(module{'s' if len(mods) > 1 else ''} {', '.join(mods)} not run or no such case yet)")
        out.append(row)
    return out


def totals(rows):
    t = {"covered": 0, "partial": 0, "not_covered": 0}
    for r in rows:
        t[r["status"]] = t.get(r["status"], 0) + 1
    return t
