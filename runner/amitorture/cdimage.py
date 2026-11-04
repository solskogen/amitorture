"""Bootable CD images for the CD32 and the CDTV (stdlib only).

The CD32 and the CDTV boot from CD: their extended ROM (the CD32's r40.60,
the CDTV's 1.0/2.x) mounts the disc with its cdfs and runs
``s/startup-sequence`` with the CD as SYS:. The disc is a plain ISO 9660
image (Amiberry mounts any file whose size is a multiple of 2048 bytes and
at least 16 KB as a one-track data CD, src/blkdev_cdimage.cpp parse_image):

    CD32.TM or CDTV.TM   the trademark file the boot ROM checks (from the
                         user's extended ROM, see TM_CRC below), pointed at
                         by the "TM" tag in the PVD's application-use area
    AMITORTURE           the payload as an AmigaDOS hunk executable
    S/STARTUP-SEQUENCE   one line: AMITORTURE

The executable is made from the (already parameter-baked) ADF, not from a
separate build: its one code hunk is the ADF's flattened image with the BSS
appended as zeros and the ADF's reloc table as HUNK_RELOC32 (the same
``*(base+r) += base`` LoadSeg does), and the ADF's 512-byte payload header
(run parameters, build id) copied into the slot after the
``AMT-CLI-HEADER!!`` marker. Offset 0 of the image is amt_cli_entry
(disk/rt/start.s), so AmigaDOS starts the payload there (CONTRACT §14).

The PVD's system identifier is "CDTV" (padded to 32 characters): it is one
of the identifiers the cdfs of both ROMs compares ("CDTV",
"COMMODORE_AMIGA", strings in the ROMs next to each other), and the one
Amiberry's own drive scan looks for (src/blkdev.cpp). Everything is
deterministic (fixed dates), so the same ADF gives the same image.
"""
from __future__ import annotations

import os
import struct

SECTOR = 2048
MIN_SECTORS = 2048         # 4 MB: see build_iso
MARKER = b"AMT-CLI-HEADER!!"
EXE_NAME = "AMITORTURE"
HUNK_HEADER, HUNK_CODE, HUNK_RELOC32, HUNK_END = 0x3F3, 0x3E9, 0x3EC, 0x3F2
DATE7 = bytes([126, 1, 1, 0, 0, 0, 0])        # 2026-01-01 00:00:00 GMT
DATE17 = b"2026010100000000\x00"

# The trademark files. Both boot ROMs compare the TM file on the disc byte
# for byte with a copy held in the ROM itself (compressed with debox.library,
# see debox_unpack):
#   CD32 (r40.60 cdstrap): reads one 2048-byte sector at the TM's LBA and
#     compares it with its CD32.TM (2048 bytes), else with the first 2048
#     bytes of the CDTV one;
#   CDTV (bootstrap "CheckTM" -> playerprefs.library LVO -138, which then
#     shows it: it is the boot screen's trademark picture): the length must
#     equal its CDTV.TM and the contents must match. CDTV.TM is two debox
#     streams one after the other in the ROM (6144 bytes, then 16008:
#     two 40x200 bitplanes and a 4-colour palette), 22152 bytes in all.
# Those files are Commodore's, so they are not in this repository: the runner
# decompresses them from the user's own extended ROM at run time and
# identifies them by CRC-32 (the CD32.TM one is also the CRC Amiberry's
# src/blkdev.cpp knows as "[CD32.TM]").
TM_CRC = {"CD32.TM": (2048, 0xe56c340f), "CDTV.TM": (22152, 0x37c532d7)}
CDTV_TM_PART1 = (6144, 0x0e506ea1)


class CdError(Exception):
    pass


# ---------------------------------------------------------------------------
# ADF -> hunk executable

