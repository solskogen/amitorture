"""The test disk: locating it, baking the run parameters, reading its header."""
from __future__ import annotations

import contextlib
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys

from .emulator import RunnerError, sha256_file

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DEFAULT_ADF = os.path.join(REPO, "disk", "build", "amitorture.adf")
BENCHMARK_ADF = os.path.join(REPO, "disk", "build", "benchmarks", "amitorture.adf")
DEFAULT_SET = os.path.join(REPO, "disk", "build", "set")
MKADF = os.path.join(REPO, "tools", "mkadf.py")
ADF_SIZE = 901120


def header_info(path):
    """Payload header + run params of an ADF (CONTRACT.md §1, §4), read directly."""
    with open(path, "rb") as fh:
        data = fh.read()
    info = {"path": os.path.abspath(path), "size": len(data), "sha256": sha256_file(path)}
    if len(data) != ADF_SIZE:
        info["problem"] = f"size {len(data)}, expected {ADF_SIZE}"
    hdr = data[1024:1536]
    info["bootblock_magic"] = data[:4].decode("latin-1")
    if hdr[:4] != b"AMTP":
        info["problem"] = "no AMTP payload header in sector 2"
        return info
    (ver, image_size, bss, nrel, entry, sectors, crc, alloc, flags,
     data_size, data_sectors, data_alloc) = struct.unpack_from(">12I", hdr, 4)
    info.update({"header_version": ver, "image_size": image_size, "bss_size": bss, "reloc_count": nrel,
                 "entry_offset": entry, "blob_sectors": sectors, "blob_crc32": f"{crc:08x}",
                 "alloc_size": alloc, "alloc_flags": f"{flags:#x}",
                 "data_size": data_size, "data_sectors": data_sectors, "data_alloc": data_alloc,
                 "build_id": hdr[128:192].split(b"\0")[0].decode("ascii", "replace")})
    p = hdr[64:128]
    if p[:4] == b"PARM":
        mode, seed, end, fl, baud, mhi, mlo = struct.unpack_from(">7I", p, 4)
        info["params"] = {"mode": mode, "seed": seed, "end_action": end, "flags": fl, "baud": baud,
                          "modules": f"{(mhi << 32) | mlo:016x}"}
    else:
        info["params"] = None
    return info


def bake(src, dst, params: dict, cache_dir=None, lock=None):
    """Copies src to dst with the run parameters baked in by tools/mkadf.py.
    cache_dir: keep each (disk, params) baked once there and copy it (the
    test floppy is write-protected, so a copy is as good as a bake)."""
    if cache_dir and os.path.isfile(src):
        st = os.stat(src)
        key = hashlib.sha256(json.dumps([os.path.realpath(src), st.st_size, st.st_mtime_ns,
                                         sorted((str(k), str(v)) for k, v in (params or {}).items())])
                             .encode()).hexdigest()[:20]
        cached = os.path.join(cache_dir, key + ".adf")
        with (lock or contextlib.nullcontext()):
            hit = os.path.isfile(cached)
            if not hit:
                os.makedirs(cache_dir, exist_ok=True)
                bake(src, cached + ".tmp", params)
                os.replace(cached + ".tmp", cached)
        shutil.copyfile(cached, dst)
        info = header_info(dst)
        info["mkadf_argv"] = [sys.executable, MKADF, "--in", src, "-o", dst] + (
            ["--param", *[f"{k}={v}" for k, v in params.items()]] if params else [])
        info["baked_from_cache"] = hit
        return info
    if not os.path.isfile(src):
        raise RunnerError(f"test disk {src} not found (build the disk set with `make -C disk` and use --disk-set disk/build/set, or build one ADF with `make -C disk MODS=\"rt <modules>\"` and pass --adf PATH)")
    if not os.path.isfile(MKADF):
        raise RunnerError(f"{MKADF} not found: the ADF builder is needed to bake the run parameters "
                          "(CONTRACT.md §4) into the disk")
    argv = [sys.executable, MKADF, "--in", src, "-o", dst]
    if params:
        argv += ["--param", *[f"{k}={v}" for k, v in params.items()]]
    p = subprocess.run(argv, capture_output=True, text=True, timeout=60)
    if p.returncode != 0:
        raise RunnerError(f"mkadf.py failed ({p.returncode}): {' '.join(argv)}\n{p.stderr.strip()}")
    info = header_info(dst)
    if info.get("params") is None:
        raise RunnerError(f"mkadf.py wrote {dst} without a PARM block")
    info["mkadf_argv"] = argv
    return info


def disk_set(directory):
    """[(name, path)] of a disk set in name order: amitorture-1-core.adf ->
    ("1-core", path). Any *.adf in the directory counts."""
    out = []
    for f in sorted(os.listdir(directory)):
        if f.lower().endswith(".adf"):
            stem = f[:-4]
            name = stem[len("amitorture-"):] if stem.startswith("amitorture-") else stem
            out.append((name, os.path.abspath(os.path.join(directory, f))))
    return out
