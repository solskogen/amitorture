#!/usr/bin/env python3
"""Generate runner/amitorture/romdb.json from Amiberry's ROM table.

Usage: gen_romdb.py [--amiberry-src DIR] [--out FILE]

Reads the `roms[]` table in <src>/src/rommgr.cpp and the ROMTYPE_* defines in
<src>/src/include/rommgr.h, and keeps the entries that matter to amitorture:
Kickstarts (incl. AROS built-in, A1000 bootstrap, special kicks), extended
ROMs (CD32/CDTV), the Cloanto rom.key files, CPU board ROMs and expansion
board ROMs (SCSI/IDE/net/...). Split chip dumps (ALTROM/ALTROMPN: even/odd
halves) are kept too, as `part` entries pointing at their parent id, so a
half dump can still be named. Freezers, Arcadia, ALG and CD32 carts are left
out. Identification is by CRC32 / SHA1; names in the table are only labels.
"""
import argparse
import json
import os
import re
import sys

KEEP_GROUPS = {
    0x00000100: "kick",
    0x00000200: "kick_cd32",
    0x00000400: "ext_cd32",
    0x00000800: "ext_cdtv",
    0x00001000: "key",
    0x00010000: "special_kick",
    0x00040000: "cpuboard",
    0x00100000: "board",
}
FLAG_NAMES = {
    0x00800000: "not", 0x01000000: "quad", 0x02000000: "even", 0x04000000: "odd",
    0x08000000: "8bit", 0x10000000: "byteswap", 0x20000000: "cd32", 0x40000000: "scrambled",
    0x80000000: "none",
}


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def split_top(s):
    """Splits s at top-level commas (not inside (), {} or string literals)."""
    out, depth, cur, i, instr = [], 0, [], 0, False
    while i < len(s):
        c = s[i]
        if instr:
            cur.append(c)
            if c == "\\":
                cur.append(s[i + 1])
                i += 1
            elif c == '"':
                instr = False
        elif c == '"':
            instr = True
            cur.append(c)
        elif c in "({":
            depth += 1
            cur.append(c)
        elif c in ")}":
            depth -= 1
            cur.append(c)
        elif c == "," and depth == 0:
            out.append("".join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    if "".join(cur).strip():
        out.append("".join(cur).strip())
    return out


def parse_defines(header):
    defs = {}
    for m in re.finditer(r"#define\s+(ROMTYPE_\w+)\s+(0x[0-9a-fA-F]+|\d+)\b", header):
        defs[m.group(1)] = int(m.group(2), 0)
    return defs


def value(tok, defs):
    tok = tok.strip()
    if tok in ("NULL", "nullptr"):
        return None
    m = re.fullmatch(r'_T\((".*")\)', tok, flags=re.S)
    if m:
        raw = json.loads(m.group(1).replace("\\0", "\\u0000"))
        parts = [p for p in raw.split("\0") if p]
        return parts if "\0" in raw else raw
    expr = re.sub(r"\bROMTYPE_\w+\b", lambda mm: str(defs[mm.group(0)]), tok)
    if not re.fullmatch(r"[0-9a-fA-FxX\s|<>()*+&-]+", expr):
        raise ValueError(f"unparsable token {tok!r}")
    return int(eval(expr, {"__builtins__": {}}))  # noqa: S307 - digits and operators only


def entries(src, defs):
    start = src.index("static struct romdata roms[] = {")
    body = src[start + len("static struct romdata roms[] = {"):]
    body = body[:body.index("{ NULL }")]
    i = 0
    while i < len(body):
        c = body[i]
        if c == "{":
            depth, j = 0, i
            while True:
                if body[j] == "{":
                    depth += 1
                elif body[j] == "}":
                    depth -= 1
                    if depth == 0:
                        break
                elif body[j] == '"':
                    j += 1
                    while body[j] != '"':
                        j += 2 if body[j] == "\\" else 1
                j += 1
            yield "entry", split_top(body[i + 1:j])
            i = j + 1
        elif body.startswith("ALTROMPN(", i) or body.startswith("ALTROM(", i):
            pn = body.startswith("ALTROMPN(", i)
            j = body.index("(", i)
            depth, k = 0, j
            while True:
                if body[k] == "(":
                    depth += 1
                elif body[k] == ")":
                    depth -= 1
                    if depth == 0:
                        break
                k += 1
            yield ("altpn" if pn else "alt"), split_top(body[j + 1:k])
            i = k + 1
        else:
            i += 1


def classify(rtype):
    group = rtype & 0x003fff00
    sub = rtype & 0xff
    cat = None
    for bit, name in KEEP_GROUPS.items():
        if group & bit:
            cat = name
            break
    flags = [n for b, n in FLAG_NAMES.items() if rtype & b]
    return cat, sub, flags


def build(src_dir):
    src = strip_comments(open(os.path.join(src_dir, "src", "rommgr.cpp"), encoding="utf-8", errors="replace").read())
    header = open(os.path.join(src_dir, "src", "include", "rommgr.h"), encoding="utf-8", errors="replace").read()
    defs = parse_defines(header)
    names_by_value = {}
    for k, v in defs.items():
        names_by_value.setdefault(v, k)
    out, parents = [], {}
    for kind, f in entries(src, defs):
        if kind == "entry":
            v = [value(t, defs) for t in f]
            v += [None] * (23 - len(v))
            name, ver, rev, subver, subrev, model, size, rid, cpu, cloanto, rtype = v[:11]
            crc, sha = v[14], v[15:20]
            configname, defaultfilename = v[20], v[21]
            if rtype is None:
                continue
            cat, sub, flags = classify(rtype)
            if cat is None:
                continue
            e = {
                "id": rid, "name": name, "category": cat,
                "type": names_by_value.get(rtype & 0x003fffff, hex(rtype & 0x003fffff)),
                "flags": flags, "version": f"{ver}.{rev}", "exec": f"{subver}.{subrev}",
                "models": model if isinstance(model, list) else ([model] if model else []),
                "size": size, "cpu_flags": cpu, "cloanto": bool(cloanto),
                "crc32": None if crc in (None, 0xffffffff) else f"{crc:08x}",
                "sha1": None if not any(sha) else "".join(f"{x:08x}" for x in sha),
                "configname": configname, "default_filename": defaultfilename,
            }
            if rid == 66 or configname == "AROS":
                e["category"] = "kick"
                e["builtin"] = ":AROS"
            parents[rid] = e
            out.append(e)
        else:
            v = [value(t, defs) for t in f]
            if kind == "altpn":
                rid, grp, num, size, flags_v, pn, crc = v[:7]
                sha = v[7:12]
            else:
                rid, grp, num, size, flags_v, crc = v[:6]
                sha, pn = v[6:11], None
            parent = parents.get(rid)
            if parent is None:
                continue
            out.append({
                "id": rid, "name": f"{parent['name']} (part {grp}.{num}{', ' + pn if pn else ''})",
                "category": "part", "parent_category": parent["category"], "type": parent["type"],
                "flags": [n for b, n in FLAG_NAMES.items() if (flags_v or 0) & b],
                "version": parent["version"], "exec": parent["exec"], "models": parent["models"],
                "size": size, "cpu_flags": parent["cpu_flags"], "cloanto": False, "partnumber": pn,
                "crc32": f"{crc:08x}", "sha1": "".join(f"{x:08x}" for x in sha),
            })
    return out


def _amiberry_rev(src):
    """" at <git rev>" of the Amiberry tree, or "" (no local paths in the output)."""
    import subprocess
    try:
        r = subprocess.run(["git", "-C", src, "rev-parse", "--short", "HEAD"], capture_output=True, text=True,
                           timeout=10)
    except OSError:
        return ""
    return f" at {r.stdout.strip()}" if r.returncode == 0 and r.stdout.strip() else ""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--amiberry-src", default=os.environ.get("AMIBERRY_SRC", os.path.expanduser("~/amiberry")))
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "amitorture", "romdb.json"))
    a = ap.parse_args()
    roms = build(a.amiberry_src)
    doc = {
        "about": "Generated by runner/tools/gen_romdb.py from Amiberry src/rommgr.cpp. Do not edit by hand.",
        "source": "Amiberry src/rommgr.cpp" + _amiberry_rev(a.amiberry_src),
        "count": len(roms),
        "roms": roms,
    }
    with open(a.out, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, indent=1, sort_keys=False)
        fh.write("\n")
    cats = {}
    for r in roms:
        cats[r["category"]] = cats.get(r["category"], 0) + 1
    print(f"wrote {len(roms)} ROMs to {os.path.normpath(a.out)}: {cats}", file=sys.stderr)


if __name__ == "__main__":
    main()
