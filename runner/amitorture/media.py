"""Test-owned media for the devio module (CONTRACT.md section 11).

The devio module (disk/modules/devio.c) writes ONLY to units whose first and
last block both carry the amitorture ownership signature. This file creates
such media in a directory (the runner's work/media), checks and describes
them, and gives the Amiberry configuration lines that attach them.

    create_adf(path)                    880 KB DD ADF, signed (for DF1)
    create_hdf(path, size_mb)           plain hardfile, no RDB, signed (use it
                                        with the "uae" controller only: on an
                                        IDE/SCSI controller Amiberry puts a
                                        virtual RDB in front of a partition
                                        hardfile, hardfile.cpp
                                        create_virtual_rdb, so the signature
                                        is no longer at LBA 0)
    create_rdb_hdf(path, size_mb)       hardfile with a valid RDSK block and no
                                        partitions (or one non-bootable,
                                        unmountable partition), signed
    create_pcmcia_sram(path)            4 MB PCMCIA SRAM card image + 4 KB attribute
                                        memory with a CIS (CISTPL_FORMAT disk), signed
    verify(path) / describe(path)       check the signature (and RDB)
    config_lines(controller, ...)       Amiberry option lines for a controller
    prepare(entry, options, media_dir, seed)
                                        runner hook: makes the media for a
                                        matrix entry, returns {"options": ...}

Signature block layout (512 bytes, big-endian; CONTRACT.md section 11):
    0   char[8]  "AMTOWN01"
    8   u8[16]   run UUID (same in both signed blocks)
    24  u32      block number (LBA of this block)
    28  u32      total blocks of the medium (last signed block = total-1)
    32  u32      block size (512)
    36  u32      first block the test may write (the region is
                 [first_free, total-1))
    40  u32      kind: 1 ADF, 2 plain HDF, 3 RDB HDF, 4 PCMCIA SRAM
    44  char[64] ASCII note, NUL padded
    108 ...      zero
    508 u32      CRC-32 (zlib/IEEE) of bytes 0..507

Command line (from runner/):  python3 -m amitorture.media create DIR [--size-mb N]
                               python3 -m amitorture.media describe FILE...
"""
from __future__ import annotations

import os
import shutil
import struct
import sys
import uuid as _uuid
import zlib

BLOCK = 512
MAGIC = b"AMTOWN01"
KIND_ADF, KIND_HDF, KIND_RDB, KIND_SRAM = 1, 2, 3, 4
KIND_NAMES = {KIND_ADF: "adf", KIND_HDF: "hdf", KIND_RDB: "rdb", KIND_SRAM: "pcmcia_sram"}
# PCMCIA SRAM card (Gayle, A600/A1200): Amiberry maps the first 4 MB of the
# image as the card's common memory and the bytes after that as its
# attribute memory (gayle.cpp). Without a CISTPL_FORMAT tuple Kickstart 3.1
# claims a card as system RAM, so the image carries a CIS saying "disk".
SRAM_BYTES = 4 << 20
SRAM_ATTR = 4096
NOTE = b"amitorture test-owned medium: contents may be overwritten"
ADF_BLOCKS = 1760
# hardfiles are sized in whole 16-head x 63-sector cylinders, so a CHS
# translation (IDE identify, RDB) gives back the exact capacity
HDF_HEADS, HDF_SECS = 16, 63
HDF_CYL = HDF_HEADS * HDF_SECS
RDB_CYLS = 2              # cylinders reserved for the RDB area
ROM_DIRS = [os.path.expanduser("~/Amiberry/ROMs")]
BOARD_ROMS = {"a2091": ["amiga-boot-a590.rom", "a590.rom", "a2091.rom"],
              "a590": ["amiga-boot-a590.rom", "a590.rom"],
              "a4091": ["amiga-boot-a4091.rom", "a4091.rom"]}


def crc32(b):
    return zlib.crc32(b) & 0xffffffff


