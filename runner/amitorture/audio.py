"""Audio capture through SDL3's disk audio driver, and its host-side analysis.

Amiberry opens its output as an SDL3 audio stream (src/sounddep/sound.cpp:
SDL_AUDIO_S16, sound_frequency, 2 channels for sound_channels=stereo). With
``SDL_AUDIO_DRIVER=disk`` SDL writes the device's mixed output to the file
named by the ``SDL_AUDIO_DISK_OUTPUT_FILE`` hint (SDL3 >= 3.2; hints can be
given as environment variables), in real time, as raw interleaved PCM in the
device format (the format and rate of the first stream, S16LE/44100/stereo
here: checked on the first analysed segment, see ``Capture.check_rate``).

``audio_mark`` requests (CONTRACT §13) become byte offsets in that file: the
file size when the request is served. The guest waits for the ack, so a
start mark comes before the sound it starts; for a stop mark the host waits
``STOP_SETTLE`` seconds first (SDL's buffering delays what reaches the file)
and then takes the offset, so the segment covers the sound. The segment is
then cut out (``audio-<id>-<tag>.wav`` next to the screenshots) and
analysed: per channel RMS, peak, DC, onset and active duration, dominant
frequency by zero crossings and by FFT, and dropouts. With
``"expect_hz"`` (and optional ``"tol_pct"``, default 3) in the stop mark's
args the host also says whether the measured frequency matches.

Everything here is stdlib (no numpy); the FFT is a plain radix-2 one on at
most 16384 samples.
"""
from __future__ import annotations

import cmath
import math
import os
import struct
import time
import wave

RATE = 44100
CHANNELS = 2
STOP_SETTLE = 0.35        # s: let SDL's buffered audio reach the file before a stop mark
FFT_MAX = 16384
MIN_HZ = 20.0             # the FFT peak is searched from here up (see fft_peak_hz)
BLOCK_S = 0.01            # active-part detection: 10 ms blocks of AC level


def env_for(path):
    """Environment variables that send SDL's output to `path`."""
    return {"SDL_AUDIO_DRIVER": "disk", "SDL_AUDIODRIVER": "disk",
            "SDL_AUDIO_DISK_OUTPUT_FILE": path,     # SDL3 hint
            "SDL_DISKAUDIOFILE": path,              # SDL2 spelling
            "SDL_AUDIO_DISK_TIMESCALE": "1.0"}


def wants_audio(adf_path, cache={}):
    """auto mode: does this disk ever ask for audio marks? (The request
    names are C string literals in the uncompressed payload.)"""
    try:
        st = os.stat(adf_path)
    except OSError:
        return False
    key = (adf_path, st.st_mtime_ns, st.st_size)
    if key not in cache:
        with open(adf_path, "rb") as fh:
            data = fh.read()
        cache[key] = b"audio_mark" in data or b"audio_start" in data
    return cache[key]


# ---------------------------------------------------------------------------
# analysis (pure)

def decode_s16(data, channels=CHANNELS):
    n = len(data) // (2 * channels)
    vals = struct.unpack(f"<{n * channels}h", data[:n * 2 * channels])
    return [[v / 32768.0 for v in vals[c::channels]] for c in range(channels)]


def decode_f32(data, channels=CHANNELS):
    n = len(data) // (4 * channels)
    vals = struct.unpack(f"<{n * channels}f", data[:n * 4 * channels])
    return [list(vals[c::channels]) for c in range(channels)]


def fft(x):
    """Radix-2 FFT of a list of complex/float (len a power of two)."""
    n = len(x)
    a = [complex(v) for v in x]
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j |= bit
        if i < j:
            a[i], a[j] = a[j], a[i]
    size = 2
    while size <= n:
        half = size // 2
        w_step = cmath.exp(-2j * math.pi / size)
        for start in range(0, n, size):
            w = 1 + 0j
            for k in range(start, start + half):
                t = w * a[k + half]
                a[k + half] = a[k] - t
                a[k] = a[k] + t
                w *= w_step
        size *= 2
    return a


