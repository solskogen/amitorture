"""ROM identification by content (CRC32 / SHA1), never by file name.

Handles: plain dumps, 256 KB ROMs doubled to 512 KB, byte-swapped dumps,
Cloanto-encrypted ``AMIROMTYPE1`` files (decrypted with ``rom.key`` when one
is next to them, otherwise reported as ``needs rom.key``), and unknown
Kickstart-like images (named from their header, e.g. 47.115).
"""
from __future__ import annotations

import hashlib
import json
import os
import struct
import zlib
from functools import lru_cache

ROMDB_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "romdb.json")
CLOANTO_MAGIC = b"AMIROMTYPE1"
MAX_ROM_SIZE = 8 * 1024 * 1024


@lru_cache(maxsize=1)
def romdb():
    with open(ROMDB_PATH, encoding="utf-8") as fh:
        doc = json.load(fh)
    by_crc, by_sha = {}, {}
    for r in doc["roms"]:
        if r.get("crc32"):
            by_crc.setdefault(r["crc32"], []).append(r)
        if r.get("sha1"):
            by_sha[r["sha1"]] = r
    return doc, by_crc, by_sha


def by_id(rom_id):
    doc = romdb()[0]
    for r in doc["roms"]:
        if r["id"] == rom_id and r["category"] != "part":
            return r
    return None


def _lookup(data):
    _, by_crc, by_sha = romdb()
    crc = f"{zlib.crc32(data) & 0xffffffff:08x}"
    sha = hashlib.sha1(data).hexdigest()
    if sha in by_sha:
        return by_sha[sha], crc, sha, "sha1"
    for r in by_crc.get(crc, []):
        if r["size"] == len(data):
            return r, crc, sha, "crc32"
    # BlizzardPPC/CyberStorm MK3/PPC flash images carry per-board config
    # data; Amiberry (rommgr.cpp, "have config data") blanks 0x10-0x2f and
    # hashes only the first 0x1c000 (or 0x50000 for 512K) bytes.
    if len(data) in (131072, 524288):
        rt = bytearray(data)
        rt[0x10:0x30] = bytes(0x20)
        for n in ((0x1c000, 0x50000) if len(data) == 524288 else (0x1c000,)):
            s2 = hashlib.sha1(rt[:n]).hexdigest()
            r = by_sha.get(s2)
            if r and r["size"] == len(data):
                return r, crc, sha, "sha1-flash"
    return None, crc, sha, None


def _byteswap(data):
    n = len(data) & ~1
    out = bytearray(n)
    out[0::2] = data[1:n:2]
    out[1::2] = data[0:n:2]
    return bytes(out)


def _kick_header(data):
    """For unknown images that look like a Kickstart: version/revision and the
    exec ident string, from the ROM header."""
    if len(data) < 0x40 or data[0:2] not in (b"\x11\x11", b"\x11\x14", b"\x11\x16") or data[2:4] != b"\x4e\xf9":
        return None
    ver, rev = struct.unpack(">HH", data[12:16])
    info = {"version": f"{ver}.{rev}"}
    i = data.find(b"exec ")
    if i >= 0:
        info["exec_ident"] = data[i:data.find(b"\r", i, i + 80) if data.find(b"\r", i, i + 80) > 0 else i + 40].decode("latin-1", "replace").strip()
    if b"AROS" in data[:0x100] or b"AROS Research" in data or b"aros" in data[:0x2000].lower():
        info["aros"] = True
    return info


def load_key(dirpath):
    for name in ("rom.key", "ROM.KEY", "Rom.key"):
        p = os.path.join(dirpath, name)
        if os.path.isfile(p):
            with open(p, "rb") as fh:
                return p, fh.read()
    return None, None


def identify_bytes(data, keydata=None):
    """Returns a dict describing the ROM image; never raises on bad input."""
    res = {"size": len(data)}
    raw = data
    if data.startswith(CLOANTO_MAGIC):
        res["encrypted"] = "cloanto"
        if not keydata:
            res["status"] = "needs rom.key"
            res["crc32"] = f"{zlib.crc32(raw) & 0xffffffff:08x}"
            return res
        body = data[len(CLOANTO_MAGIC):]
        data = bytes(b ^ keydata[i % len(keydata)] for i, b in enumerate(body))
        res["size"] = len(data)
    rom, crc, sha, how = _lookup(data)
    res.update({"crc32": crc, "sha1": sha})
    if rom is None and len(data) == 524288 and data[:262144] == data[262144:]:
        rom, crc2, sha2, how = _lookup(data[:262144])
        if rom:
            res["doubled"] = True
            res["crc32_256k"] = crc2
    if rom is None:
        sw = _byteswap(data)
        rom2, crc2, sha2, how2 = _lookup(sw)
        if rom2:
            rom, how = rom2, how2
            res["byteswapped"] = True
            res["status"] = "byteswapped"
            res["crc32_unswapped"] = crc2
    if rom is not None:
        res.setdefault("status", "ok")
        res["match"] = how
        res["rom"] = {k: rom.get(k) for k in ("id", "name", "category", "type", "version", "exec", "models",
                                              "cpu_flags", "cloanto", "parent_category", "partnumber") if k in rom}
        return res
    kh = _kick_header(data) or _kick_header(_byteswap(data))
    if kh:
        res["status"] = "unknown kickstart"
        res["header"] = kh
        if _kick_header(data) is None:
            res["byteswapped"] = True
    else:
        res["status"] = "unknown"
    return res


def identify_file(path):
    res = {"path": os.path.abspath(path), "file": os.path.basename(path)}
    try:
        st = os.stat(path)
        if st.st_size > MAX_ROM_SIZE or st.st_size == 0:
            res["status"] = "not a rom (size)"
            res["size"] = st.st_size
            return res
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError as e:
        res["status"] = f"unreadable: {e}"
        return res
    keypath, key = (None, None)
    if data.startswith(CLOANTO_MAGIC):
        keypath, key = load_key(os.path.dirname(os.path.abspath(path)))
    res.update(identify_bytes(data, key))
    if keypath and res.get("encrypted"):
        res["key"] = keypath
    res["sha256_file"] = hashlib.sha256(data).hexdigest()
    return res


def scan_dir(dirpath, recursive=False):
    """Identifies every file in dirpath (rom.key itself is reported as the key)."""
    out = []
    if not os.path.isdir(dirpath):
        return out
    walker = os.walk(dirpath) if recursive else [(dirpath, [], sorted(os.listdir(dirpath)))]
    for root, _dirs, files in walker:
        for f in sorted(files):
            p = os.path.join(root, f)
            if not os.path.isfile(p):
                continue
            r = identify_file(p)
            out.append(r)
    return out


def usable(r):
    """A ROM the emulator can load as-is (encrypted with key or plain; not byteswapped/unknown)."""
    return r.get("status") == "ok" and not r.get("byteswapped")


class RomSet:
    """The ROMs available for a run, indexed by romdb id."""

    def __init__(self, scanned):
        self.scanned = scanned
        self.by_id = {}
        for r in scanned:
            if usable(r) and r["rom"]["category"] != "part":
                self.by_id.setdefault(r["rom"]["id"], r)

    def find(self, ids):
        for i in ids:
            if i in self.by_id:
                return self.by_id[i]
        return None
