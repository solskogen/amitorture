#!/usr/bin/env python3
"""mkadf.py - build and inspect amitorture ADFs (docs/CONTRACT.md sections 1 and 4).

Build a disk:
    mkadf.py --boot build/bootblock.bin --payload build/payload.exe -o build/amitorture.adf \
             [--build-id STR] [--alloc-flags 0x10000] [--param key=value ...]

Re-bake the run parameters of an existing disk (the runner does this per run):
    mkadf.py --in build/amitorture.adf -o /tmp/run.adf --param mode=quick seed=5 end_action=0

Inspect a disk (checks the bootblock checksum, the blob CRC and the relocs):
    mkadf.py --dump build/amitorture.adf

Parameters (section 4): mode=full|quick|<n> (soak = 2 is reserved, runs like full), seed=<n>, end_action=halt|reboot|return|<n>,
flags=<n>|unsafe,noserial,nombox,net,noscreen (comma or '+' separated), baud=<n>, modules=all|<64-bit mask>.
Numbers accept 0x prefixes. Any error exits with status 1 and a message on stderr.
"""

import argparse
import struct
import sys
import zlib

ADF_SIZE = 901120
SECTOR = 512
HDR_OFF = 1024
BLOB_OFF = 1536
HDR_VERSION = 1
MEMF_ANY_CLEAR = 0x10000
MEMF_CHIP = 0x2
MEMF_FAST = 0x4

HUNK_UNIT, HUNK_NAME, HUNK_CODE, HUNK_DATA, HUNK_BSS = 0x3E7, 0x3E8, 0x3E9, 0x3EA, 0x3EB
HUNK_RELOC32, HUNK_RELOC16, HUNK_RELOC8, HUNK_EXT, HUNK_SYMBOL = 0x3EC, 0x3ED, 0x3EE, 0x3EF, 0x3F0
HUNK_DEBUG, HUNK_END, HUNK_HEADER, HUNK_OVERLAY, HUNK_BREAK = 0x3F1, 0x3F2, 0x3F3, 0x3F5, 0x3F6
HUNK_DREL32, HUNK_DREL16, HUNK_DREL8, HUNK_LIB, HUNK_INDEX = 0x3F7, 0x3F8, 0x3F9, 0x3FA, 0x3FB
HUNK_RELOC32SHORT, HUNK_RELRELOC32, HUNK_ABSRELOC16 = 0x3FC, 0x3FD, 0x3FE
HUNK_NAMES = {v: k for k, v in globals().items() if k.startswith("HUNK_")}

MODES = {"full": 0, "quick": 1, "soak": 2}
END_ACTIONS = {"halt": 0, "reboot": 1, "coldreboot": 1, "return": 2}
FLAG_BITS = {"unsafe": 1, "noserial": 2, "nombox": 4, "net": 8, "noscreen": 16}
ENTRY_SYMBOLS = ("_amt_entry", "amt_entry")


class Fail(Exception):
    pass


def u32(data, off):
    if off + 4 > len(data):
        raise Fail(f"truncated file at offset {off:#x}")
    return struct.unpack_from(">I", data, off)[0]


def align(n, a):
    return (n + a - 1) // a * a


# ---------------------------------------------------------------------------
# hunk executable

class Hunk:
    def __init__(self, index, alloc_bytes, memflags):
        self.index = index
        self.alloc = alloc_bytes      # size from HUNK_HEADER
        self.memflags = memflags      # MEMF_ bits requested by the header
        self.kind = None              # HUNK_CODE / HUNK_DATA / HUNK_BSS
        self.data = b""
        self.relocs = []              # (offset in this hunk, target hunk)
        self.symbols = {}
        self.flat = None


