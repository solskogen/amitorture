"""Coverage keys: what a matrix checks, independent of how the runs are laid out.

An old-style run boots every matrix entry with every disk and runs every
module (minus the entry's module mask). Most modules depend on only a few
configuration factors: cpu_basic on the CPU, the core and the engine, display
on the chipset, the video standard and the core, and so on. A module's
results on two entries that agree on its factors are the same check twice.

Each module declares its axes (AXES): the option groups and entry fields its
results may depend on. A coverage key is (module, the entry projected on
those axes). The keys of a matrix are those of every (entry, module) an
old-style run would run (``required``). A plan (plan.py) may lay the runs
out differently, but must run every key at least once; a unit test checks it
for every matrix. Whether the axes are wide enough is checked against
old-style results (``validate``): two entries with the same key must give the
same fingerprints for that module.

Axes err wide: a factor that may matter is an axis. Factors named by xfail
rules (``when`` options, ``configs`` globs) and by findings are axes too.
"""
import fnmatch
import functools

from . import matrix

# Option groups, by option name. Every option a matrix entry sets must belong
# to one (a unit test checks it), so a new option forces a decision.
OPTION_GROUPS = {
    "cpu": ("cpu_model", "mmu_model", "cpu060_revision", "cpu_24bit_addressing", "cpu_no_unimplemented"),
    "fpu": ("fpu_model", "fpu_softfloat", "fpu_strict", "fpu_no_unimplemented", "compfpu"),
    "core": ("cpu_compatible", "cycle_exact", "cpu_cycle_exact", "cpu_memory_cycle_exact", "blitter_cycle_exact",
             "cpu_speed", "cpu_multiplier"),
    "engine_class": ("cachesize", "cpu_threaded"),
    "engine": ("cachesize", "cpu_threaded", "comp_*", "compfpu"),
    "chipset": ("chipset", "chipset_compatible"),
    "video": ("ntsc",),
    "memory": ("*mem_size", "cpuboardmem*_size"),
    "boards": ("*_rom_file", "gfxcard_*", "a2065", "ne2000_*", "zz9000_net", "rtc", "cpuboard_type", "ppc_*"),
    "input": ("joyport*",),
    "model": ("flash_file",),
}
# Entry fields (not options) that are factors, by group.
FIELD_GROUPS = {
    "model": ("model", "rom", "ext_rom"),
    "boot": ("boot", "media", "media_files"),
    "flags": ("flags",),
    "boards": ("board_roms",),
}

# The groups each module's results may depend on (docs/cases/<module>.md, the
# module sources, the xfail rules). engine_class: JIT on/off and the CPU
# thread; engine: also every JIT sub-option.
AXES = {
    "takeover": ("model", "cpu", "core", "engine_class"),
    "rt": ("cpu", "core", "engine_class"),
    "cpu_basic": ("cpu", "core", "engine"),
    "flags": ("cpu", "core", "engine"),
    "exceptions": ("cpu", "fpu", "core", "engine", "flags"),
    "smc": ("cpu", "core", "engine", "memory"),
    "interrupts": ("video", "chipset", "model", "cpu", "core", "engine_class", "boards", "boot"),
    "cia": ("video", "chipset", "model", "cpu", "core", "engine_class", "boards", "boot"),
    "blitter": ("chipset", "cpu", "core", "engine_class"),
    "fpu": ("cpu", "fpu", "core", "engine"),
    "timing": ("video", "chipset", "cpu", "fpu", "core", "engine_class"),
    "devio": ("model", "boot", "memory", "boards", "cpu", "flags"),
    "display": ("chipset", "video", "model", "cpu", "core", "engine_class"),
    "copper": ("chipset", "video", "cpu", "core", "engine_class"),
    "paula": ("chipset", "video", "model", "boot", "cpu", "core", "engine_class", "boards"),
    "input": ("input", "model", "video", "cpu", "core", "engine_class"),
    "rtg": ("boards", "model", "cpu", "memory"),
    "expsound": ("boards", "model", "cpu", "core", "video", "engine_class", "flags"),
    "network": ("boards", "model", "flags"),
    "misc_hw": ("model", "boards", "chipset", "memory", "flags", "boot"),
    "mmu": ("cpu", "core", "model", "memory"),
    "memory": ("memory", "model", "cpu", "engine", "boards"),
    "dispatch": ("cpu", "core", "engine"),
    "dmacontention": ("chipset", "video", "cpu", "core", "boot", "engine_class"),
    "misc_hw_os": ("model", "boards", "memory"),
    "memory_os": ("memory", "model", "boards"),
    "hostspeed": ("cpu", "fpu", "core", "engine", "chipset", "video"),
}

