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
    return d