def parse_hunks(blob):
    pos = 0

    def get():
        nonlocal pos
        v = u32(blob, pos)
        pos += 4
        return v

    if get() != HUNK_HEADER:
        raise Fail("not an AmigaOS executable (no HUNK_HEADER)")
    while True:                       # resident library names (must be none)
        n = get()
        if n == 0:
            break
        raise Fail("HUNK_HEADER lists resident libraries; not supported")
    get()                             # table size
    first, last = get(), get()
    if first != 0 or last < first:
        raise Fail(f"unsupported hunk range {first}..{last}")
    hunks = []
    for i in range(last - first + 1):
        raw = get()
        flags = raw >> 30
        size = (raw & 0x3FFFFFFF) * 4
        memflags = 0
        if flags == 1:
            memflags = MEMF_CHIP
        elif flags == 2:
            memflags = MEMF_FAST
        elif flags == 3:
            memflags = get() & ~(1 << 30)
        hunks.append(Hunk(i, size, memflags))

    cur = 0
    h = None
    while pos < len(blob):
        raw = get()
        typ = raw & 0x3FFFFFFF
        if typ in (HUNK_CODE, HUNK_DATA, HUNK_BSS):
            if cur >= len(hunks):
                raise Fail("more hunks than HUNK_HEADER declares")
            h = hunks[cur]
            if h.kind is not None:
                raise Fail(f"hunk {cur} has two content blocks")
            h.kind = typ
            n = get() * 4
            if typ == HUNK_BSS:
                if n > h.alloc:
                    h.alloc = n
            else:
                if pos + n > len(blob):
                    raise Fail(f"hunk {cur} data truncated")
                h.data = blob[pos:pos + n]
                pos += n
                if n > h.alloc:
                    raise Fail(f"hunk {cur}: {n} data bytes but header allocates {h.alloc}")
        elif typ == HUNK_NAME:
            pos += get() * 4
        elif typ == HUNK_RELOC32:
            if h is None:
                raise Fail("HUNK_RELOC32 before any hunk")
            while True:
                n = get()
                if n == 0:
                    break
                target = get()
                for _ in range(n):
                    h.relocs.append((get(), target))
        elif typ == HUNK_RELOC32SHORT:
            if h is None:
                raise Fail("HUNK_RELOC32SHORT before any hunk")
            words = 0
            while True:
                n = struct.unpack_from(">H", blob, pos)[0]
                pos += 2
                words += 1
                if n == 0:
                    break
                target = struct.unpack_from(">H", blob, pos)[0]
                pos += 2
                words += 1
                for _ in range(n):
                    h.relocs.append((struct.unpack_from(">H", blob, pos)[0], target))
                    pos += 2
                    words += 1
            if words & 1:
                pos += 2
        elif typ == HUNK_SYMBOL:
            while True:
                n = get()
                if n == 0:
                    break
                name = blob[pos:pos + (n & 0xFFFFFF) * 4].rstrip(b"\0").decode("latin-1")
                pos += (n & 0xFFFFFF) * 4
                val = get()
                if h is not None:
                    h.symbols[name] = val
        elif typ == HUNK_DEBUG:
            pos += get() * 4
        elif typ == HUNK_END:
            if h is None:
                raise Fail("HUNK_END without a hunk")
            cur += 1
            h = None
        elif typ == HUNK_BREAK:
            break
        else:
            raise Fail(f"cannot represent {HUNK_NAMES.get(typ, hex(typ))} at offset {pos - 4:#x}")
    if cur != len(hunks):
        raise Fail(f"HUNK_HEADER declares {len(hunks)} hunks, file has {cur}")
    return hunks


