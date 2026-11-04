"""Isolation check: the user's real Amiberry dirs must not change during a run.

The runner never writes there itself; this catches the emulator doing it
(e.g. because an environment variable did not take). Paths follow Amiberry's
own resolution (src/osdep/amiberry.cpp): XDG_CONFIG_HOME/amiberry,
XDG_DATA_HOME/amiberry, and the home dir (AMIBERRY_HOME_DIR or ~/Amiberry).
All taken from the runner's *own* environment, i.e. the user's real one.
"""
from __future__ import annotations

import os
import threading
import time


def user_amiberries(home=None):
    """{pid: command line} of running Amiberry processes that use the user's
    real environment (HOME is the user's home), i.e. not an isolated
    amitorture emulator: those read and write the user's own config."""
    home = os.path.realpath(home or os.path.expanduser("~"))
    out = {}
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            with open(f"/proc/{pid}/comm", "rb") as fh:
                if fh.read().strip() != b"amiberry":
                    continue
            with open(f"/proc/{pid}/environ", "rb") as fh:
                env = dict(x.split(b"=", 1) for x in fh.read().split(b"\0") if b"=" in x)
            with open(f"/proc/{pid}/cmdline", "rb") as fh:
                cmd = fh.read().replace(b"\0", b" ").decode(errors="replace").strip()
        except (OSError, ValueError):
            continue
        h = env.get(b"HOME")
        if h is None or os.path.realpath(h.decode(errors="replace")) == home:
            out[int(pid)] = cmd[:200]
    return out


def watched_paths(environ=None, extra=()):
    env = os.environ if environ is None else environ
    home = env.get("HOME") or os.path.expanduser("~")
    cfg = env.get("XDG_CONFIG_HOME") or os.path.join(home, ".config")
    data = env.get("XDG_DATA_HOME") or os.path.join(home, ".local", "share")
    ahome = env.get("AMIBERRY_HOME_DIR") or os.path.join(home, "Amiberry")
    paths = [
        # (path, recursive, severity)
        (os.path.join(cfg, "amiberry"), True, "fail"),
        (os.path.join(data, "amiberry"), True, "fail"),
        (os.path.join(home, ".amiberry"), True, "fail"),           # legacy location
        (os.path.join(ahome, "Configurations"), True, "fail"),
        (os.path.join(ahome, "SaveStates"), True, "fail"),
        (os.path.join(ahome, "Screenshots"), True, "fail"),
        (ahome, False, "fail"),                                    # Amiberry.log etc. at the top
    ]
    if env.get("AMIBERRY_CONFIG_DIR"):
        paths.append((env["AMIBERRY_CONFIG_DIR"], True, "fail"))
    for p, rec, sev in extra:
        paths.append((p, rec, sev))
    seen, out = set(), []
    for p, rec, sev in paths:
        p = os.path.abspath(p)
        if p not in seen:
            seen.add(p)
            out.append((p, rec, sev))
    return out


def _stat(p):
    try:
        st = os.lstat(p)
    except OSError:
        return None
    return (st.st_mtime_ns, st.st_size, st.st_mode)


def snapshot(paths):
    snap = {}
    for root, recursive, sev in paths:
        snap[root] = (_stat(root), sev)
        if not os.path.isdir(root):
            continue
        if recursive:
            for dirpath, dirnames, filenames in os.walk(root):
                for n in dirnames + filenames:
                    p = os.path.join(dirpath, n)
                    snap[p] = (_stat(p), sev)
        else:
            try:
                names = os.listdir(root)
            except OSError:
                names = []
            for n in names:
                p = os.path.join(root, n)
                snap[p] = (_stat(p), sev)
    return snap


def compare(before, after):
    """Returns a list of changes: {path, change, severity, before, after}."""
    out = []
    for p in sorted(set(before) | set(after)):
        b, sb = before.get(p, (None, None))
        a, sa = after.get(p, (None, None))
        sev = sb or sa
        if b == a:
            continue
        if b is None:
            change = "created"
        elif a is None:
            change = "deleted"
        elif b[1] != a[1]:
            change = "size changed"
        else:
            change = "modified (mtime)"
        out.append({"path": p, "change": change, "severity": sev,
                    "before": None if b is None else {"mtime_ns": b[0], "size": b[1]},
                    "after": None if a is None else {"mtime_ns": a[0], "size": a[1]}})
    return out


class Guard:
    """with Guard(paths) as g: ...; then g.changes / g.ok."""

    SAMPLE_S = 5.0

    def __init__(self, paths, sample=True):
        self.paths = paths
        self.before = None
        self.changes = []
        # Amiberry instances on the user's real config seen during the run:
        # the likely cause of a change (the guard cannot prove who wrote).
        self.user_instances = {}
        self._sample = sample
        self._stop = threading.Event()

    def _note_user_instances(self):
        now = time.strftime("%H:%M:%S")
        for pid, cmd in user_amiberries().items():
            u = self.user_instances.setdefault(pid, {"pid": pid, "cmd": cmd, "first_seen": now})
            u["last_seen"] = now

    def _sampler(self):
        while not self._stop.wait(self.SAMPLE_S):
            self._note_user_instances()

    def __enter__(self):
        self.before = snapshot(self.paths)
        if self._sample:
            self._note_user_instances()
            threading.Thread(target=self._sampler, daemon=True).start()
        return self

    def check(self):
        if self._sample:
            self._note_user_instances()
        self.changes = compare(self.before, snapshot(self.paths))
        return self.changes

    def __exit__(self, *exc):
        self.check()
        self._stop.set()
        return False

    @property
    def ok(self):
        return not any(c["severity"] == "fail" for c in self.changes)

    def report(self):
        return {"ok": self.ok, "watched": [{"path": p, "recursive": r, "severity": s, "exists": os.path.exists(p)}
                                           for p, r, s in self.paths],
                "changes": self.changes,
                "user_amiberry_seen": sorted(self.user_instances.values(), key=lambda u: u["pid"])}