def fft_peak_hz(x, rate, min_hz=None):
    """Dominant frequency of x by FFT (Hann window, parabolic interpolation
    on the log magnitude), searched from `min_hz` (default MIN_HZ) up: below
    that only level changes live (a step in the held output level puts a
    1/f spectrum there). None for silence or too few samples."""
    n = 1
    while n * 2 <= min(len(x), FFT_MAX):
        n *= 2
    if n < 64:
        return None
    off = (len(x) - n) // 2
    seg = x[off:off + n]
    mean = sum(seg) / n
    win = [(seg[i] - mean) * (0.5 - 0.5 * math.cos(2 * math.pi * i / (n - 1))) for i in range(n)]
    spec = fft(win)
    mags = [abs(c) for c in spec[:n // 2]]
    k0 = max(1, int(math.ceil((MIN_HZ if min_hz is None else min_hz) * n / rate)))
    if k0 >= len(mags):
        return None
    k = max(range(k0, len(mags)), key=lambda i: mags[i])
    if mags[k] <= 1e-9:
        return None
    delta = 0.0
    if 1 <= k < len(mags) - 1:
        a, b, c = (math.log(max(m, 1e-12)) for m in (mags[k - 1], mags[k], mags[k + 1]))
        den = a - 2 * b + c
        if den:
            delta = 0.5 * (a - c) / den
    return (k + delta) * rate / n


def zero_cross_hz(x, rate, hyst):
    """Frequency from positive-going crossings with hysteresis (period
    between the first and the last crossing)."""
    first = last = None
    count = 0
    state = None
    for i, v in enumerate(x):
        if v > hyst:
            if state is False:
                count += 1
                if first is None:
                    first = i
                last = i
            state = True
        elif v < -hyst:
            state = False
    if count < 2 or last == first:
        return None
    return (count - 1) * rate / (last - first)


def analyse_channel(x, rate):
    n = len(x)
    if n == 0:
        return {"samples": 0}
    dc = sum(x) / n
    peak = max(abs(v) for v in x)
    rms = math.sqrt(sum((v - dc) ** 2 for v in x) / n)
    out = {"samples": n, "rms": round(rms, 6), "peak": round(peak, 6), "dc": round(dc, 6),
           "clipped": sum(1 for v in x if abs(v) >= 0.9999)}
    ac_peak = max(abs(v - dc) for v in x)
    thr = max(ac_peak * 0.1, 1.0 / 32768 * 4)
    first = next((i for i, v in enumerate(x) if abs(v - dc) > thr), None)
    if first is None or ac_peak < 1.0 / 32768 * 8:
        out.update({"silent": True, "active_s": 0.0})
        return out
    last = n - 1 - next(i for i, v in enumerate(reversed(x)) if abs(v - dc) > thr)
    # A held output level is not sound: Paula keeps the last sample after its
    # DMA stops, so a segment can end (or start) on a DC step. Trim the head
    # and tail to the 10 ms blocks whose own AC level (block mean removed)
    # reaches 10 % of the loudest block's.
    blk = max(1, int(rate * BLOCK_S))
    acl = []
    for i in range(first, last + 1, blk):
        b = x[i:min(i + blk, last + 1)]
        bm = sum(b) / len(b)
        acl.append(math.sqrt(sum((v - bm) ** 2 for v in b) / len(b)))
    top = max(acl)
    if top < 1.0 / 32768 * 4:
        out.update({"silent": True, "active_s": 0.0})
        return out
    on = [j for j, a in enumerate(acl) if a >= 0.1 * top]
    first, last = first + on[0] * blk, min(last, first + (on[-1] + 1) * blk - 1)
    seg = x[first:last + 1]
    m = sum(seg) / len(seg)
    seg = [v - m for v in seg]
    out["silent"] = False
    out["onset_s"] = round(first / rate, 4)
    out["active_s"] = round((last + 1 - first) / rate, 4)
    out["active_rms"] = round(math.sqrt(sum(v * v for v in seg) / len(seg)), 6)
    zc = zero_cross_hz(seg, rate, max(ac_peak * 0.1, 1e-4))
    fp = fft_peak_hz(seg, rate)
    out["freq_zc_hz"] = None if zc is None else round(zc, 2)
    out["freq_fft_hz"] = None if fp is None else round(fp, 2)
    # dropouts: 10 ms blocks inside the active part with < 5 % of its RMS
    blk = max(1, rate // 100)
    blocks = [seg[i:i + blk] for i in range(0, len(seg) - blk + 1, blk)]
    if blocks and out["active_rms"] > 0:
        quiet = sum(1 for b in blocks if math.sqrt(sum(v * v for v in b) / len(b)) < 0.05 * out["active_rms"])
        out["dropout_fraction"] = round(quiet / len(blocks), 4)
    return out


def analyse(channels, rate=RATE, expect_hz=None, tol_pct=3.0):
    """channels: [[float samples -1..1], ...] -> analysis dict."""
    res = {"rate": rate, "frames": len(channels[0]) if channels else 0,
           "seconds": round((len(channels[0]) if channels else 0) / rate, 4),
           "channels": {}}
    names = ["L", "R"] if len(channels) == 2 else [str(i) for i in range(len(channels))]
    for name, x in zip(names, channels):
        res["channels"][name] = analyse_channel(x, rate)
    loud = [(c.get("active_rms") or 0, k) for k, c in res["channels"].items() if not c.get("silent", True)]
    if loud:
        best = max(loud)[1]
        c = res["channels"][best]
        res["dominant_hz"] = c.get("freq_fft_hz") or c.get("freq_zc_hz")
        res["loudest"] = best
    else:
        res["dominant_hz"] = None
        res["silent"] = True
    if expect_hz:
        try:
            e = float(expect_hz)
            tol = float(tol_pct or 3.0)
            got = res.get("dominant_hz")
            res["expect_hz"] = e
            res["tol_pct"] = tol
            res["freq_ok"] = bool(got) and abs(got - e) <= e * tol / 100.0
        except (TypeError, ValueError):
            res["expect_error"] = f"bad expect_hz {expect_hz!r}"
    return res


def expected_hz(args):
    """The frequency a mark's args promise: expect_hz, or freq_mhz / 1000,
    or clock / (period * samples) (Paula: one sample per period clocks)."""
    if not args:
        return None
    try:
        if args.get("expect_hz"):
            return float(args["expect_hz"])
        if args.get("freq_mhz"):
            return float(args["freq_mhz"]) / 1000.0
        if args.get("clock") and args.get("period") and args.get("samples"):
            return float(args["clock"]) / (float(args["period"]) * float(args["samples"]))
    except (TypeError, ValueError, ZeroDivisionError):
        return None
    return None


def merged_args(start_args, stop_args):
    """The stop mark's args, with the expectation taken from the start mark
    when the stop does not give one (a guest may describe the tone once)."""
    a = dict(start_args or {})
    a.update(stop_args or {})
    e = expected_hz(stop_args) or expected_hz(start_args)
    if e:
        a["expect_hz"] = e
    return a


def goertzel_amp(x, rate, freq):
    """Amplitude of the `freq` component of x (Goertzel, whole segment)."""
    n = len(x)
    if n == 0 or not freq:
        return 0.0
    w = 2 * math.pi * freq / rate
    c = 2 * math.cos(w)
    s1 = s2 = 0.0
    for v in x:
        s0 = v + c * s1 - s2
        s2, s1 = s1, s0
    power = s1 * s1 + s2 * s2 - c * s1 * s2
    return 2 * math.sqrt(max(power, 0.0)) / n


def side_check(res, side, channels=None):
    """side: "L"/"R"/"left"/"right": which channel must carry the tone. With
    the samples and an expected frequency, the comparison is of the tone
    itself (Goertzel at expect_hz): a DC step or a click left on the other
    channel by an earlier tone does not count; otherwise of the RMS."""
    s = str(side or "").lower()
    if s == "both":
        _side_both(res, channels)
        return
    want = {"l": "L", "left": "L", "r": "R", "right": "R"}.get(s)
    if want is None or res.get("silent"):
        return
    other = "R" if want == "L" else "L"
    res["expect_side"] = want
    names = list(res["channels"])
    if channels is not None and res.get("expect_hz") and want in names and other in names:
        a = goertzel_amp(channels[names.index(want)], res["rate"], res["expect_hz"])
        b = goertzel_amp(channels[names.index(other)], res["rate"], res["expect_hz"])
        res["side_amp"] = {want: round(a, 6), other: round(b, 6)}
    else:
        a = (res["channels"].get(want) or {}).get("active_rms") or 0.0
        b = (res["channels"].get(other) or {}).get("active_rms") or 0.0
    res["side_ok"] = a > 0 and b < 0.1 * a


def _side_both(res, channels):
    """side "both": a mono source sent to left and right. The tone must be on
    both outputs, the quieter at least half the louder (Goertzel at expect_hz
    when the samples are there, else the RMS)."""
    if res.get("silent"):
        return
    res["expect_side"] = "both"
    names = list(res["channels"])
    if "L" not in names or "R" not in names:
        return
    if channels is not None and res.get("expect_hz"):
        a = goertzel_amp(channels[names.index("L")], res["rate"], res["expect_hz"])
        b = goertzel_amp(channels[names.index("R")], res["rate"], res["expect_hz"])
        res["side_amp"] = {"L": round(a, 6), "R": round(b, 6)}
    else:
        a = (res["channels"]["L"] or {}).get("active_rms") or 0.0
        b = (res["channels"]["R"] or {}).get("active_rms") or 0.0
    res["side_ok"] = min(a, b) > 0 and min(a, b) >= 0.5 * max(a, b)


def write_wav(path, channels, rate):
    n = len(channels[0]) if channels else 0
    with wave.open(path, "wb") as w:
        w.setnchannels(len(channels))
        w.setsampwidth(2)
        w.setframerate(rate)
        frames = bytearray()
        for i in range(n):
            for ch in channels:
                v = max(-32768, min(32767, int(round(ch[i] * 32768))))
                frames += struct.pack("<h", v)
        w.writeframes(bytes(frames))


# ---------------------------------------------------------------------------

class Capture:
    """One launch's audio stream file and its marks."""

    def __init__(self, path, rawdir, rel="", rate=RATE, channels=CHANNELS, fmt="s16", clock=time.monotonic,
                 sleep=time.sleep, settle=STOP_SETTLE):
        self.path, self.rawdir, self.rel = path, rawdir, rel
        self.rate, self.channels, self.fmt = rate, channels, fmt
        self.clock, self.sleep, self.settle = clock, sleep, settle
        self.t0 = clock()
        self.open = []        # [{"tag", "id", "offset", "t"}]
        self.segments = []
        self.first_size_t = None

    @property
    def frame_bytes(self):
        return self.channels * (2 if self.fmt == "s16" else 4)

    def size(self):
        try:
            return os.path.getsize(self.path)
        except OSError:
            return 0

    def offset(self):
        s = self.size()
        return s - s % self.frame_bytes

    def info(self):
        s = self.size()
        el = self.clock() - self.t0
        return {"file": self.path, "bytes": s, "format": f"{self.fmt}le", "rate": self.rate,
                "channels": self.channels, "seconds_written": round(s / (self.frame_bytes * self.rate), 3),
                "seconds_elapsed": round(el, 3), "segments": len(self.segments), "open_marks": len(self.open)}

    def mark(self, edge, tag, rid, args=None):
        args = args or {}
        t = round(self.clock() - self.t0, 3)
        if edge == "start":
            off = self.offset()
            self.open.append({"tag": tag, "id": rid, "offset": off, "t": t, "args": dict(args)})
            return {"edge": "start", "offset": off, "t_s": t, "stream_s": round(off / (self.frame_bytes * self.rate), 4)}
        if self.settle:
            self.sleep(self.settle)
        off = self.offset()
        start = None
        for i in range(len(self.open) - 1, -1, -1):
            if self.open[i]["tag"] == tag:
                start = self.open.pop(i)
                break
        if start is None and self.open:
            start = self.open.pop()
        res = {"edge": "stop", "offset": off, "t_s": t, "stream_s": round(off / (self.frame_bytes * self.rate), 4),
               "settle_s": self.settle}
        if start is None:
            res["error"] = "stop mark without a start mark"
            return res
        res["start"] = {"id": start["id"], "offset": start["offset"], "t_s": start["t"]}
        res["bytes"] = off - start["offset"]
        res["analysis"] = self.analyse_range(start["offset"], off, rid, tag, merged_args(start.get("args"), args))
        if res["analysis"].get("wav"):
            res["wav"] = res["analysis"].pop("wav")
        self.segments.append({"tag": tag, "id": rid, "start": start["offset"], "stop": off})
        return res

    def read(self, a, b):
        with open(self.path, "rb") as fh:
            fh.seek(a)
            return fh.read(max(0, b - a))

    def analyse_range(self, a, b, rid, tag, args):
        data = self.read(a, b)
        if not data:
            return {"error": "empty segment (no audio reached the file between the marks)", "silent": True}
        chans = decode_s16(data, self.channels) if self.fmt == "s16" else decode_f32(data, self.channels)
        res = analyse(chans, self.rate, args.get("expect_hz"), args.get("tol_pct", 3.0))
        side_check(res, args.get("side"), chans)
        # what the guest says it played, for the host.tone_<tag> pseudo case
        # (cases.tone_case): how long, and from which of its channels
        try:
            if args.get("ms"):
                res["expect_ms"] = float(args["ms"])
        except (TypeError, ValueError):
            pass
        if args.get("ch") is not None:
            res["source_ch"] = args["ch"]
        from .hostreq import _safe_tag
        name = f"audio-{rid}-{_safe_tag(tag)}.wav"
        try:
            write_wav(os.path.join(self.rawdir, name), chans, self.rate)
            res["wav"] = self.rel + name
        except OSError as e:
            res["wav_error"] = str(e)
        return res