def flatten(hunks, alloc_flags):
    """Lay the hunks out per section 1. Returns (image, bss_size, relocs, entry_offset)."""
    # trailing BSS: BSS hunks, plus any zero-only tail of the last hunk
    n = len(hunks)
    first_bss = n
    while first_bss > 0 and hunks[first_bss - 1].kind == HUNK_BSS:
        first_bss -= 1
    for h in hunks[:first_bss]:
        if h.kind == HUNK_BSS:
            raise Fail(f"BSS hunk {h.index} is followed by a CODE/DATA hunk; BSS must come last")
    for h in hunks:
        if h.kind is None:
            raise Fail(f"hunk {h.index} has no content")
        if h.memflags & MEMF_CHIP and not alloc_flags & MEMF_CHIP:
            raise Fail(f"hunk {h.index} asks for chip RAM but alloc_flags lacks MEMF_CHIP; "
                       "use AllocMem for chip buffers or pass --alloc-flags")
        if h.memflags & MEMF_FAST and alloc_flags & MEMF_CHIP:
            raise Fail(f"hunk {h.index} asks for fast RAM but alloc_flags has MEMF_CHIP")

    off = 0
    for h in hunks:
        off = align(off, 8)
        h.flat = off
        off += h.alloc
    total = align(off, 4)

    image_end = 0
    for h in hunks[:first_bss]:
        image_end = h.flat + len(h.data)
    # the zero tail of the last stored hunk need not be stored ... unless relocs point into it
    image = bytearray(align(image_end, 4))
    for h in hunks[:first_bss]:
        image[h.flat:h.flat + len(h.data)] = h.data

    relocs = []
    for h in hunks:
        if h.relocs and h.kind == HUNK_BSS:
            raise Fail(f"relocations in BSS hunk {h.index}")
        for off, target in h.relocs:
            if target >= n:
                raise Fail(f"hunk {h.index}: reloc to missing hunk {target}")
            if off + 4 > len(h.data):
                raise Fail(f"hunk {h.index}: reloc at {off:#x} outside the hunk's stored data")
            loc = h.flat + off
            if loc & 1:
                raise Fail(f"hunk {h.index}: reloc at odd address {loc:#x} (68000 cannot do that)")
            val = struct.unpack_from(">I", image, loc)[0] + hunks[target].flat
            if val > 0xFFFFFFFF:
                raise Fail("reloc value overflow")
            struct.pack_into(">I", image, loc, val & 0xFFFFFFFF)
            relocs.append(loc)
    relocs.sort()
    if len(set(relocs)) != len(relocs):
        raise Fail("duplicate reloc locations")

    entry = None
    for name in ENTRY_SYMBOLS:
        for h in hunks:
            if name in h.symbols:
                entry = h.flat + h.symbols[name]
                break
        if entry is not None:
            break
    if entry is None:
        if hunks[0].kind != HUNK_CODE:
            raise Fail("no amt_entry symbol and hunk 0 is not code")
        entry = hunks[0].flat
    if entry & 1:
        raise Fail("odd entry offset")
    bss_size = total - len(image)
    return bytes(image), bss_size, relocs, entry


# ---------------------------------------------------------------------------
# parameters

def parse_num(s):
    try:
        return int(s, 0)
    except ValueError:
        raise Fail(f"bad number {s!r}")


def default_params():
    return {"mode": 0, "seed": 1, "end_action": 0, "flags": 0, "baud": 115200,
            "modules": 0xFFFFFFFFFFFFFFFF}


def apply_param(params, item):
    if "=" not in item:
        raise Fail(f"--param wants key=value, got {item!r}")
    key, val = item.split("=", 1)
    key = key.strip().lower()
    val = val.strip()
    if key == "mode":
        params["mode"] = MODES[val.lower()] if val.lower() in MODES else parse_num(val)
    elif key == "seed":
        params["seed"] = parse_num(val)
    elif key in ("end_action", "end"):
        params["end_action"] = END_ACTIONS[val.lower()] if val.lower() in END_ACTIONS else parse_num(val)
    elif key == "flags":
        if val and all(p in FLAG_BITS for p in val.replace("+", ",").split(",")):
            params["flags"] = 0
            for p in val.replace("+", ",").split(","):
                params["flags"] |= FLAG_BITS[p]
        else:
            params["flags"] = parse_num(val)
    elif key == "baud":
        params["baud"] = parse_num(val)
    elif key in ("modules", "module_mask"):
        params["modules"] = 0xFFFFFFFFFFFFFFFF if val.lower() == "all" else parse_num(val)
    else:
        raise Fail(f"unknown parameter {key!r}")
    for k in ("mode", "seed", "end_action", "flags", "baud"):
        if not 0 <= params[k] <= 0xFFFFFFFF:
            raise Fail(f"{k} out of range")
    if not 0 <= params["modules"] <= 0xFFFFFFFFFFFFFFFF:
        raise Fail("modules out of range")
    if params["mode"] > 2:
        raise Fail("mode must be 0..2")
    if params["end_action"] > 2:
        raise Fail("end_action must be 0..2")
    if params["baud"] == 0:
        raise Fail("baud must not be 0")


def pack_params(p):
    blk = struct.pack(">4sIIIIIII", b"PARM", p["mode"], p["seed"], p["end_action"], p["flags"],
                      p["baud"], p["modules"] >> 32, p["modules"] & 0xFFFFFFFF)
    return blk.ljust(64, b"\0")


def unpack_params(blk):
    if blk[:4] != b"PARM":
        return None
    mode, seed, end, flags, baud, mhi, mlo = struct.unpack_from(">IIIIIII", blk, 4)
    return {"mode": mode, "seed": seed, "end_action": end, "flags": flags, "baud": baud,
            "modules": (mhi << 32) | mlo}


