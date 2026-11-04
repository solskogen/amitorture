"""Host / tool environment facts recorded with every result."""
from __future__ import annotations

import os
import platform
import subprocess
import sys

from .adf import REPO


def git_rev(repo=REPO):
    def git(*a):
        p = subprocess.run(["git", "-C", repo, *a], capture_output=True, text=True, timeout=10)
        return p.stdout.strip() if p.returncode == 0 else None
    try:
        rev = git("rev-parse", "HEAD")
        status = git("status", "--porcelain")
    except (OSError, subprocess.TimeoutExpired):
        return {"rev": None, "note": "git not available"}
    return {"rev": rev or "none (no commits yet)", "dirty": bool(status), "repo": repo}


def host():
    u = platform.uname()
    d = {"os": u.system, "release": u.release, "version": u.version, "machine": u.machine,
         "node": u.node, "python": sys.version.split()[0], "python_executable": sys.executable,
         "cpu_count": os.cpu_count()}
    try:
        with open("/etc/os-release", encoding="utf-8") as fh:
            for line in fh:
                if line.startswith("PRETTY_NAME="):
                    d["distro"] = line.split("=", 1)[1].strip().strip('"')
    except OSError:
        pass
    try:
        with open("/proc/device-tree/model", "rb") as fh:
            d["board"] = fh.read().rstrip(b"\0").decode(errors="replace")
    except OSError:
        pass
    d["cpu_model"] = host_brief().get("cpu_model")
    return d


# ARM "CPU part" ids (/proc/cpuinfo) of common cores, for host_brief()
_ARM_PARTS = {"0xd03": "Cortex-A53", "0xd04": "Cortex-A35", "0xd05": "Cortex-A55", "0xd07": "Cortex-A57",
              "0xd08": "Cortex-A72", "0xd09": "Cortex-A73", "0xd0a": "Cortex-A75", "0xd0b": "Cortex-A76",
              "0xd0c": "Neoverse-N1", "0xd0d": "Cortex-A77", "0xd41": "Cortex-A78", "0xd44": "Cortex-X1",
              "0xd46": "Cortex-A510", "0xd47": "Cortex-A710", "0xd48": "Cortex-X2", "0xd4d": "Cortex-A715",
              "0xd4e": "Cortex-X3", "0xd80": "Cortex-A520", "0xd81": "Cortex-A720", "0xd82": "Cortex-X4"}
_BRIEF = None


def host_brief():
    """{"cpu_model", "cpus", "board"}: the host a timing verdict was measured
    on (host.speed_* failures name it)."""
    global _BRIEF
    if _BRIEF is not None:
        return dict(_BRIEF)
    model = None
    try:
        with open("/proc/cpuinfo", encoding="utf-8", errors="replace") as fh:
            info = fh.read()
        for line in info.splitlines():
            k, _, v = line.partition(":")
            k, v = k.strip().lower(), v.strip()
            if k in ("model name", "cpu model") and v:
                model = v
                break
        if model is None:
            parts = {v.strip() for k, _, v in (ln.partition(":") for ln in info.splitlines())
                     if k.strip().lower() == "cpu part"}
            names = sorted(_ARM_PARTS.get(p.lower(), f"ARM part {p}") for p in parts)
            model = " + ".join(names) or None
    except OSError:
        pass
    d = {"cpu_model": model or platform.processor() or platform.machine(), "cpus": os.cpu_count()}
    try:
        with open("/proc/device-tree/model", "rb") as fh:
            d["board"] = fh.read().rstrip(b"\0").decode(errors="replace")
    except OSError:
        pass
    _BRIEF = d
    return dict(d)