# ---------------------------------------------------------------- signature
def signature_block(run_uuid: bytes, lba: int, total: int, first_free: int, kind: int) -> bytes:
    if len(run_uuid) != 16:
        raise ValueError("run uuid must be 16 bytes")
    b = bytearray(BLOCK)
    b[0:8] = MAGIC
    b[8:24] = run_uuid
    struct.pack_into(">IIIII", b, 24, lba, total, BLOCK, first_free, kind)
    b[44:44 + len(NOTE)] = NOTE
    struct.pack_into(">I", b, 508, crc32(bytes(b[:508])))
    return bytes(b)


def parse_signature(b: bytes):
    """dict for a valid signature block, else None."""
    if len(b) < BLOCK or b[:8] != MAGIC:
        return None
    if struct.unpack_from(">I", b, 508)[0] != crc32(b[:508]):
        return None
    lba, total, bs, first, kind = struct.unpack_from(">IIIII", b, 24)
    return {"uuid": b[8:24].hex(), "lba": lba, "total": total, "block_size": bs, "first_free": first,
            "kind": KIND_NAMES.get(kind, kind)}


def _new_uuid(run_uuid):
    if run_uuid is None:
        return _uuid.uuid4().bytes
    if isinstance(run_uuid, str):
        return _uuid.UUID(run_uuid).bytes
    return bytes(run_uuid)


def _write_image(path, total, first_free, kind, run_uuid, extra_blocks=()):
    u = _new_uuid(run_uuid)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.truncate(total * BLOCK)
        f.seek(0)
        f.write(signature_block(u, 0, total, first_free, kind))
        for lba, data in extra_blocks:
            f.seek(lba * BLOCK)
            f.write(data)
        f.seek((total - 1) * BLOCK)
        f.write(signature_block(u, total - 1, total, first_free, kind))
    os.replace(tmp, path)
    return {"path": os.path.abspath(path), "uuid": u.hex(), "blocks": total, "first_free": first_free,
            "kind": KIND_NAMES[kind], "size": total * BLOCK}


# ---------------------------------------------------------------- media
def pcmcia_cis(size=SRAM_BYTES, fmt="disk"):
    """Attribute memory (SRAM_ATTR bytes) with a CIS on the even addresses
    (PC Card standard, metaformat): CISTPL_DEVICE (SRAM, 4 MB units),
    CISTPL_FORMAT (disk, 512-byte blocks, the whole card) unless fmt is
    None, CISTPL_FUNCID (memory), CISTPL_END."""
    total = size // BLOCK
    cis = [0x01, 3, 0x64, (7 << 3) | 5, 0xff]
    if fmt == "disk":
        body = [0, 0] + list(struct.pack("<II", 0, size)) + list(struct.pack("<HII", BLOCK, total, 0))
        cis += [0x41, len(body)] + body
    cis += [0x21, 2, 1, 0, 0xff]
    attr = bytearray(SRAM_ATTR)
    for i, c in enumerate(cis):
        attr[2 * i] = c
    return bytes(attr)


def create_pcmcia_sram(path, run_uuid=None, fmt="disk"):
    """4 MB SRAM card image, signed in its first and last common-memory
    block, followed by the attribute memory with the CIS."""
    u = _new_uuid(run_uuid)
    total = SRAM_BYTES // BLOCK
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.truncate(SRAM_BYTES)
        f.write(signature_block(u, 0, total, 2, KIND_SRAM))
        f.seek((total - 1) * BLOCK)
        f.write(signature_block(u, total - 1, total, 2, KIND_SRAM))
        f.seek(SRAM_BYTES)
        f.write(pcmcia_cis(SRAM_BYTES, fmt))
    os.replace(tmp, path)
    return {"path": os.path.abspath(path), "uuid": u.hex(), "blocks": total, "first_free": 2,
            "kind": KIND_NAMES[KIND_SRAM], "size": SRAM_BYTES, "attribute_bytes": SRAM_ATTR, "cis_format": fmt}


def pcmcia_sram_options(path):
    """Amiberry options inserting the card (gayle.cpp: the board enabled,
    "inserted", and a hardfile on controller custom0_pcmciasram). The
    hardfile key has a "#pcmcia" tag so it can sit next to an IDE hardfile2
    (amiberry_cfg writes it as hardfile2=)."""
    return {"pcmciasram_rom_file": ":ENABLED", "pcmciasram_rom_options": "inserted=true",
            "hardfile2#pcmcia": f"rw,CC0:{path},0,0,0,512,0,,custom0_pcmciasram"}