# ---------------------------------------------------------------------------
# ADF

def boot_checksum(block):
    s = 0
    for i in range(0, 1024, 4):
        if i == 4:
            continue
        s += struct.unpack_from(">I", block, i)[0]
        if s > 0xFFFFFFFF:
            s = (s & 0xFFFFFFFF) + 1
    return (~s) & 0xFFFFFFFF


def make_bootblock(code):
    if code[:4] == b"DOS\0":
        block = bytearray(code)
    else:
        block = bytearray(b"DOS\0" + b"\0" * 4 + struct.pack(">I", 880) + code)
    if len(block) > 1024:
        raise Fail(f"bootblock is {len(block)} bytes; max 1024 (1012 bytes of code)")
    block = block.ljust(1024, b"\0")
    if struct.unpack_from(">I", block, 8)[0] != 880:
        raise Fail("bootblock: rootblock field at +8 is not 880")
    struct.pack_into(">I", block, 4, boot_checksum(block))
    return bytes(block)


def build(args):
    with open(args.boot, "rb") as f:
        boot = make_bootblock(f.read())
    with open(args.payload, "rb") as f:
        exe = f.read()
    alloc_flags = parse_num(args.alloc_flags)
    hunks = parse_hunks(exe)
    image, bss, relocs, entry = flatten(hunks, alloc_flags)
    blob = image + b"".join(struct.pack(">I", r) for r in relocs)
    blob_sectors = align(len(blob), SECTOR) // SECTOR
    if BLOB_OFF + blob_sectors * SECTOR > ADF_SIZE:
        raise Fail(f"payload does not fit on the disk: it needs {(BLOB_OFF + blob_sectors * SECTOR) // 1024} KB, "
                   f"an ADF holds {ADF_SIZE // 1024} KB. Build the disk set instead (`make -C disk` or "
                   f"`make -C disk set`), or build fewer modules (`make -C disk MODS=\"rt <modules>\"`).")
    alloc = max(blob_sectors * SECTOR, len(image) + bss)
    params = default_params()
    for item in args.param or []:
        apply_param(params, item)
    bid = (args.build_id or "").encode("ascii")
    if len(bid) > 63:
        raise Fail("build id longer than 63 bytes")
    hdr = bytearray(SECTOR)
    struct.pack_into(">4sIIIIIIIII", hdr, 0, b"AMTP", HDR_VERSION, len(image), bss, len(relocs),
                     entry, blob_sectors, zlib.crc32(blob) & 0xFFFFFFFF, alloc, alloc_flags)
    hdr[64:128] = pack_params(params)
    hdr[128:128 + len(bid)] = bid
    adf = bytearray(ADF_SIZE)
    adf[0:1024] = boot
    adf[HDR_OFF:HDR_OFF + SECTOR] = hdr
    adf[BLOB_OFF:BLOB_OFF + len(blob)] = blob
    write_out(args.output, adf)
    print(f"{args.output}: image {len(image)} bss {bss} relocs {len(relocs)} entry {entry:#x} "
          f"blob {blob_sectors} sectors, alloc {alloc}")


def write_out(path, data):
    if not path:
        raise Fail("no output file (-o)")
    with open(path, "wb") as f:
        f.write(data)


def repatch(args):
    with open(args.input, "rb") as f:
        adf = bytearray(f.read())
    check_adf_basic(adf)
    p = unpack_params(adf[HDR_OFF + 64:HDR_OFF + 128]) or default_params()
    for item in args.param or []:
        apply_param(p, item)
    adf[HDR_OFF + 64:HDR_OFF + 128] = pack_params(p)
    if args.build_id is not None:
        bid = args.build_id.encode("ascii")
        if len(bid) > 63:
            raise Fail("build id longer than 63 bytes")
        adf[HDR_OFF + 128:HDR_OFF + 192] = bid.ljust(64, b"\0")
    write_out(args.output or args.input, adf)


def check_adf_basic(adf):
    if len(adf) != ADF_SIZE:
        raise Fail(f"ADF is {len(adf)} bytes, expected {ADF_SIZE}")
    if adf[HDR_OFF:HDR_OFF + 4] != b"AMTP":
        raise Fail("no AMTP payload header in sector 2")


