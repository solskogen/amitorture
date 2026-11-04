"""Host side of the request channel (CONTRACT.md §13).

The guest emits ``{"t":"request","id":N,"what":...,"tag":...,"args":{...}}``
(on serial and in the mailbox ring) and then polls the mailbox's
``host_ack`` field (offset 44) until it reads N. The runner, while watching
the run, picks request records up from either transport (whichever delivers
first; the other copy is a duplicate), acts on them, writes N to
mailbox+44 with IPC ``WRITE_MEM <addr> 4 <value>`` (Amiberry put_long:
big-endian, as the guest reads it), and keeps a ``host`` record

    {"t":"host","id":N,"what":"...","tag":"...","acked":true|false,"result":{...}}

that is merged into the entry's records right after the request.

Requests are only honoured once the mailbox address is known (the ack goes
there); until then they wait in ``pending`` and the watcher scans for the
mailbox at once. Unknown ``what`` values are never acknowledged (a host
record with ``acked: false`` says why), and neither is anything the host
could not do (no joystick injection in Amiberry's IPC, a failed
screenshot, no audio capture in this launch): the guest then reports the
dependent case ``untestable``.

The old v1 ``capture`` records (best effort, no ack) are served too:
``screen`` takes a screenshot, ``audio_start``/``audio_stop`` set audio marks.

The IPC object only needs ``request(line) -> reply`` (emulator.IPC), so the
logic is unit-tested with a fake.
"""
from __future__ import annotations

import hashlib
import os
import re
import struct
import time

ACK_OFFSET = 44
KNOWN = ("screenshot", "audio_mark", "key", "mouse", "joy", "cd32pad", "wait")

NO_JOY = ("Amiberry's IPC (src/osdep/amiberry_ipc_socket.cpp) has no joystick/CD32 pad injection: only "
          "SEND_KEY (Amiga keyboard), SEND_MOUSE (mouse device 0) and SET_JOYPORT_MODE")


def _safe_tag(tag):
    t = re.sub(r"[^A-Za-z0-9_.-]+", "_", str(tag or ""))[:40].strip("._")
    return t or "x"


def png_size(path):
    """(width, height) from a PNG's IHDR, or None."""
    try:
        with open(path, "rb") as fh:
            head = fh.read(24)
    except OSError:
        return None
    if len(head) < 24 or head[:8] != b"\x89PNG\r\n\x1a\n" or head[12:16] != b"IHDR":
        return None
    return struct.unpack(">II", head[16:24])


def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _int(v, default=0):
    try:
        return int(v)
    except (TypeError, ValueError):
        return default


