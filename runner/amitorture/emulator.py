"""Launching Amiberry isolated, and talking to it (IPC socket, serial over TCP).

IPC (src/osdep/amiberry_ipc_socket.cpp): a Unix socket whose path Amiberry
prints as ``IPC: Listening on <path>`` (under $XDG_RUNTIME_DIR). Requests are
lines ``CMD\\targ\\targ\\n``; replies are lines ``OK[\\tfield...]`` or
``ERROR\\tmessage``. Requests are handled on the emulation thread once per
frame, as many as fit in ~5 ms, so we pipeline batches of requests in one
write. READ_MEM takes ``<addr> <width>`` with width 1, 2 or 4 and replies
``OK\\t<value in decimal>``; one value per request (there is no block read).
"""
from __future__ import annotations

import hashlib
import os
import re
import shutil
import signal
import socket
import subprocess
import threading
import time

PORT_RANGE = (23540, 23599)   # this runner's serial/GDB port range


class RunnerError(Exception):
    """A problem with the host setup (not a finding about the emulator)."""


class IsolationError(RunnerError):
    """The user's files changed while the runner owned the emulator."""


# ---------------------------------------------------------------------------
# Ctrl+C. Emulators run in their own session (start_new_session), so the
# terminal's SIGINT never reaches them: the runner stops them itself.
# cli.main installs a SIGINT/SIGTERM handler that calls interrupt(); from then
# on no emulator is started (Process, run_amiberry and slot waits raise
# Interrupted) and every running one gets SIGTERM. The watch loops raise
# Interrupted at their next poll; their `finally` blocks stop the emulator
# and release its slot.
class Interrupted(BaseException):
    """Ctrl+C. A BaseException, like KeyboardInterrupt, so that the
    `except Exception` guards (one entry's runner bug must not lose the other
    entries) never swallow it."""


INTERRUPT = threading.Event()
_live = set()                  # Popen objects of running emulators
_live_lock = threading.Lock()


def check_interrupt():
    if INTERRUPT.is_set():
        raise Interrupted("interrupted (Ctrl+C)")


def interrupt(sig=signal.SIGTERM):
    """No new launches, and `sig` to every running emulator's process group."""
    INTERRUPT.set()
    with _live_lock:
        procs = list(_live)
    for p in procs:
        try:
            os.killpg(p.pid, sig)
        except (ProcessLookupError, PermissionError):
            pass


def stop_all(timeout=5.0):
    """After Ctrl+C: give the emulators `timeout` s to exit, then SIGKILL
    whatever is still registered (an interrupt that landed inside a stop)."""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        with _live_lock:
            procs = [p for p in _live if p.poll() is None]
        if not procs:
            return
        time.sleep(0.1)
    interrupt(signal.SIGKILL)


def run_parallel(fn, items, jobs):
    """fn(item) for every item, `jobs` at a time. On Ctrl+C (or Interrupted
    from a worker) the queued items are cancelled and the running emulators
    stopped, instead of waiting for every running item to finish."""
    from concurrent.futures import ThreadPoolExecutor
    ex = ThreadPoolExecutor(max_workers=jobs)
    try:
        futures = [ex.submit(fn, x) for x in items]
        for f in futures:
            f.result()
    except (KeyboardInterrupt, Interrupted):
        interrupt()
        ex.shutdown(wait=True, cancel_futures=True)
        raise
    except BaseException:
        ex.shutdown(wait=True, cancel_futures=True)
        raise
    ex.shutdown(wait=True)

# ---------------------------------------------------------------------------
# Machine-wide limit on concurrent emulator instances (shared by every
# launcher on this host): tools/amislot.py, flock()ed ~/.cache/amitorture/locks/emu-slot-<n>,
# os.cpu_count() slots (AMT_MAX_EMU overrides). Every Amiberry process the
# runner starts (--version, --dump-config, emulation runs) holds a slot for
# its whole lifetime. If tools/amislot.py cannot be imported, the same scheme
# is used from here (same lock files).
_REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
try:
    import sys as _sys
    _tools = os.path.join(_REPO, "tools")
    if _tools not in _sys.path:
        _sys.path.insert(0, _tools)
    from amislot import emulator_slot as _emulator_slot, max_slots  # noqa: E402
except ImportError as e:  # the shared limiter is mandatory (one machine-wide pool)
    raise ImportError(f"tools/amislot.py not importable from {_tools}: {e}; the runner must use the "
                      "machine-wide emulator slot limiter") from e