# The disk group each module is built into (disk/Makefile DISK_SET_*; a unit
# test keeps this in step).
MODULE_GROUP = {}
for _g, _mods in (("core", "takeover rt flags exceptions smc interrupts cia blitter timing display copper paula "
                           "input dmacontention"),
                  ("cpu", "cpu_basic mmu dispatch"),
                  ("exp", "fpu rtg expsound network misc_hw misc_hw_os devio memory memory_os hostspeed")):
    for _m in _mods.split():
        MODULE_GROUP[_m] = _g
GROUPS = ("core", "cpu", "exp")

# Modules that must run in the same launch as another one: memory tests the
# blocks memory_os allocated.
COMPANIONS = {"memory": ("memory_os",)}


@functools.lru_cache(maxsize=None)
def option_group(key):
    """The groups an option belongs to (an option may be in engine and fpu)."""
    return tuple(g for g, pats in OPTION_GROUPS.items() if any(fnmatch.fnmatch(key, p) for p in pats))


def _mask(entry):
    m = (entry.get("params") or {}).get("modules")
    if m is None or str(m).lower() == "all":
        return matrix.ALL_MODULES
    return int(str(m), 0)


def modules_run(entry):
    """The modules an old-style run runs on this entry: those of its disk
    groups, minus its module mask."""
    groups = entry.get("disk_groups") or GROUPS
    mask = _mask(entry)
    return [m for m, i in sorted(matrix.MODULE_IDS.items(), key=lambda kv: kv[1])
            if MODULE_GROUP[m] in groups and mask >> i & 1]


def _freeze(v):
    if isinstance(v, dict):
        return tuple(sorted((k, _freeze(x)) for k, x in v.items()))
    if isinstance(v, (list, tuple)):
        return tuple(_freeze(x) for x in v)
    return v


def _jit_on(v):
    return str(v).strip() not in ("", "0")


def project(entry, groups):
    """The entry's values on these groups: (name, value) pairs, sorted.
    engine_class keeps only whether the JIT is on (not its cache size) and
    the CPU thread."""
    out = []
    for k, v in entry["options"].items():
        gs = option_group(k)
        if "engine" in groups and "engine" in gs:
            out.append((k, str(v)))
        elif "engine_class" in groups and "engine_class" in gs:
            out.append((k, str(_jit_on(v)) if k == "cachesize" else str(v)))
        elif any(g in groups for g in gs if g not in ("engine", "engine_class")):
            out.append((k, str(v)))
    for g in groups:
        for f in FIELD_GROUPS.get(g, ()):
            if entry.get(f) not in (None, "", [], {}):
                out.append((f, _freeze(entry[f])))
    return tuple(sorted(set(out), key=lambda kv: kv[0]))


def key(module, entry):
    return (module, project(entry, AXES[module]))


def required(entries):
    """{coverage key: [entry ids that an old-style run covers it on]}."""
    out = {}
    for e in entries:
        for m in modules_run(e):
            out.setdefault(key(m, e), []).append(e["id"])
    return out


def unclassified_options(entries):
    """Options set by some entry that belong to no group."""
    return sorted({k for e in entries for k in e["options"] if not option_group(k)})