class Requests:
    """Collects request records, acts on them, acks them, keeps host records.

    ipc         emulator.IPC (or a fake with .request(line))
    rawdir      where screenshots/audio segments go (raw/<entry>/<disk>/)
    rel         the same, relative to the results dir (for the paths in results)
    options     the entry's Amiberry options (joyport mapping for mouse)
    audio       audio.Capture or None (no capture in this launch)
    frame_hz    50 (PAL) / 60 (NTSC): screenshot {"frames": n} waits n frames
    """

    def __init__(self, ipc, rawdir, rel="", options=None, audio=None, frame_hz=50, clock=time.monotonic,
                 sleep=time.sleep, t0=None, log=None):
        self.ipc, self.rawdir, self.rel = ipc, rawdir, rel
        self.options = options or {}
        self.audio = audio
        self.frame_hz = frame_hz or 50
        self.clock, self.sleep = clock, sleep
        self.t0 = clock() if t0 is None else t0
        self.log = log
        self.seen = set()          # (seq, id) of requests taken (either transport)
        self.pending = []          # [(obj, source, t_seen)]
        self.host = []             # host records, in the order they were made
        self.stats = {"requests": 0, "acked": 0, "not_acked": 0, "captures": 0, "duplicates": 0}

    # -- intake ------------------------------------------------------------
    def offer(self, obj, source="?"):
        """A parsed, CRC-valid record from a transport. Takes request and
        capture records, ignores everything else. Returns True when new."""
        t = obj.get("t")
        if t not in ("request", "capture"):
            return False
        key = (t, obj.get("seq"), obj.get("id"))
        if key in self.seen:
            self.stats["duplicates"] += 1
            return False
        self.seen.add(key)
        self.pending.append((obj, source, self.clock()))
        return True

    def new_boot(self):
        """The guest rebooted (seq restarted): ids and seqs start again."""
        self.seen.clear()

    def waiting(self):
        return bool(self.pending)

    # -- service -----------------------------------------------------------
    def service(self, mbox_addr):
        """Acts on the pending requests. Requests (not captures) wait while
        the mailbox is unknown. Returns the host records made."""
        made, keep = [], []
        for obj, source, t_seen in self.pending:
            if obj.get("t") == "request" and mbox_addr is None:
                keep.append((obj, source, t_seen))
                continue
            h = self._handle(obj, source, t_seen, mbox_addr)
            self.host.append(h)
            made.append(h)
        self.pending = keep
        return made

    def _now(self):
        return round(self.clock() - self.t0, 3)

    def _handle(self, obj, source, t_seen, mbox_addr):
        is_req = obj.get("t") == "request"
        what = obj.get("what") if is_req else {"screen": "screenshot", "audio_start": "audio_mark",
                                                 "audio_stop": "audio_mark"}.get(obj.get("what"), obj.get("what"))
        args = obj.get("args") if isinstance(obj.get("args"), dict) else {}
        if not is_req and obj.get("what") in ("audio_start", "audio_stop"):
            args = {"edge": obj["what"][6:]}
        rid = obj.get("id") if is_req else None
        tag = obj.get("tag") or ""
        t_start = self.clock()
        acked, result, ipc_trouble = False, {}, False
        if is_req:
            self.stats["requests"] += 1
        else:
            self.stats["captures"] += 1
        try:
            fn = getattr(self, f"_do_{what}", None) if what in KNOWN else None
            if fn is None:
                ok, result = False, {"error": f"unknown request {what!r}: never acknowledged (CONTRACT §13)"}
            else:
                ok, result = fn(obj, args, rid, tag)
        except OSError as e:     # IPC trouble: no ack, the guest times out
            ok, result, ipc_trouble = False, {"error": f"{type(e).__name__}: {e}"}, True
        if ok and is_req:
            reply = self._ack(mbox_addr, rid)
            acked = reply == "OK"
            if not acked:
                result["ack_error"] = reply
        elif is_req and not ipc_trouble and rid is not None:
            # cannot serve it (unknown request, no joystick injection, ...):
            # refuse, so the guest stops waiting at once (CONTRACT §13)
            reply = self._ack(mbox_addr, int(rid) | 0x80000000)
            result["refused"] = reply == "OK"
        elif not is_req:
            acked = None          # a capture record wants no ack
        if is_req:
            self.stats["acked" if acked else "not_acked"] += 1
        h = {"t": "host", "id": rid, "what": what, "tag": tag, "acked": acked, "result": result,
             "req_seq": obj.get("seq"), "req_t": obj.get("t"), "source": source,
             "t_seen": round(t_seen - self.t0, 3), "t_done": self._now(),
             "latency_s": round(self.clock() - t_start, 3)}
        if not is_req:
            h["capture_what"] = obj.get("what")
        if self.log:
            self.log(f"host request {rid} {what} {tag!r}: {'acked' if acked else ('done' if acked is None else 'NOT acked')}"
                     + (f" ({result.get('error')})" if result.get("error") else ""))
        return h

    def _ack(self, mbox_addr, rid):
        try:
            return self.ipc.request(f"WRITE_MEM\t{mbox_addr + ACK_OFFSET:#x}\t4\t{int(rid) & 0xffffffff:#x}")
        except (OSError, TypeError, ValueError) as e:
            return f"ERROR\t{type(e).__name__}: {e}"

    # -- actions: each returns (ok, result) -----------------------------------
    def _do_wait(self, obj, args, rid, tag):
        return True, {}

    def _do_screenshot(self, obj, args, rid, tag):
        frames = max(0, min(_int(args.get("frames")), 50 * 60))
        if frames:
            self.sleep(frames / self.frame_hz)
        name = f"shot-{rid}-{_safe_tag(tag)}.png" if rid is not None else f"shot-c{obj.get('seq')}-{_safe_tag(tag)}.png"
        path = os.path.join(self.rawdir, name)
        reply = self.ipc.request(f"SCREENSHOT\t{path}", timeout=30)
        if not reply.startswith("OK"):
            return False, {"error": f"IPC SCREENSHOT: {reply}"}
        if not os.path.isfile(path):
            return False, {"error": f"IPC SCREENSHOT said {reply!r} but {path} does not exist"}
        res = {"path": self.rel + name, "sha256": _sha256(path), "size": os.path.getsize(path)}
        wh = png_size(path)
        if wh:
            res["width"], res["height"] = wh
        if frames:
            res["waited_frames"] = frames
        return True, res

    def _do_audio_mark(self, obj, args, rid, tag):
        if self.audio is None:
            return False, {"error": "no audio capture in this launch (SDL disk audio driver off: see --audio)"}
        edge = args.get("edge")
        if edge not in ("start", "stop"):
            return False, {"error": f"audio_mark edge {edge!r}: must be start or stop"}
        return True, self.audio.mark(edge, tag, rid, args)

    def _do_key(self, obj, args, rid, tag):
        code = _int(args.get("code"), -1)
        if not 0 <= code <= 0x7f:
            return False, {"error": f"key code {args.get('code')!r} is not an Amiga raw key code 0..0x7f"}
        up = 1 if _int(args.get("up")) else 0
        reply = self.ipc.request(f"SEND_KEY\t{code}\t{0 if up else 1}")
        if not reply.startswith("OK"):
            return False, {"error": f"IPC SEND_KEY: {reply}"}
        return True, {"injected": {"key": code, "up": up}, "ipc": f"SEND_KEY {code} {0 if up else 1}"}

    def _do_mouse(self, obj, args, rid, tag):
        port = _int(args.get("port"), 0)
        # SEND_MOUSE moves mouse device 0 (setmousestate(0, ...)); it reaches
        # the guest only on the joyport that has the mouse attached.
        jp = self.options.get(f"joyport{port}", "mouse" if port == 0 else "none")
        if jp != "mouse":
            return False, {"error": f"mouse on port {port}: joyport{port}={jp} in this config; SEND_MOUSE drives "
                                    "the host mouse device, attached only where joyportN=mouse"}
        dx, dy, b = _int(args.get("dx")), _int(args.get("dy")), _int(args.get("buttons"))
        reply = self.ipc.request(f"SEND_MOUSE\t{dx}\t{dy}\t{b}")
        if not reply.startswith("OK"):
            return False, {"error": f"IPC SEND_MOUSE: {reply}"}
        return True, {"injected": {"dx": dx, "dy": dy, "buttons": b, "port": port}}

    def _do_joy(self, obj, args, rid, tag):
        return False, {"error": NO_JOY}

    def _do_cd32pad(self, obj, args, rid, tag):
        # Nothing can be pressed (no pad injection), but "no buttons" is a
        # state the host can vouch for: the pad on that port is a cd32joy
        # fed by keyboard layout kbd1, and no host key is pressed.
        port = _int(args.get("port"), 1)
        mode = self.options.get(f"joyport{port}mode", "")
        if _int(args.get("buttons"), -1) == 0 and mode == "cd32joy":
            return True, {"injected": None, "buttons": 0, "port": port,
                          "note": f"joyport{port}={self.options.get(f'joyport{port}')} mode cd32joy, no host input: "
                                  "every button reads released"}
        if mode != "cd32joy":
            return False, {"error": f"no CD32 pad on port {port} (joyport{port}mode={mode or 'default'}); " + NO_JOY}
        return False, {"error": NO_JOY}

    # -- results -------------------------------------------------------------
    def summary(self):
        by_what = {}
        for h in self.host:
            w = by_what.setdefault(h["what"] or "?", {"n": 0, "acked": 0})
            w["n"] += 1
            w["acked"] += 1 if h["acked"] else 0
        return dict(self.stats, by_what=by_what, unserved=len(self.pending))


def attach(merged, host_records):
    """Puts the host records into a records.Merged, each right after the
    request it answers (matched by seq and id), marked ``host: True`` and
    sourced "host". Returns the number attached. Guest record counts
    (done.records, gaps) must skip entries with ``host`` set."""
    if not host_records:
        return 0
    recs = merged.records
    for h in host_records:
        idx = None
        for i in range(len(recs) - 1, -1, -1):
            r = recs[i]["rec"]
            if not recs[i].get("host") and r.get("t") == h.get("req_t") and r.get("seq") == h.get("req_seq") \
                    and r.get("id") == h.get("id"):
                idx = i
                break
        boot = recs[idx]["boot"] if idx is not None else (recs[-1]["boot"] if recs else 0)
        item = {"boot": boot, "seq": h.get("req_seq"), "rec": h, "sources": ["host"], "host": True}
        if idx is None:
            recs.append(item)
        else:
            j = idx + 1
            while j < len(recs) and recs[j].get("host"):
                j += 1
            recs.insert(j, item)
    return len(host_records)


def guest_records(merged_records):
    """The merged records without the host's own (for integrity counts)."""
    return [m for m in merged_records if not m.get("host")]