def emulator_slot(quiet=True):
    """Context manager: hold one machine-wide emulator slot (blocks until
    free; raises Interrupted if Ctrl+C comes while waiting)."""
    check_interrupt()
    return _emulator_slot(quiet=quiet, cancel=INTERRUPT.is_set, cancel_exc=Interrupted)


def run_amiberry(argv, **kw):
    """subprocess.run for a short-lived Amiberry invocation, inside a slot."""
    with emulator_slot():
        check_interrupt()
        return subprocess.run(argv, **kw)


def find_binary(path=None):
    if path:
        if not (os.path.isfile(path) and os.access(path, os.X_OK)):
            raise RunnerError(f"--amiberry {path}: not an executable file")
        return os.path.abspath(path)
    found = shutil.which("amiberry")
    if not found:
        raise RunnerError("no Amiberry binary: pass --amiberry PATH or put 'amiberry' on PATH "
                          "(amitorture never builds Amiberry)")
    return os.path.abspath(found)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


ENV_KEEP = {"PATH", "LANG", "LANGUAGE", "TZ", "TERM", "USER", "LOGNAME", "SHELL", "TMPDIR", "LD_LIBRARY_PATH"}


def display_env(base=None):
    """--show: the user's own desktop settings, passed through as they are:
    every SDL_* variable except the audio ones (the runner owns audio: dummy,
    or the disk capture), DISPLAY and WAYLAND_DISPLAY. Only two paths are
    adjusted, because HOME and XDG_RUNTIME_DIR stay inside the work dir:
      - a relative WAYLAND_DISPLAY becomes the absolute path of the same
        socket in the real $XDG_RUNTIME_DIR. That dir is not shared: Amiberry
        puts its IPC socket there as amiberry.sock (amiberry_ipc_socket.cpp
        get_socket_path), and a test emulator must never take the name the
        user's own Amiberry (and anything driving it over IPC) uses;
      - XAUTHORITY, when unset, is the real ~/.Xauthority (X11 looks in HOME).
    Raises RunnerError when the environment has no desktop session."""
    src = os.environ if base is None else base
    out = {k: v for k, v in src.items() if k.startswith("SDL_") and "AUDIO" not in k}
    wl = src.get("WAYLAND_DISPLAY")
    if wl:
        rt = src.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}"
        out["WAYLAND_DISPLAY"] = wl if os.path.isabs(wl) else os.path.join(rt, wl)
    if src.get("DISPLAY"):
        out["DISPLAY"] = src["DISPLAY"]
        xa = src.get("XAUTHORITY") or os.path.join(src.get("HOME") or os.path.expanduser("~"), ".Xauthority")
        if os.path.isfile(xa) or src.get("XAUTHORITY"):
            out["XAUTHORITY"] = xa
    if not wl and not src.get("DISPLAY"):
        raise RunnerError("--show: neither WAYLAND_DISPLAY nor DISPLAY is set; run it from a terminal on the desktop")
    return out


def isolated_env(home_root, base=None, tmpdir=None, show=False):
    """Environment that keeps every Amiberry path inside home_root.

    TMPDIR points inside the work dir too (``tmpdir``, default
    home_root/tmp): /tmp on this host is a small tmpfs and nothing the
    emulator writes may land there."""
    dirs = {
        "HOME": home_root,
        "XDG_CONFIG_HOME": os.path.join(home_root, ".config"),
        "XDG_DATA_HOME": os.path.join(home_root, ".local", "share"),
        "XDG_CACHE_HOME": os.path.join(home_root, ".cache"),
        "XDG_STATE_HOME": os.path.join(home_root, ".local", "state"),
        "XDG_RUNTIME_DIR": os.path.join(home_root, "run"),
        "AMIBERRY_HOME_DIR": os.path.join(home_root, "Amiberry"),
    }
    dirs["TMPDIR"] = tmpdir or os.path.join(home_root, "tmp")
    for d in dirs.values():
        os.makedirs(d, exist_ok=True)
    os.chmod(dirs["XDG_RUNTIME_DIR"], 0o700)
    # A clean environment from a whitelist: nothing inherited can point
    # Amiberry or SDL at the user's setup (this host exports e.g.
    # SDL_VIDEO_DRIVER=kmsdrm,wayland,x11, which made Amiberry drive the
    # real console, or fail to start SDL at all).
    src = os.environ if base is None else base
    env = {k: v for k, v in src.items() if k in ENV_KEEP or k.startswith("LC_")}
    env.update(dirs)
    # SDL3 reads SDL_VIDEO_DRIVER / SDL_AUDIO_DRIVER, SDL2 the old
    # spellings: set both.
    env["SDL_VIDEO_DRIVER"] = env["SDL_VIDEODRIVER"] = "offscreen"
    env["SDL_AUDIO_DRIVER"] = env["SDL_AUDIODRIVER"] = "dummy"
    if show:
        for k in ("SDL_VIDEO_DRIVER", "SDL_VIDEODRIVER"):
            env.pop(k)          # the user's own driver setting (if any) comes from display_env
        env.update(display_env(base))
    return env