def read_adf_payload(adf_bytes):
    """(header 512 bytes, image bytes, bss_size, [reloc offsets]) from an ADF (CONTRACT §1)."""
    hdr = adf_bytes[1024:1536]
    if hdr[:4] != b"AMTP":
        raise CdError("ADF has no AMTP payload header in sector 2")
    ver, image_size, bss, nrel, entry, sectors = struct.unpack_from(">6I", hdr, 4)
    blob = adf_bytes[1536:1536 + sectors * 512]
    image = blob[:image_size]
    relocs = list(struct.unpack_from(f">{nrel}I", blob, image_size)) if nrel else []
    if len(image) != image_size:
        raise CdError("ADF payload blob is truncated")
    return hdr, image, bss, relocs


def hunk_exe(adf_bytes):
    """A one-hunk AmigaDOS executable with the ADF's payload."""
    hdr, image, bss, relocs = read_adf_payload(adf_bytes)
    img = bytearray(image) + bytes(bss)
    at = img.find(MARKER)
    if at < 0:
        raise CdError("payload has no CLI header slot (AMT-CLI-HEADER!! marker): the disk was built before "
                      "disk/rt/start.s got amt_cli_entry (CONTRACT §14); rebuild it")
    if at + len(MARKER) + 512 > len(image):
        raise CdError("CLI header slot is not inside the image")
    img[at + len(MARKER):at + len(MARKER) + 512] = hdr
    if len(img) % 4:
        img += bytes(4 - len(img) % 4)
    n = len(img) // 4
    out = bytearray()
    out += struct.pack(">IIIII", HUNK_HEADER, 0, 1, 0, 0)
    out += struct.pack(">I", n)
    out += struct.pack(">II", HUNK_CODE, n) + img
    if relocs:
        out += struct.pack(">III", HUNK_RELOC32, len(relocs), 0)
        out += struct.pack(f">{len(relocs)}I", *sorted(relocs))
        out += struct.pack(">I", 0)
    out += struct.pack(">I", HUNK_END)
    return bytes(out), {"exe_bytes": len(out), "image_bytes": len(image), "bss_bytes": bss, "relocs": len(relocs),
                        "header_slot": at + len(MARKER)}


# ---------------------------------------------------------------------------
# ISO 9660

def _both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def _both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def _dirrec(name: bytes, lba, size, is_dir):
    ln = 33 + len(name)
    pad = b"\0" if len(name) % 2 == 0 else b""
    rec = struct.pack("<BB", ln + len(pad), 0) + _both32(lba) + _both32(size) + DATE7 \
        + bytes([2 if is_dir else 0, 0, 0]) + _both16(1) + bytes([len(name)]) + name + pad
    return rec


def _achars(s, n):
    return s.encode("ascii").upper().ljust(n, b" ")[:n]