def create_adf(path, run_uuid=None):
    """880 KB DD ADF: signature in blocks 0 and 1759, the rest zero. It has
    no DOS bootblock, so it must not go in DF0 (devio expects it in DF1)."""
    return _write_image(path, ADF_BLOCKS, 2, KIND_ADF, run_uuid)


def hdf_blocks(size_mb):
    """Blocks of an N MB hardfile, rounded down to whole 16x63 cylinders
    (at least 4 cylinders)."""
    if size_mb >= 4096:
        # devio addresses blocks with the 32-bit io_Offset (no TD64/NSD) and
        # refuses a signed medium larger than 4 GB (review M3)
        raise ValueError(f"test hardfile of {size_mb} MB: must be below 4096 MB")
    n = int(size_mb * 1024 * 1024) // BLOCK
    return max(4, n // HDF_CYL) * HDF_CYL


def create_hdf(path, size_mb=8, run_uuid=None):
    """Plain hardfile (no RDB): signature in block 0 and the last block."""
    total = hdf_blocks(size_mb)
    info = _write_image(path, total, 2, KIND_HDF, run_uuid)
    info.update({"heads": HDF_HEADS, "sectors": HDF_SECS, "cylinders": total // HDF_CYL})
    return info


def _rdb_checksum(b, n_longs):
    s = sum(struct.unpack_from(f">{n_longs}I", b, 0)) & 0xffffffff
    return (-s) & 0xffffffff


def rdsk_block(cyls, heads=HDF_HEADS, secs=HDF_SECS, part_lba=None):
    """A valid RigidDiskBlock (devices/hardblocks.h) at block 1, with no
    partitions (or one PART block at part_lba), no filesystem headers."""
    b = bytearray(BLOCK)
    NONE = 0xffffffff
    cylblocks = heads * secs
    struct.pack_into(">4sIiI", b, 0, b"RDSK", 64, 0, 7)          # ID, SummedLongs, ChkSum, HostID
    struct.pack_into(">II", b, 16, BLOCK, 0x7)                   # BlockBytes, Flags (LAST|LASTLUN|LASTTID)
    struct.pack_into(">IIIII", b, 24, NONE, NONE if part_lba is None else part_lba, NONE, NONE, NONE)
    for i in range(5):                                           # rdb_Reserved1[5]
        struct.pack_into(">I", b, 44 + 4 * i, NONE)
    struct.pack_into(">IIIII", b, 64, cyls, secs, heads, 1, cyls)   # Cylinders..Park
    struct.pack_into(">III", b, 96, cyls, cyls, 3)                # WritePreComp, ReducedWrite, StepRate
    rdb_hi = RDB_CYLS * cylblocks - 1
    struct.pack_into(">IIIIII", b, 128, 0, rdb_hi, RDB_CYLS, cyls - 1, cylblocks, 0)
    struct.pack_into(">I", b, 152, 1 if part_lba is None else part_lba)   # HighRDSKBlock
    b[160:168] = b"AMITORTU"
    b[168:184] = b"TEST-OWNED DISK "
    b[184:188] = b"0001"
    struct.pack_into(">I", b, 8, _rdb_checksum(b, 64))
    return bytes(b)


def part_block(cyls, heads=HDF_HEADS, secs=HDF_SECS):
    """One PART block, non-bootable and with automount off (PBFF_NOMOUNT),
    DosType 0 (no filesystem), covering the cylinders after the RDB area."""
    b = bytearray(BLOCK)
    NONE = 0xffffffff
    struct.pack_into(">4sIiI", b, 0, b"PART", 64, 0, 7)
    struct.pack_into(">IIIII", b, 16, NONE, 0x2, 0, 0, 0)     # Next, Flags=PBFF_NOMOUNT, Reserved1[2], DevFlags
    name = b"AMTTEST"
    b[36] = len(name)
    b[37:37 + len(name)] = name
    env = [16, BLOCK // 4, 0, heads, 1, secs, 2, 0, 0, RDB_CYLS, cyls - 1, 30, 0, 0x7fffffff, 0xfffffffe,
           (-128) & 0xffffffff, 0]                              # DosEnvec ... BootPri, DosType 0
    struct.pack_into(f">{len(env)}I", b, 128, *env)
    struct.pack_into(">I", b, 8, _rdb_checksum(b, 64))
    return bytes(b)


def create_rdb_hdf(path, size_mb=8, run_uuid=None, partition=False):
    """Hardfile with an RDB: block 0 = signature, block 1 = RDSK (valid
    checksum, no partitions; with partition=True one PART block at block 2,
    non-bootable, PBFF_NOMOUNT, DosType 0), last block = signature. The test
    region starts after the reserved RDB cylinders."""
    total = hdf_blocks(size_mb)
    cyls = total // HDF_CYL
    extra = [(1, rdsk_block(cyls, part_lba=2 if partition else None))]
    if partition:
        extra.append((2, part_block(cyls)))
    info = _write_image(path, total, RDB_CYLS * HDF_CYL, KIND_RDB, run_uuid, extra)
    info.update({"heads": HDF_HEADS, "sectors": HDF_SECS, "cylinders": cyls, "rdb_block": 1,
                 "partition": bool(partition)})
    return info


# ---------------------------------------------------------------- checks
def verify(path):
    """{'ok': bool, 'problems': [...], 'first': sig, 'last': sig, 'rdb': {...}}"""
    out = {"path": os.path.abspath(path), "ok": False, "problems": []}
    size = os.path.getsize(path)
    out["size"] = size
    if size % BLOCK or size < 4 * BLOCK:
        out["problems"].append(f"size {size} is not a multiple of {BLOCK}")
        return out
    total = size // BLOCK
    if size == SRAM_BYTES + SRAM_ATTR:
        total = SRAM_BYTES // BLOCK          # PCMCIA SRAM: attribute memory after the card
    with open(path, "rb") as f:
        b0 = f.read(BLOCK)
        rdb = None
        for lba in range(1, 16):
            f.seek(lba * BLOCK)
            blk = f.read(BLOCK)
            if blk[:4] == b"RDSK":
                rdb = (lba, blk)
                break
        f.seek((total - 1) * BLOCK)
        bl = f.read(BLOCK)
    s0, sl = parse_signature(b0), parse_signature(bl)
    out["first"], out["last"] = s0, sl
    if not s0:
        out["problems"].append("block 0: no valid AMTOWN01 signature")
    if not sl:
        out["problems"].append(f"block {total - 1}: no valid AMTOWN01 signature")
    if s0 and sl:
        if s0["uuid"] != sl["uuid"]:
            out["problems"].append("signature UUIDs differ")
        if s0["lba"] != 0 or sl["lba"] != total - 1:
            out["problems"].append("signature block numbers are wrong")
        if s0["total"] != total or sl["total"] != total:
            out["problems"].append(f"signature says {s0['total']} blocks, file has {total}")
    if rdb:
        lba, blk = rdb
        n = struct.unpack_from(">I", blk, 4)[0]
        ok = 0 < n <= 128 and sum(struct.unpack_from(f">{n}I", blk, 0)) & 0xffffffff == 0
        out["rdb"] = {"block": lba, "checksum_ok": ok,
                      "cylinders": struct.unpack_from(">I", blk, 64)[0],
                      "partition_list": f"{struct.unpack_from('>I', blk, 28)[0]:08x}"}
        if not ok:
            out["problems"].append("RDSK checksum bad")
    out["ok"] = not out["problems"]
    return out


def describe(path):
    v = verify(path)
    if not v["ok"]:
        return f"{path}: NOT test-owned ({'; '.join(v['problems'])})"
    s = v["first"]
    r = f", RDB at block {v['rdb']['block']}" if v.get("rdb") else ""
    return (f"{path}: test-owned {s['kind']}, {s['total']} blocks of {s['block_size']}, uuid {s['uuid']}, "
            f"writable region [{s['first_free']}, {s['total'] - 1}){r}")


# ---------------------------------------------------------------- Amiberry
CONTROLLERS = {
    # name: (description, controller field of hardfile2, extra options)
    "gayle": ("A600/A1200 Gayle IDE (KS scsi.device)", "ide{u}", ["ide=a600/a1200"]),
    "a4000ide": ("A4000 motherboard IDE (KS scsi.device)", "ide{u}", ["ide=a4000"]),
    "a3000": ("A3000 WD33C93 SCSI (KS scsi.device)", "scsi{u}_a3000", ["scsi_a3000=true"]),
    "a2091": ("A590/A2091 SCSI with its boot ROM (scsi.device)", "scsi{u}_a2091", []),
    "a4091": ("A4091 NCR53C710 SCSI with its boot ROM (scsi.device)", "scsi{u}_a4091", []),
    "uae": ("UAE hardfile controller (uaehf.device, needs the UAE boot ROM)", "uae{u}",
            ["boot_rom_uae=automatic"]),
}
ALIASES = {"a600": "gayle", "a1200": "gayle", "ide": "gayle", "a4000": "a4000ide", "a590": "a2091",
           "a3000scsi": "a3000", "uaehf": "uae"}


def find_board_rom(controller, rom_dirs=None):
    for d in (rom_dirs or ROM_DIRS):
        for name in BOARD_ROMS.get(controller, []):
            p = os.path.join(d, name)
            if os.path.isfile(p):
                return p
    return None


def floppy1_lines(adf_path):
    return ["nr_floppies=2", "floppy1type=0", f"floppy1={adf_path}", "floppy1wp=false"]


def config_lines(controller, hdf_path=None, adf_path=None, rom_path=None, unit=0, rdb=None, readonly=False):
    """Amiberry option lines ("key=value") attaching the media.

    controller: gayle | a4000ide | a3000 | a2091 (A590) | a4091 | uae | None
    (None: only the ADF). rdb: True if the hardfile has an RDB (geometry
    0,0,0 makes Amiberry treat it as a whole disk and read the RDB); None
    asks the file. rom_path: the board ROM for a2091/a4091 (searched in
    ~/Amiberry/ROMs when not given)."""
    lines = []
    if adf_path:
        lines += floppy1_lines(adf_path)
    if not controller or not hdf_path:
        return lines
    c = ALIASES.get(controller.lower(), controller.lower())
    if c not in CONTROLLERS:
        raise ValueError(f"unknown controller {controller!r} (one of {', '.join(CONTROLLERS)})")
    _, hdc, extra = CONTROLLERS[c]
    lines += extra
    if c in ("a2091", "a4091"):
        rom_path = rom_path or find_board_rom(c)
        if not rom_path:
            raise FileNotFoundError(f"no {c} boot ROM found ({', '.join(BOARD_ROMS[c])})")
        lines.append(f"{c}_rom_file={rom_path}")
    if rdb is None:
        rdb = bool(verify(hdf_path).get("rdb"))
    geom = "0,0,0,512" if rdb else f"{HDF_SECS},{HDF_HEADS},2,512"
    # rw,DEVICE:path,sectors,surfaces,reserved,blocksize,bootpri,filesys,controller
    # (cfgfile.cpp mount_entry_values / parse_newfilesys_entry). Boot
    # priority -128: never boot from it.
    lines.append(f"hardfile2={'ro' if readonly else 'rw'},AMT0:{hdf_path},{geom},-128,,{hdc.format(u=unit)}")
    return lines


def default_controller(entry=None, options=None):
    """The controller a matrix entry's machine has built in (or names in
    entry['media']['controller']); None for machines without one."""
    entry = entry or {}
    options = dict(options or entry.get("options") or {})
    m = entry.get("media") or {}
    if m.get("controller"):
        return m["controller"]
    for k, c in (("a4091_rom_file", "a4091"), ("a2091_rom_file", "a2091")):
        if options.get(k):
            return c
    if str(options.get("scsi_a3000", "")).lower() == "true":
        return "a3000"
    ide = str(options.get("ide", "")).lower()
    if ide == "a4000":
        return "a4000ide"
    if ide.startswith("a600"):
        return "gayle"
    cs = str(options.get("chipset_compatible") or entry.get("model") or "").upper()
    # the A4000T has the A4000's motherboard IDE (Amiberry cfgfile.cpp CP_A4000T: cs_ide = IDE_A4000)
    return {"A600": "gayle", "A1200": "gayle", "A4000": "a4000ide", "A4000T": "a4000ide", "A3000": "a3000"}.get(cs)


def make_set(media_dir, controller=None, size_mb=8, rdb=True, adf=True, unit=0, rom_path=None,
             run_uuid=None, partition=False, tag="amt", pcmcia_sram=False):
    """Creates the media in media_dir and returns {"options": {...}, "media": [...], "lines": [...]}."""
    media_dir = os.path.abspath(media_dir)
    os.makedirs(media_dir, exist_ok=True)
    u = _new_uuid(run_uuid)
    media, adf_path, hdf_path = [], None, None
    if adf:
        adf_path = os.path.join(media_dir, f"{tag}-df1.adf")
        media.append(create_adf(adf_path, u))
    c = ALIASES.get((controller or "").lower(), (controller or "").lower()) or None
    if c:
        hdf_path = os.path.join(media_dir, f"{tag}-{c}-{'rdb' if rdb else 'plain'}.hdf")
        media.append(create_rdb_hdf(hdf_path, size_mb, u, partition) if rdb else create_hdf(hdf_path, size_mb, u))
        if c in ("a2091", "a4091") and rom_path is None:
            src = find_board_rom(c)
            if src:
                os.makedirs(os.path.join(media_dir, "roms"), exist_ok=True)
                rom_path = os.path.join(media_dir, "roms", os.path.basename(src))
                if not os.path.exists(rom_path):
                    shutil.copyfile(src, rom_path)
    lines = config_lines(c, hdf_path, adf_path, rom_path, unit, rdb)
    opts = {}
    for line in lines:
        k, v = line.split("=", 1)
        opts[k] = v
    if pcmcia_sram:
        sram = os.path.join(media_dir, f"{tag}-pcmcia-sram.img")
        media.append(create_pcmcia_sram(sram, u))
        po = pcmcia_sram_options(sram)
        opts.update(po)
        lines += [f"{k.split('#')[0]}={v}" for k, v in po.items()]
    return {"options": opts, "lines": lines, "media": media, "controller": c, "uuid": u.hex()}


def prepare(entry=None, options=None, media_dir=None, seed=1):
    """Runner hook (runner.py _media_options): media for one matrix entry.
    entry['media'] may set controller / size_mb / rdb / adf / unit / rom."""
    entry = entry or {}
    m = entry.get("media") or {}
    c = default_controller(entry, options)
    cs = str((options or entry.get("options") or {}).get("chipset_compatible") or entry.get("model") or "").upper()
    sram = m.get("pcmcia_sram", cs in ("A600", "A1200"))   # every Gayle machine gets the test SRAM card
    return make_set(media_dir, c, size_mb=m.get("size_mb", 8), rdb=m.get("rdb", True), adf=m.get("adf", True),
                    unit=m.get("unit", 0), rom_path=m.get("rom"), partition=m.get("partition", False),
                    pcmcia_sram=sram)


def main(argv=None):
    import argparse
    import json
    ap = argparse.ArgumentParser(prog="amitorture.media", description="amitorture test-owned media")
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("create", help="create media in DIR and print the Amiberry option lines")
    c.add_argument("dir")
    c.add_argument("--controller", choices=sorted(set(CONTROLLERS) | set(ALIASES)))
    c.add_argument("--size-mb", type=float, default=8)
    c.add_argument("--plain", action="store_true", help="hardfile without RDB")
    c.add_argument("--partition", action="store_true", help="RDB with one non-mountable partition")
    c.add_argument("--no-adf", action="store_true")
    c.add_argument("--unit", type=int, default=0)
    c.add_argument("--rom", help="board ROM (a2091/a4091)")
    c.add_argument("--json", action="store_true")
    d = sub.add_parser("describe", help="check and describe media files")
    d.add_argument("files", nargs="+")
    a = ap.parse_args(argv)
    if a.cmd == "create":
        r = make_set(a.dir, a.controller, a.size_mb, not a.plain, not a.no_adf, a.unit, a.rom, None, a.partition)
        if a.json:
            print(json.dumps(r, indent=1))
        else:
            for line in r["lines"]:
                print(line)
        return 0
    rc = 0
    for f in a.files:
        print(describe(f))
        rc |= 0 if verify(f)["ok"] else 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