def binary_info(binary, env):
    info = {"path": binary, "sha256": sha256_file(binary), "size": os.path.getsize(binary),
            "mtime": time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(os.path.getmtime(binary)))}
    try:
        p = run_amiberry([binary, "--version"], env=env, capture_output=True, timeout=30, stdin=subprocess.DEVNULL)
        out = (p.stdout + p.stderr).decode(errors="replace").strip()
        info["version_output"] = out
        info["version"] = out.splitlines()[0] if out else None
    except (OSError, subprocess.TimeoutExpired) as e:
        info["version"] = None
        info["version_error"] = str(e)
    return info


def port_free(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.bind(("127.0.0.1", port))
        return True
    except OSError:
        return False
    finally:
        s.close()


_ports_taken = set()
_ports_lock = threading.Lock()


def pick_port(lo=PORT_RANGE[0], hi=PORT_RANGE[1]):
    """A free port in the range, not handed out before (unless released)."""
    import random
    ports = list(range(lo, hi + 1))
    random.shuffle(ports)
    with _ports_lock:
        for p in ports:
            if p not in _ports_taken and port_free(p):
                _ports_taken.add(p)
                return p
        # all handed out once: reuse those that are free again
        for p in ports:
            if port_free(p):
                return p
    raise RunnerError(f"no free TCP port in {lo}-{hi}")


def release_port(port):
    with _ports_lock:
        _ports_taken.discard(port)


class Process:
    """Amiberry child process with its console output captured (timestamped)."""

    def __init__(self, binary, args, env, log_path, cwd=None):
        self.t0 = time.monotonic()
        self.lines = []          # (t, text)
        self.log = open(log_path, "w", encoding="utf-8", errors="replace")
        self.argv = [binary, *args]
        check_interrupt()
        self.proc = subprocess.Popen(self.argv, env=env, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, start_new_session=True)
        with _live_lock:
            _live.add(self.proc)
        self.lock = threading.Lock()
        self.reader = threading.Thread(target=self._drain, daemon=True)
        self.reader.start()
        self.killed_by_us = False
        self.stop_method = None

    def _drain(self):
        for raw in self.proc.stdout:
            t = time.monotonic() - self.t0
            text = raw.decode(errors="replace").rstrip("\n")
            with self.lock:
                self.lines.append((t, text))
                self.log.write(f"[{t:9.3f}] {text}\n")
                self.log.flush()

    def find(self, pattern):
        rx = re.compile(pattern)
        with self.lock:
            for t, line in self.lines:
                m = rx.search(line)
                if m:
                    return m, t
        return None, None

    def wait_for(self, pattern, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            check_interrupt()
            m, t = self.find(pattern)
            if m:
                return m, t
            if self.proc.poll() is not None:
                time.sleep(0.2)  # let the reader drain
                return self.find(pattern)
            time.sleep(0.05)
        return None, None

    def alive(self):
        return self.proc.poll() is None

    def tail(self, n=60):
        with self.lock:
            return [line for _, line in self.lines[-n:]]

    def exit_info(self):
        rc = self.proc.poll()
        if rc is None:
            return {"running": True}
        d = {"running": False, "returncode": rc}
        if rc < 0:
            try:
                d["signal"] = signal.Signals(-rc).name
            except ValueError:
                d["signal"] = f"signal {-rc}"
        return d

    def stop(self, ipc=None, grace=5.0):
        """QUIT over IPC, then SIGTERM, then SIGKILL. Returns how it ended."""
        if not self.alive():
            self.stop_method = "already exited"
            return self.stop_method
        self.killed_by_us = True
        if ipc is not None:
            try:
                ipc.request("QUIT", timeout=2)
            except OSError:
                pass
            if self._wait(grace):
                self.stop_method = "ipc QUIT"
                return self._done()
        for sig, name, wait in ((signal.SIGTERM, "SIGTERM", 3.0), (signal.SIGKILL, "SIGKILL", 5.0)):
            try:
                os.killpg(self.proc.pid, sig)
            except ProcessLookupError:
                pass
            if self._wait(wait):
                self.stop_method = name
                return self._done()
        self.stop_method = "could not stop"
        return self._done()

    def _wait(self, t):
        try:
            self.proc.wait(timeout=t)
            return True
        except subprocess.TimeoutExpired:
            return False

    def _done(self):
        self.reader.join(timeout=2)
        try:
            # Anything else left in the process group (helpers) goes too.
            os.killpg(self.proc.pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass
        with _live_lock:
            _live.discard(self.proc)
        self.log.close()
        return self.stop_method


class IPC:
    """Client for Amiberry's control socket. Also a memory reader for mailbox.py."""

    BATCH = 2048

    def __init__(self, endpoint, timeout=15.0):
        self.endpoint = endpoint
        self.sock = None
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                s.connect(endpoint)
                s.settimeout(10)
                self.sock = s
                break
            except OSError as e:
                last = e
                s.close()
                time.sleep(0.05)
        if self.sock is None:
            raise OSError(f"cannot connect to IPC {endpoint}: {last}")
        self.buf = b""
        self.requests = 0
        self.lock = threading.Lock()

    def _readlines(self, n, timeout):
        self.sock.settimeout(timeout)
        while self.buf.count(b"\n") < n:
            chunk = self.sock.recv(1 << 16)
            if not chunk:
                raise OSError("IPC connection closed")
            self.buf += chunk
        lines = self.buf.split(b"\n")
        out, self.buf = lines[:n], b"\n".join(lines[n:])
        return [x.decode(errors="replace") for x in out]

    def pipeline(self, lines, timeout=15):
        with self.lock:
            try:
                self.sock.sendall("".join(line + "\n" for line in lines).encode())
                self.requests += len(lines)
                return self._readlines(len(lines), timeout)
            except OSError:
                self.close()
                self.buf = b""
                raise

    def request(self, line, timeout=15):
        return self.pipeline([line], timeout)[0]

    def read_longs(self, addrs):
        out = []
        for i in range(0, len(addrs), self.BATCH):
            chunk = addrs[i:i + self.BATCH]
            replies = self.pipeline([f"READ_MEM\t{a:#x}\t4" for a in chunk])
            for r in replies:
                if r.startswith("OK\t"):
                    try:
                        out.append(int(r[3:]) & 0xffffffff)
                        continue
                    except ValueError:
                        pass
                out.append(None)
        return out

    def write_long(self, addr, value):
        return self.request(f"WRITE_MEM\t{addr:#x}\t4\t{value & 0xffffffff:#x}")

    def write_bytes(self, addr, data):
        """Writes data (length multiple of 4) at addr, pipelined."""
        reqs = []
        for i in range(0, len(data), 4):
            v = int.from_bytes(data[i:i + 4].ljust(4, b"\0"), "big")
            reqs.append(f"WRITE_MEM\t{addr + i:#x}\t4\t{v:#x}")
        bad = []
        for i in range(0, len(reqs), self.BATCH):
            bad += [r for r in self.pipeline(reqs[i:i + self.BATCH]) if r != "OK"]
        return bad

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


class SerialClient:
    """Connects to Amiberry's serial TCP listener and records the byte stream.

    With ``serial_port=TCP://127.0.0.1:<port>/wait`` Amiberry blocks at
    startup until a client connects, so no byte is lost: we connect as soon
    as the port listens.
    """

    def __init__(self, port, raw_path, on_data=None):
        self.port = port
        self.raw = open(raw_path, "wb")
        self.on_data = on_data
        self.stop_flag = threading.Event()
        self.connected_at = None
        self.connect_attempts = 0
        self.bytes = 0
        self.last_data = None
        self.closed_by_peer = False
        self.error = None
        self.t0 = time.monotonic()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self):
        s = None
        while not self.stop_flag.is_set():
            try:
                self.connect_attempts += 1
                s = socket.create_connection(("127.0.0.1", self.port), timeout=0.5)
                break
            except OSError:
                s = None
                time.sleep(0.05)
        if s is None:
            return
        self.connected_at = time.monotonic() - self.t0
        s.settimeout(0.2)
        try:
            while not self.stop_flag.is_set():
                try:
                    data = s.recv(65536)
                except socket.timeout:
                    continue
                if not data:
                    self.closed_by_peer = True
                    break
                self.bytes += len(data)
                self.last_data = time.monotonic()
                self.raw.write(data)
                self.raw.flush()
                if self.on_data:
                    self.on_data(data)
        except OSError as e:
            self.error = str(e)
        finally:
            s.close()

    def stop(self):
        self.stop_flag.set()
        self.thread.join(timeout=3)
        self.raw.close()