def build_iso(files, volume_id="AMITORTURE", system_id="CDTV", tm_file=None):
    """files: {"PATH/NAME": bytes} with at most one directory level.
    tm_file: the trademark file the PVD's "TM" tag points at (see
    app_use_tags). Returns the ISO image bytes."""
    dirs = {"": []}
    for path in files:
        d, _, n = path.rpartition("/")
        if "/" in d:
            raise CdError("only one directory level supported")
        if d:
            dirs.setdefault(d, [])
            if d not in dirs[""]:
                dirs[""].append(d)
        dirs.setdefault(d, []).append(n)
    # layout: 16 system sectors, PVD, terminator, L path table, M path table,
    # one sector per directory, then the files
    lba = 20
    dir_lba = {}
    for d in [""] + sorted(k for k in dirs if k):
        dir_lba[d] = lba
        lba += 1
    file_lba = {}
    for path in sorted(files):
        file_lba[path] = lba
        lba += max(1, (len(files[path]) + SECTOR - 1) // SECTOR)
    # The CD32 ROM reads ahead 128 sectors from LBA 16 (Akiko "filling
    # buffer sector=16-143"); a disc shorter than its reads does not boot, so
    # the image is padded to MIN_SECTORS.
    total = max(lba + 256, MIN_SECTORS)
    img = bytearray(total * SECTOR)

    def put(sector, data):
        if len(data) > SECTOR:
            raise CdError("directory or path table larger than one sector")
        img[sector * SECTOR:sector * SECTOR + len(data)] = data

    # directories
    for d, lb in dir_lba.items():
        parent = dir_lba[""]
        recs = _dirrec(b"\0", lb, SECTOR, True) + _dirrec(b"\1", parent if d else lb, SECTOR, True)
        entries = []
        for n in dirs[d]:
            full = f"{d}/{n}" if d else n
            if d == "" and n in dirs and full not in files:
                entries.append((n.upper().encode(), dir_lba[n], SECTOR, True))
            else:
                entries.append(((n.upper() + ";1").encode(), file_lba[full], len(files[full]), False))
        for name, l, size, isd in sorted(entries):
            recs += _dirrec(name, l, size, isd)
        put(lb, recs)
    for path, lb in file_lba.items():
        img[lb * SECTOR:lb * SECTOR + len(files[path])] = files[path]
    # path tables (root = 1, its subdirectories have parent 1)
    order = [""] + sorted(k for k in dirs if k)

    def ptable(big):
        out = b""
        for d in order:
            name = d.upper().encode() if d else b"\0"
            out += bytes([len(name), 0]) + struct.pack(">I" if big else "<I", dir_lba[d]) \
                + struct.pack(">H" if big else "<H", 1) + name + (b"\0" if len(name) % 2 else b"")
        return out
    lpt, mpt = ptable(False), ptable(True)
    put(18, lpt)
    put(19, mpt)
    # primary volume descriptor
    pvd = bytearray(SECTOR)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[6] = 1
    pvd[8:40] = _achars(system_id, 32)
    pvd[40:72] = _achars(volume_id, 32)
    pvd[80:88] = _both32(total)
    pvd[120:124] = _both16(1)
    pvd[124:128] = _both16(1)
    pvd[128:132] = _both16(SECTOR)
    pvd[132:140] = _both32(len(lpt))
    pvd[140:144] = struct.pack("<I", 18)
    pvd[148:152] = struct.pack(">I", 19)
    root = _dirrec(b"\0", dir_lba[""], SECTOR, True)
    pvd[156:156 + len(root)] = root
    pvd[190:318] = _achars("AMITORTURE", 128)        # volume set
    pvd[318:446] = _achars("AMITORTURE", 128)        # publisher
    pvd[446:574] = _achars("AMITORTURE RUNNER", 128)  # data preparer
    pvd[574:702] = _achars("AMITORTURE", 128)        # application
    pvd[702:813] = b" " * 111                         # copyright/abstract/bibliographic file ids
    if tm_file:
        tags = app_use_tags(len(files[tm_file]), file_lba[tm_file])
        pvd[884:884 + len(tags)] = tags
    for off in (813, 830):
        pvd[off:off + 17] = DATE17
    for off in (847, 864):
        pvd[off:off + 17] = b"0000000000000000\x00"
    pvd[881] = 1
    put(16, bytes(pvd))
    term = bytearray(SECTOR)
    term[0] = 255
    term[1:6] = b"CD001"
    term[6] = 1
    put(17, bytes(term))
    return bytes(img)


def _lzss(src, out_size):
    """debox method 1: flag bytes, LSB first: 1 = literal byte, 0 = two bytes
    b0 b1, copy (b1 & 15) + 3 bytes from offset b0 | (b1 & 0xf0) << 4 back."""
    out = bytearray()
    i = 0
    while i < len(src) and len(out) < out_size:
        flags = src[i]
        i += 1
        for bit in range(8):
            if len(out) >= out_size or i >= len(src):
                break
            if flags >> bit & 1:
                out.append(src[i])
                i += 1
            else:
                if i + 1 >= len(src):
                    return None
                b0, b1 = src[i], src[i + 1]
                i += 2
                off = b0 | (b1 & 0xf0) << 4
                if not 0 < off <= len(out):
                    return None
                for _ in range((b1 & 15) + 3):
                    out.append(out[-off])
    return bytes(out) if len(out) == out_size else None


def _vrle(src, out_size):
    """debox method 2: u16 stride, then run codes written column by column
    (each byte goes `stride` further; at the end, the next column): n > 0 =
    n literal bytes, n < 0 (signed) = the next byte -n times, n = 0 = skip
    the next byte's count (unchanged from the reference frame: zero here)."""
    if len(src) < 2:
        return None
    stride = struct.unpack_from(">H", src, 0)[0]
    if not stride:
        return None
    out = bytearray(out_size)
    st = {"p": 0, "col": 0}

    def put(v):
        if v is not None:
            out[st["p"]] = v
        st["p"] += stride
        if st["p"] >= out_size:
            st["col"] += 1
            st["p"] = st["col"]
        return st["col"] >= stride

    i = 2
    while i < len(src):
        n = src[i]
        i += 1
        if n == 0:
            if i >= len(src):
                return None
            cnt = src[i]
            i += 1
            for _ in range(cnt):
                if put(None):
                    return bytes(out)
        elif n < 128:
            if i + n > len(src):
                return None
            for k in range(n):
                if put(src[i + k]):
                    return bytes(out)
            i += n
        else:
            if i >= len(src):
                return None
            v = src[i]
            i += 1
            for _ in range(256 - n):
                if put(v):
                    return bytes(out)
    return None


def debox_unpack(buf, pos=0, depth=0):
    """Decompresses a debox.library stream at buf[pos:] (CD32 extended ROM
    debox.library LVO -36, which the CDTV's playerprefs uses as well).
    Returns (bytes, position after the stream) or (None, None).

    Chunk header (16 bytes): u16 at 4 = flags | method (low byte: 1 LZSS,
    2 column RLE), u32 out size at 8, u32 in size at 12, then the data.
    Flags: 0x8000 another chunk follows, 0x4000 the data starts with a u32
    size and decodes to an inner debox stream (decoded again), 0x1000 the in
    size counts one pad byte. Chunks are padded to even sizes."""
    out = bytearray()
    while True:
        if depth > 4 or pos + 16 > len(buf):
            return None, None
        flags = struct.unpack_from(">H", buf, pos + 4)[0]
        osz, isz = struct.unpack_from(">II", buf, pos + 8)
        if not 0 < osz <= 1 << 20 or pos + 16 + isz > len(buf):
            return None, None
        data = buf[pos + 16:pos + 16 + isz]
        n = isz - (1 if flags & 0x1000 else 0)
        method = flags & 0xff
        if flags & 0x4000:
            if n < 4:
                return None, None
            mid = struct.unpack_from(">I", data, 0)[0]
            inner = _lzss(data[4:n], mid) if method == 1 else (_vrle(data[4:n], mid) if method == 2 else None)
            o = debox_unpack(inner, 0, depth + 1)[0] if inner else None
        elif method == 1:
            o = _lzss(data[:n], osz)
        elif method == 2:
            o = _vrle(data[:n], osz)
        else:
            o = None
        if o is None or len(o) != osz:
            return None, None
        out += o
        pos += 16 + isz + (isz & 1)
        if not flags & 0x8000:
            return bytes(out), pos


def lzss_unpack(blob):
    """One debox stream from the start of `blob` (bytes or None)."""
    return debox_unpack(blob, 0)[0]


def trademarks(rom_paths):
    """{"CD32.TM": bytes, "CDTV.TM": bytes} found in the given ROM files: the
    CD32 extended ROM has CD32.TM (and the first part of CDTV.TM only), a
    CDTV extended ROM has CDTV.TM."""
    import zlib

    def crc(b):
        return zlib.crc32(b) & 0xffffffff
    found = {}
    sizes = {TM_CRC["CD32.TM"][0], CDTV_TM_PART1[0]}
    for path in rom_paths:
        if not path or not os.path.isfile(path):
            continue
        with open(path, "rb") as fh:
            rom = fh.read()
        for i in range(0, len(rom) - 16, 2):
            if rom[i + 5] not in (1, 2) or struct.unpack_from(">I", rom, i + 8)[0] not in sizes:
                continue
            data, nxt = debox_unpack(rom, i)
            if data is None:
                continue
            if (len(data), crc(data)) == TM_CRC["CD32.TM"]:
                found.setdefault("CD32.TM", data)
            elif (len(data), crc(data)) == CDTV_TM_PART1 and "CDTV.TM" not in found:
                part2, _ = debox_unpack(rom, nxt)
                if part2 is not None and (len(data) + len(part2), crc(data + part2)) == TM_CRC["CDTV.TM"]:
                    found["CDTV.TM"] = data + part2
    return found


def app_use_tags(tm_len, tm_lba):
    """The PVD application-use area (from byte 884) as the cdfs of both
    ROMs parses it: a list of (u16 tag, u16 data length, data), ended by tag
    0. The "TM" tag tells the boot ROM where the trademark is: u32 length,
    then up to four u32 LBAs of copies (CD32 cdfs.library 40.8 / CDTV cdfs
    24.9 tag switch; the CDTV bootstrap reads `length` bytes at each LBA, the
    CD32 cdstrap one sector at the first)."""
    return struct.pack(">HH5I", 0x544D, 20, tm_len, tm_lba, 0, 0, 0) + struct.pack(">HH", 0, 0)


def boot_files(adf_bytes, command=None, tm=None):
    exe, info = hunk_exe(adf_bytes)
    cmd = command or EXE_NAME
    files = {EXE_NAME: exe, "S/STARTUP-SEQUENCE": (cmd + "\n").encode("ascii")}
    files.update(tm or {})
    return files, info


def make_boot_iso(adf_path, iso_path, command=None, roms=(), machine="CD32"):
    """Writes a bootable CD32/CDTV ISO for the (baked) ADF; the trademark
    files come from the extended ROM(s) in `roms`. The PVD's TM tag points at
    CD32.TM for a CD32 (a native CD32 disc; the cdstrap would also take the
    CDTV one, as a CDTV title) and at CDTV.TM for a CDTV. Returns info."""
    with open(adf_path, "rb") as fh:
        adf = fh.read()
    tm = trademarks(roms)
    if not tm:
        raise CdError("no CD32.TM/CDTV.TM trademark data found in " + ", ".join(map(str, roms))
                      + ": the CD32/CDTV boot ROM refuses a disc without it")
    files, info = boot_files(adf, command, tm)
    info["trademarks"] = sorted(tm)
    want = "CD32.TM" if machine.upper() == "CD32" else "CDTV.TM"
    tm_file = want if want in tm else sorted(tm)[0]
    info["tm_tag"] = tm_file
    iso = build_iso(files, tm_file=tm_file)
    tmp = iso_path + ".part"
    with open(tmp, "wb") as fh:
        fh.write(iso)
    os.replace(tmp, iso_path)
    info.update({"iso": iso_path, "iso_bytes": len(iso), "files": sorted(files),
                 "startup_sequence": files["S/STARTUP-SEQUENCE"].decode().strip()})
    return info


def list_iso(data):
    """[(path, lba, size)] of an image made by build_iso (for tests/inspection)."""
    out = []
    pvd = data[16 * SECTOR:17 * SECTOR]
    if pvd[1:6] != b"CD001":
        raise CdError("no PVD")

    def walk(lba, prefix):
        sec = data[lba * SECTOR:(lba + 1) * SECTOR]
        i = 0
        while i < len(sec) and sec[i]:
            ln = sec[i]
            ext = struct.unpack_from("<I", sec, i + 2)[0]
            size = struct.unpack_from("<I", sec, i + 10)[0]
            flags = sec[i + 25]
            nl = sec[i + 32]
            name = sec[i + 33:i + 33 + nl]
            if name not in (b"\0", b"\1"):
                n = name.decode().split(";")[0]
                if flags & 2:
                    walk(ext, prefix + n + "/")
                else:
                    out.append((prefix + n, ext, size))
            i += ln
    root = pvd[156:190]
    walk(struct.unpack_from("<I", root, 2)[0], "")
    return out
