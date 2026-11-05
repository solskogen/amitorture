#!/usr/bin/env python3
"""Machine-wide limit on concurrent emulator instances.

Every launcher (tests/smoke_disk.py, the runner, any ad-hoc script) must hold a
slot while an emulator runs, and should tell the slot the emulator's PID:

    from amislot import emulator_slot
    with emulator_slot() as slot:        # blocks until a slot is free
        proc = subprocess.Popen(...)
        slot.set_pid(proc.pid)           # optional: a direct child of the slot
                                         # holder is recognised anyway
        ...
        proc.wait()                      # leave the slot only after it exited

Design:
- One lock directory per user/machine (~/.cache/amitorture/locks, or
  $AMT_LOCK_DIR), so several checkouts or worktrees share the same limit.
- Number of slots: os.cpu_count(); AMT_MAX_EMU may lower it, never raise it.
- A slot is taken under a gate lock: amiberry processes that are alive but
  not registered by any slot holder (orphans whose launcher died, launchers
  that don't use this module) count against the limit too:
  held slots + orphans must stay below the limit.
- Locks are released by the kernel when the holder exits, even on a crash.
- `Slot.wait_gone(pid)` lets a launcher make sure the emulator really exited
  before it releases its slot.

`with emulator_slot() as i` still yields the slot number (an int subclass).
"""
import contextlib
import fcntl
import os
import sys
import time


def lock_dir():
    d = os.environ.get("AMT_LOCK_DIR")
    if not d:
        base = os.environ.get("XDG_CACHE_HOME") or os.path.join(os.path.expanduser("~"), ".cache")
        d = os.path.join(base, "amitorture", "locks")
    os.makedirs(d, exist_ok=True)
    return d


def max_slots():
    n = os.cpu_count() or 1
    try:
        req = int(os.environ.get("AMT_MAX_EMU", "0"))
    except ValueError:
        req = 0
    return min(req, n) if req > 0 else n


def _amiberry_procs():
    """{pid: ppid} of live amiberry processes."""
    procs = {}
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            with open(f"/proc/{pid}/stat", "rb") as fh:
                st = fh.read()
            name = st[st.index(b"(") + 1:st.rindex(b")")]
            rest = st[st.rindex(b")") + 2:].split()
        except (OSError, ValueError):
            continue
        if name == b"amiberry" and rest and rest[0] not in (b"Z", b"X"):
            procs[int(pid)] = int(rest[1])
    return procs


def _amiberry_pids():
    pids = set()
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            with open(f"/proc/{pid}/stat", "rb") as fh:
                st = fh.read()
            name = st[st.index(b"(") + 1:st.rindex(b")")]
            state = st[st.rindex(b")") + 2:st.rindex(b")") + 3]
        except (OSError, ValueError):
            continue
        if name == b"amiberry" and state not in (b"Z", b"X"):
            pids.add(int(pid))
    return pids


def running_emulators():
    """All amiberry processes alive on this machine."""
    return len(_amiberry_pids())


def _registered(d, n):
    """(emulator pids, holder pids) written by current slot holders."""
    emus, holders = set(), set()
    for i in range(n):
        try:
            with open(os.path.join(d, f"emu-slot-{i}")) as fh:
                for line in fh:
                    w = line.split()
                    if len(w) >= 2 and w[0] == "emu":
                        emus.add(int(w[1]))
                    elif len(w) >= 2 and w[0] == "holder":
                        holders.add(int(w[1]))
        except (OSError, ValueError):
            pass
    return emus, holders


def _orphans(d, n):
    """Live amiberry processes not accounted for by a slot holder: neither
    registered with set_pid() nor a direct child of a holder process."""
    emus, holders = _registered(d, n)
    return [p for p, pp in _amiberry_procs().items() if p not in emus and pp not in holders]


class Slot(int):
    """The slot number, plus helpers bound to the held lock file."""

    def __new__(cls, num, fd):
        o = int.__new__(cls, num)
        o._fd = fd
        return o

    def set_pid(self, pid):
        """Record the emulator's PID in our slot file."""
        os.lseek(self._fd, 0, os.SEEK_END)
        os.write(self._fd, f"emu {int(pid)}\n".encode())

    @staticmethod
    def wait_gone(pid, timeout=30.0):
        """True once the process has exited."""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            try:
                os.kill(int(pid), 0)
            except OSError:
                return True
            if int(pid) not in _amiberry_pids():
                return True
            time.sleep(0.2)
        return False


@contextlib.contextmanager
def emulator_slot(poll=0.5, quiet=False, cancel=None, cancel_exc=KeyboardInterrupt):
    """`cancel`: optional callable; while waiting for a slot, a true result
    gives up and raises `cancel_exc` (the runner's Ctrl+C)."""
    d = lock_dir()
    n = max_slots()
    gate = os.open(os.path.join(d, "gate"), os.O_RDWR | os.O_CREAT, 0o644)
    waited = False
    try:
        while True:
            fd = num = None
            fcntl.flock(gate, fcntl.LOCK_EX)
            try:
                held = 0
                for i in range(n):
                    f = os.open(os.path.join(d, f"emu-slot-{i}"), os.O_RDWR | os.O_CREAT, 0o644)
                    try:
                        fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    except OSError:
                        os.close(f)
                        held += 1
                        continue
                    # Unlocked registrations cannot hide surviving orphans.
                    os.ftruncate(f, 0)
                    if fd is None:
                        fd, num = f, i
                    else:
                        fcntl.flock(f, fcntl.LOCK_UN)
                        os.close(f)
                if fd is not None:
                    orphans = len(_orphans(d, n))
                    if held + orphans < n:
                        os.ftruncate(fd, 0)
                        os.write(fd, f"holder {os.getpid()} {' '.join(sys.argv)[:200]}\n".encode())
                    else:
                        os.ftruncate(fd, 0)
                        fcntl.flock(fd, fcntl.LOCK_UN)
                        os.close(fd)
                        fd = None
            finally:
                fcntl.flock(gate, fcntl.LOCK_UN)
            if fd is not None:
                try:
                    yield Slot(num, fd)
                finally:
                    try:
                        os.ftruncate(fd, 0)
                    except OSError:
                        pass
                    fcntl.flock(fd, fcntl.LOCK_UN)
                    os.close(fd)
                return
            if not waited and not quiet:
                print(f"amislot: all {n} emulator slots busy (held or orphan emulators running), waiting",
                      file=sys.stderr, flush=True)
                waited = True
            if cancel is not None and cancel():
                raise cancel_exc("cancelled while waiting for an emulator slot")
            time.sleep(poll)
    finally:
        os.close(gate)