def dump(path):
    with open(path, "rb") as f:
        adf = f.read()
    check_adf_basic(adf)
    errors = []
    boot = adf[:1024]
    if boot[:4] != b"DOS\0":
        errors.append("bootblock does not start with DOS\\0")
    cs = struct.unpack_from(">I", boot, 4)[0]
    if cs != boot_checksum(boot):
        errors.append(f"bootblock checksum {cs:08x} != {boot_checksum(boot):08x}")
    (magic, ver, image, bss, nrel, entry, sectors, crc, alloc, aflags) = \
        struct.unpack_from(">4sIIIIIIIII", adf, HDR_OFF)
    bid = adf[HDR_OFF + 128:HDR_OFF + 192].split(b"\0")[0].decode("ascii", "replace")
    print(f"bootblock     checksum {cs:08x} ({'ok' if cs == boot_checksum(boot) else 'BAD'})")
    print(f"magic         {magic.decode('latin-1')}  version {ver}")
    print(f"image_size    {image}")
    print(f"bss_size      {bss}")
    print(f"reloc_count   {nrel}")
    print(f"entry_offset  {entry:#x}")
    print(f"blob_sectors  {sectors}")
    print(f"blob_crc32    {crc:08x}")
    print(f"alloc_size    {alloc}")
    print(f"alloc_flags   {aflags:#x}")
    print(f"build_id      {bid!r}")
    p = unpack_params(adf[HDR_OFF + 64:HDR_OFF + 128])
    if p is None:
        print("params        (absent: defaults)")
    else:
        mode = {v: k for k, v in MODES.items()}.get(p["mode"], "?")
        print(f"params        mode={p['mode']}({mode}) seed={p['seed']} end_action={p['end_action']} "
              f"flags={p['flags']:#x} baud={p['baud']} modules={p['modules']:#018x}")
    if ver != HDR_VERSION:
        errors.append(f"header version {ver}")
    if image % 4:
        errors.append("image_size not a multiple of 4")
    nbytes = image + nrel * 4
    if align(nbytes, SECTOR) // SECTOR != sectors:
        errors.append(f"blob_sectors {sectors} does not match {nbytes} bytes")
    if alloc != max(sectors * SECTOR, image + bss):
        errors.append("alloc_size mismatch")
    if max(nbytes, sectors * SECTOR) > len(adf) - BLOB_OFF:
        errors.append("blob extends beyond the ADF")
        for e in errors:
            print(f"ERROR: {e}", file=sys.stderr)
        return 1
    blob = adf[BLOB_OFF:BLOB_OFF + nbytes]
    real = zlib.crc32(blob) & 0xFFFFFFFF
    print(f"blob crc      {real:08x} ({'ok' if real == crc else 'BAD'})")
    if real != crc:
        errors.append("blob CRC mismatch")
    if entry >= image or entry & 1:
        errors.append("entry_offset outside the image or odd")
    bad = 0
    for i in range(nrel):
        r = struct.unpack_from(">I", blob, image + 4 * i)[0]
        if r & 1 or r + 4 > image:
            bad += 1
            continue
        if struct.unpack_from(">I", blob, r)[0] > image + bss:
            bad += 1
    print(f"relocs        {nrel} ({'ok' if bad == 0 else f'{bad} BAD'})")
    if bad:
        errors.append(f"{bad} relocs out of range")
    if any(adf[BLOB_OFF + nbytes:BLOB_OFF + sectors * SECTOR]):
        errors.append("blob padding not zero")
    for e in errors:
        print(f"ERROR: {e}", file=sys.stderr)
    return 1 if errors else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--boot", help="bootblock binary (vasm -Fbin output)")
    ap.add_argument("--payload", help="linked AmigaOS hunk executable")
    ap.add_argument("--in", dest="input", help="existing ADF whose parameters to re-bake")
    ap.add_argument("-o", "--output", help="output ADF")
    ap.add_argument("--param", nargs="+", action="extend", metavar="KEY=VALUE")
    ap.add_argument("--build-id")
    ap.add_argument("--alloc-flags", default=hex(MEMF_ANY_CLEAR))
    ap.add_argument("--dump", metavar="ADF")
    args = ap.parse_args()
    try:
        if args.dump:
            return dump(args.dump)
        if args.input:
            repatch(args)
            return 0
        if not args.boot or not args.payload:
            ap.error("need --boot and --payload (or --in, or --dump)")
        build(args)
        return 0
    except (Fail, KeyError, OSError) as e:
        print(f"mkadf: error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
