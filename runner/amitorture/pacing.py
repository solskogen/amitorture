"""Pacing per module: which modules of an entry run warped.

A module that measures only emulated time (chipset cycles, CIA ticks,
frames, instruction results) gives the same results warped as paced, and a
warped emulator runs those frames faster. Measured on the Pi 5 (2026-11-10,
8 machines, everything paced vs everything warped, results/20261110-170557Z
and -172107Z): on 68000/68010 and cycle-exact machines warp roughly halves
every module (cpu_basic 80 -> 45 s, display 85 -> 45 s, devio 47 -> 27 s);
on faster cores it mostly helps display (96 -> 75 s, JIT 97 -> 52 s), and
the cpu modules under the JIT get slower. Warp changed case results only in
modules that measure host time (misc_hw RTC, input's IPC key events) and on
the threaded CPU (interrupts, cia, timing, paula), so:

  * only WARP_SAFE modules are ever warped;
  * a family (reference and twins) with a threaded member stays paced, and
    the policy otherwise depends only on the CPU model and core, which twins
    share, so engine parity compares runs with the same pacing;
  * slow machines warp every safe module, the others only display.

The guest asks before each module (a ``pacing`` request with the module's
name, run-parameter flag ``modpace``, CONTRACT §13); the runner sets warp and
acks. Without the flag (real hardware, --module-warp off) nothing changes.
"""

# Modules whose results depend on emulated time only (no host clock, no
# host input or audio, no real floppy speed). Not: misc_hw (RTC against the
# host clock), input (IPC key/mouse events), hostspeed (host time is what it
# measures), paula and dmacontention (real floppy speed, audio capture),
# expsound (audio capture), network (external ARP), timing (H-counter
# polls depend on how often the CPU gets to run, warp changes that on fast
# cores), interrupts and cia (latency against host scheduling on threaded
# cores; kept paced until a warped A/B on more machines says otherwise).
WARP_SAFE = frozenset((
    "takeover", "rt", "cpu_basic", "flags", "exceptions", "smc", "blitter", "display", "copper",
    "mmu", "dispatch", "fpu", "rtg", "devio", "memory", "memory_os", "misc_hw_os",
))
# Modules worth warping on a fast core (measured: display only).
FAST_WARP = frozenset(("display",))


def _true(v):
    return str(v).strip().lower() in ("true", "yes", "1")


def slow(entry):
    o = entry.get("options") or {}
    return (str(o.get("cpu_model")) in ("68000", "68010") or _true(o.get("cycle_exact"))
            or _true(o.get("cpu_cycle_exact")))


def _threaded(entry):
    return _true((entry.get("options") or {}).get("cpu_threaded"))


def families(entries):
    """{entry id: family root id}: a twin belongs to its reference's family."""
    by_id = {e["id"]: e for e in entries}

    def root(eid, seen=()):
        ref = by_id.get(eid, {}).get("twin_of")
        if ref in by_id and ref not in seen:
            return root(ref, seen + (eid,))
        return eid
    return {e["id"]: root(e["id"]) for e in entries}


def warped_modules(entry, family_threaded):
    """The modules this entry runs warped (a frozenset)."""
    if family_threaded or _threaded(entry):
        return frozenset()
    return WARP_SAFE if slow(entry) else FAST_WARP


def assign(entries):
    """Sets entry["module_warp"] (sorted module names) on every entry."""
    fam = families(entries)
    thr = {}
    for e in entries:
        thr[fam[e["id"]]] = thr.get(fam[e["id"]], False) or _threaded(e)
    for e in entries:
        e["module_warp"] = sorted(warped_modules(e, thr[fam[e["id"]]]))
    return entries
