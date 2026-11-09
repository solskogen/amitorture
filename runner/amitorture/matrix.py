"""The test matrix (data-driven).

An *entry* is one emulated machine: a model preset (chipset_compatible), a
Motorola CPU variant, an FPU, a core mode (fast / compatible / cycle-exact),
a video standard, the ROM(s) it needs and the exact Amiberry options.

Named matrices:
  smoke     one per big code path, plus hostspeed and strict instruction policies
  standard  about 60: every model preset, every CPU variant, the core modes,
            JIT twins, the JIT sub-options on one machine, threaded CPU,
            softfloat
  full      everything: every CPU variant x FPU x core mode, every model x
            PAL/NTSC, and every twin of every entry that allows it
  benchmarks hostspeed only, every CPU family plus legal JIT/threaded twins

*Twins.* A twin differs from its reference entry (``twin_of``) only in the
options its ``twin_kind`` is about, so a difference in results is the
engine's:
  jit        cachesize > 0 (+ one comp_* option or cache size for the
             sub-option twins); parity enforced (engine_mismatch)
  threaded   cpu_threaded=true; parity enforced
  softfloat  fpu_softfloat=true; compared and reported, not enforced (the
             native FPU on this host is 64-bit double, softfloat is 80-bit
             extended, so results may legitimately differ)

Where Amiberry does not support a combination (MMU with JIT, JIT with
cycle-exact, threaded CPU with cycle-exact, FPU on a 68000 ...)
no entry is generated for it. Anything Amiberry still adjusts is found at run
time with --dump-config, and the entry is reported as "config adjusted by
emulator".

Option names: Amiberry src/cfgfile.cpp. Fixups: src/main.cpp fixup_cpu().
"""
from __future__ import annotations

import copy
import fnmatch

MATRICES = ("smoke", "standard", "full", "benchmarks")

# ---------------------------------------------------------------------------
# ROMs (romdb ids in order of preference, see romdb.json / `amitorture roms`)
ROMS = {
    "A1000": {"ids": [6, 5], "what": "Kickstart 1.2/1.3 (the A1000 loads it into WOM)"},
    "A500": {"ids": [6, 5, 14, 7, 10], "what": "a 68000 Kickstart (1.2/1.3/2.04/2.05/3.1 40.063)"},
    "A500+": {"ids": [7, 14, 10, 6], "what": "Kickstart 2.04 (or 3.1 40.063 / 2.05 / 1.3)"},
    "A600": {"ids": [10, 14, 7], "what": "Kickstart 2.05 A600 (or 3.1 40.063 / 2.04)"},
    "A2000": {"ids": [14, 6, 5, 7], "what": "Kickstart 3.1 40.063 A500/A600/A2000 (or 1.3/1.2/2.04)"},
    "A3000": {"ids": [61, 16, 13], "what": "Kickstart 3.1 40.068 A3000 (or the A4000 ROM)"},
    "A4000": {"ids": [16, 13, 31, 12, 278, 283, 288, 293, 336, 17],
              "what": "Kickstart 3.1 40.068 A4000 (or 40.070 A4000T / 3.x A4000)"},
    "A4000T": {"ids": [17, 336, 16], "what": "Kickstart 3.1 40.070 A4000T (or 3.2.3 A4000T)"},
    "A1200": {"ids": [15, 11, 333, 276, 281, 286, 291], "what": "Kickstart 3.1 40.068 A1200 (or 3.0 / 3.2.x A1200)"},
    "CD32": {"ids": [18], "what": "CD32 Kickstart 3.1 40.060",
             "ext": {"ids": [19], "what": "CD32 extended ROM r40.60"}},
    "CDTV": {"ids": [6, 5], "what": "Kickstart 1.3 (CDTV)",
             "ext": {"ids": [22, 21, 20], "what": "CDTV extended ROM (2.7 / 2.30 / 1.0)"}},
    "AROS": {"builtin": ":AROS", "what": "the AROS ROM built into Amiberry"},
}

# ---------------------------------------------------------------------------
# Model presets. chip in 512K units, bogo (slow) in 256K units (cfgfile.cpp).
MODELS = {
    "A1000": {"cs": "A1000", "chipset": "ocs", "rom": "A1000", "cpu": "68000", "core": "ce", "chip": "1", "bogo": "0"},
    "A500": {"cs": "A500", "chipset": "ocs", "rom": "A500", "cpu": "68000", "core": "ce", "chip": "1", "bogo": "2"},
    "A500+": {"cs": "A500+", "chipset": "ecs", "rom": "A500+", "cpu": "68000", "core": "ce", "chip": "2", "bogo": "0"},
    "A600": {"cs": "A600", "chipset": "ecs", "rom": "A600", "cpu": "68000", "core": "compat", "chip": "2", "bogo": "0"},
    "A2000": {"cs": "A2000", "chipset": "ecs_agnus", "rom": "A2000", "cpu": "68000", "core": "ce", "chip": "2",
              "bogo": "0"},
    "A3000": {"cs": "A3000", "chipset": "ecs", "rom": "A3000", "cpu": "68030", "fpu": "68882", "core": "compat",
              "chip": "4", "bogo": "0"},
    "A4000": {"cs": "A4000", "chipset": "aga", "rom": "A4000", "cpu": "68040nommu", "core": "fast", "chip": "4",
              "bogo": "0"},
    "A4000T": {"cs": "A4000T", "chipset": "aga", "rom": "A4000T", "cpu": "68040nommu", "core": "fast", "chip": "4",
               "bogo": "0"},
    "A1200": {"cs": "A1200", "chipset": "aga", "rom": "A1200", "cpu": "68EC020", "core": "fast", "chip": "4",
              "bogo": "0"},
    # CD32/CDTV boot from CD (runner: cdimage.py), as the real machines do
    # CD32: the pad on port 1 (cd32joy, fed by keyboard layout A: no host
    # keys, so every button reads released)
    "CD32": {"cs": "CD32", "chipset": "aga", "rom": "CD32", "cpu": "68EC020", "core": "fast", "chip": "4", "bogo": "0",
             "boot": "cd", "opts": {"joyport1": "kbd1", "joyport1mode": "cd32joy", "flash_file": "{media}/cd32.nvr"},
             "media_files": [{"name": "cd32.nvr", "size": 1024}]},
    "CDTV": {"cs": "CDTV", "chipset": "ecs_agnus", "rom": "CDTV", "cpu": "68000", "core": "ce", "chip": "2",
             "bogo": "0", "boot": "cd"},
    "AROS-ECS": {"cs": "A500+", "chipset": "ecs", "rom": "AROS", "cpu": "68000", "core": "ce", "chip": "1",
                 "bogo": "2"},
    "AROS-AGA": {"cs": "A1200", "chipset": "aga", "rom": "AROS", "cpu": "68020", "core": "fast", "chip": "4",
                 "bogo": "0"},
}
CHIPSET_LABEL = {"ocs": "OCS", "ecs": "ECS", "ecs_agnus": "ECS", "ecs_denise": "ECS", "aga": "AGA"}

# ---------------------------------------------------------------------------
# CPU variants. label = what the guest probe should report (CONTRACT §8).
# fpus: the FPUs the variant can have ("internal" = the 68040/68060 on-chip one).
CPUS = {
    "68000": {"label": "68000", "cpu_model": "68000", "addr24": True, "mmu": "0", "fpus": ["none"]},
    "68010": {"label": "68010", "cpu_model": "68010", "addr24": True, "mmu": "0", "fpus": ["none"]},
    "68EC020": {"label": "68EC020", "cpu_model": "68020", "addr24": True, "mmu": "0",
                "fpus": ["none", "68881", "68882"]},
    "68020": {"label": "68020", "cpu_model": "68020", "addr24": False, "mmu": "0", "fpus": ["none", "68881", "68882"]},
    # a 68030 without MMU emulation is what the guest sees as an EC030
    "68EC030": {"label": "68EC030", "cpu_model": "68030", "addr24": False, "mmu": "0",
                "fpus": ["none", "68881", "68882"]},
    # ... and the MMU core in EC mode (mmu_model=68ec030)
    "68EC030mmuec": {"label": "68EC030", "cpu_model": "68030", "addr24": False, "mmu": "68ec030",
                     "fpus": ["none", "68882"]},
    "68030": {"label": "68030", "cpu_model": "68030", "addr24": False, "mmu": "68030",
              "fpus": ["none", "68881", "68882"]},
    "68EC040": {"label": "68EC040", "cpu_model": "68040", "addr24": False, "mmu": "0", "fpus": ["none"]},
    "68LC040": {"label": "68LC040", "cpu_model": "68040", "addr24": False, "mmu": "68040", "fpus": ["none"]},
    "68040": {"label": "68040", "cpu_model": "68040", "addr24": False, "mmu": "68040", "fpus": ["internal"]},
    # FPU but no MMU emulation: the usual JIT setup. No such real part; the
    # probe may call it 68EC040 or 68040 (report.probe_mismatches knows).
    "68040nommu": {"label": "68040", "cpu_model": "68040", "addr24": False, "mmu": "0", "fpus": ["internal"]},
    # the MMU core in EC mode (mmu_ec: TTRs, no paging) for the 040/060 too
    "68EC040mmuec": {"label": "68EC040", "cpu_model": "68040", "addr24": False, "mmu": "68ec040", "fpus": ["none"]},
    "68EC060": {"label": "68EC060", "cpu_model": "68060", "addr24": False, "mmu": "0", "fpus": ["none"]},
    "68EC060mmuec": {"label": "68EC060", "cpu_model": "68060", "addr24": False, "mmu": "68ec060", "fpus": ["none"]},
    "68LC060": {"label": "68LC060", "cpu_model": "68060", "addr24": False, "mmu": "68060", "fpus": ["none"]},
    "68060": {"label": "68060", "cpu_model": "68060", "addr24": False, "mmu": "68060", "fpus": ["internal"]},
    "68060nommu": {"label": "68060", "cpu_model": "68060", "addr24": False, "mmu": "0", "fpus": ["internal"]},
}
# 68060 PCR revisions (Amiberry cpu060_revision); the first is the default
CPU060_REVISIONS = [1, 6]

# Core modes. Written in this order: cycle_exact (cfgfile.cpp) sets the three
# *_cycle_exact options itself, so it comes last.
CORES = {
    "fast": {"cpu_compatible": "false", "cpu_cycle_exact": "false", "cpu_memory_cycle_exact": "false",
             "blitter_cycle_exact": "false", "cycle_exact": "false"},
    "compat": {"cpu_compatible": "true", "cpu_cycle_exact": "false", "cpu_memory_cycle_exact": "false",
               "blitter_cycle_exact": "false", "cycle_exact": "false"},
    "ce": {"cpu_compatible": "true", "cpu_cycle_exact": "true", "cpu_memory_cycle_exact": "true",
           "blitter_cycle_exact": "true", "cycle_exact": "true"},
}

# ---------------------------------------------------------------------------
# JIT: the defaults every config writes (amiberry_cfg.COMMON) and the twins.
JIT_CACHE = "8192"
JIT_DEFAULTS = {"compfpu": "true", "comp_trustbyte": "direct", "comp_trustword": "direct",
                "comp_trustlong": "direct", "comp_trustnaddr": "direct", "comp_nf": "true",
                "comp_constjump": "true", "comp_flushmode": "soft", "comp_catchfault": "true"}
# suffix -> options changed on top of the default JIT twin
JIT_VARIANTS = {
    "": {},
    "cache128": {"cachesize": "128"},          # MIN_JIT_CACHE (include/newcpu.h)
    "cache16384": {"cachesize": "16384"},      # MAX_JIT_CACHE
    "trustbyte-ind": {"comp_trustbyte": "indirect"},
    "trustword-ind": {"comp_trustword": "indirect"},
    "trustlong-ind": {"comp_trustlong": "indirect"},
    "trustnaddr-ind": {"comp_trustnaddr": "indirect"},
    "nocompfpu": {"compfpu": "false"},
    "noconstjump": {"comp_constjump": "false"},
    "hardflush": {"comp_flushmode": "hard"},
    "nonf": {"comp_nf": "false"},
    "nocatchfault": {"comp_catchfault": "false"},
}
JIT_OPTION_KEYS = {"cachesize", *JIT_DEFAULTS}
# options that do not make two entries different machines (timing knobs of
# twins are what a twin is about; these are only labels)
TWIN_IGNORE = {"config_description"}


def cpu_supports_jit(cpu_key, core):
    c = CPUS[cpu_key]
    return c["cpu_model"] not in ("68000", "68010") and c["mmu"] == "0" and core == "fast"


def cpu_supports_threaded(cpu_key, core):
    # Amiberry runs the CPU thread for every model and core except cycle-exact
    # (since its automatic CPU thread; an older build declines it for
    # 68000/010, compatible and MMU, and the entry is reported as adjusted)
    return core != "ce"


def fpu_model_value(cpu_key, fpu):
    if fpu == "none":
        return "0"
    if fpu == "internal":
        return CPUS[cpu_key]["cpu_model"]
    return fpu


def fpu_label(cpu_key, fpu):
    return "none" if fpu == "none" else (CPUS[cpu_key]["cpu_model"] if fpu == "internal" else fpu)


def _memory(model, cpu_key):
    """Memory layout. 32-bit CPUs get RAM above 16 MB (Z3 or motherboard
    fast) so the probe can tell a 68020 from a 68EC020 by the address bus."""
    m = MODELS[model]
    c = CPUS[cpu_key]
    mem = {"chipmem_size": m["chip"], "bogomem_size": m["bogo"], "fastmem_size": "0", "z3mem_size": "0",
           "mbresmem_size": "0", "a3000mem_size": "0"}
    if c["addr24"]:
        if c["cpu_model"] not in ("68000", "68010"):
            mem["fastmem_size"] = "4"        # Zorro II fast for the EC020
    elif m["cs"] in ("A3000", "A4000", "A4000T"):
        mem["mbresmem_size"] = "8"           # motherboard fast at $07xxxxxx
    else:
        mem["z3mem_size"] = "16"
        mem["fastmem_size"] = "4" if m["cs"] == "A1200" else "0"
    return mem


def make_entry(model, cpu_key=None, fpu=None, core=None, ntsc=False, extra=None, eid=None, title_extra=""):
    """One base entry (no twins)."""
    m = MODELS[model]
    cpu_key = cpu_key or m["cpu"]
    c = CPUS[cpu_key]
    if fpu is None:
        if m.get("fpu") in c["fpus"]:
            fpu = m["fpu"]
        elif "internal" in c["fpus"]:
            fpu = "internal"
        else:
            fpu = c["fpus"][0]
    core = core or m["core"]
    chip = m["chipset"]
    opts = {"chipset_compatible": m["cs"], "chipset": chip, "ntsc": "true" if ntsc else "false",
            "cpu_model": c["cpu_model"], "fpu_model": fpu_model_value(cpu_key, fpu), "mmu_model": c["mmu"]}
    core_opts = dict(CORES[core])
    opts["cpu_compatible"] = core_opts.pop("cpu_compatible")
    opts["cpu_24bit_addressing"] = "true" if c["addr24"] else "false"
    opts.update(core_opts)
    opts["cpu_speed"] = "real" if (core == "ce" or (core == "compat" and c["cpu_model"] in ("68000", "68010"))) \
        else "max"
    opts["cpu_multiplier"] = "2" if c["cpu_model"] in ("68000", "68010") else "4"
    opts.update(_memory(model, cpu_key))
    opts["cachesize"] = "0"
    opts["cpu_threaded"] = "false"
    opts["fpu_softfloat"] = "false"
    opts.update(m.get("opts") or {})
    if c["cpu_model"] == "68060":
        opts["cpu060_revision"] = str(CPU060_REVISIONS[0])
    if extra:
        opts.update(extra)
    fl = fpu_label(cpu_key, fpu)
    if eid is None:
        parts = [model.lower().replace("+", "plus"), cpu_key.lower()]
        if fpu in ("68881", "68882"):
            parts.append(fpu[-3:])
        parts.append(core)
        if ntsc:
            parts.append("ntsc")
        eid = "-".join(parts)
    rom = ROMS[m["rom"]]
    fdesc = "" if fpu == "none" else (" (FPU)" if fpu == "internal" else f" + {fl}")
    title = (f"{model} {CHIPSET_LABEL[chip]} {'NTSC' if ntsc else 'PAL'} {c['label']}{fdesc}"
             + (f" MMU {c['mmu']}" if c["mmu"] != "0" else "")
             + {"fast": ", fast core", "compat": ", compatible core", "ce": ", cycle-exact"}[core]
             + (", 24-bit" if c["addr24"] else "") + title_extra)
    e = {"id": eid, "title": title, "model": model, "cpu_variant": cpu_key, "core": core,
         "cpu": c["label"], "fpu": fl, "chipset": CHIPSET_LABEL[chip], "video": "NTSC" if ntsc else "PAL",
         "jit": False, "twin_of": None, "twin_kind": None, "twin_options": [],
         "rom": {k: v for k, v in rom.items() if k != "ext"}, "options": opts,
         "jit_capable": cpu_supports_jit(cpu_key, core), "threaded_capable": cpu_supports_threaded(cpu_key, core),
         "has_fpu": fpu != "none", "matrices": set()}
    if m.get("media_files"):
        e["media_files"] = [dict(f) for f in m["media_files"]]   # e.g. a fresh, zeroed CD32 NVRAM per launch
    if m.get("boot"):
        e["boot"] = m["boot"]
        e["title"] += ", CD boot" if m["boot"] == "cd" else ""
    if rom.get("ext"):
        e["ext_rom"] = dict(rom["ext"])
    return e


def make_twin(base, kind, variant="", extra_opts=None):
    t = copy.deepcopy(base)
    changes = {}
    if kind == "jit":
        changes["cachesize"] = JIT_CACHE
        changes.update(JIT_VARIANTS[variant])
        suffix = "-jit" + (f"-{variant}" if variant else "")
        what = "JIT" + (f" ({', '.join(f'{k}={v}' for k, v in JIT_VARIANTS[variant].items())})" if variant else "")
        t["jit"] = True
        t["jit_variant"] = variant or "default"
    elif kind == "threaded":
        changes["cpu_threaded"] = "true"
        suffix, what = "-thr", "threaded CPU"
    elif kind == "softfloat":
        changes["fpu_softfloat"] = "true"
        suffix, what = "-sf", "softfloat FPU"
    else:
        raise ValueError(kind)
    if extra_opts:
        changes.update(extra_opts)
    t["options"].update(changes)
    t["id"] = base["id"] + suffix
    t["title"] = base["title"] + " + " + what
    t["twin_of"] = base["id"]
    t["twin_kind"] = kind
    t["twin_options"] = sorted(k for k, v in changes.items() if base["options"].get(k) != v)
    t["matrices"] = set()
    return t


def cpu_host(cpu_key):
    c = CPUS[cpu_key]
    if c["cpu_model"] in ("68000", "68010"):
        return "A500" if c["cpu_model"] == "68000" else "A2000"
    if c["cpu_model"] == "68020":
        return "A1200"
    return "A4000"


def machine_key(e):
    """What makes two entries different machines: the options, plus the test
    media controller and the boot medium."""
    return (tuple(sorted(e["options"].items())), (e.get("media") or {}).get("controller"), e.get("boot"))


def _dedupe(entries):
    seen, ids, out = set(), set(), []
    for e in entries:
        key = machine_key(e)
        if key in seen or e["id"] in ids:
            continue
        seen.add(key)
        ids.add(e["id"])
        out.append(e)
    return out


def _twins_for(base, jit_variants, threaded, softfloat):
    out = []
    if base["jit_capable"]:
        for v in jit_variants:
            if v == "nocompfpu" and not base["has_fpu"]:
                continue
            out.append(make_twin(base, "jit", v))
    if threaded and base["threaded_capable"]:
        out.append(make_twin(base, "threaded"))
        if base["jit_capable"]:
            jt = make_twin(base, "jit", "", extra_opts={"cpu_threaded": "true"})
            jt["id"] = base["id"] + "-jit-thr"
            jt["title"] = base["title"] + " + JIT + threaded CPU"
            jt["jit_variant"] = "threaded"
            out.append(jt)
    if softfloat and base["has_fpu"]:
        sf = make_twin(base, "softfloat")
        out.append(sf)
        if base["jit_capable"]:
            # JIT FPU + softfloat is refused (fixup_cpu), so with compfpu off.
            # Its parity reference is the softfloat interpreter entry (same
            # FPU emulation), not the native one: then only the JIT differs.
            js = make_twin(base, "jit", "nocompfpu", extra_opts={"fpu_softfloat": "true"})
            js["id"] = base["id"] + "-jit-sf"
            js["title"] = base["title"] + " + JIT (compfpu=false) + softfloat FPU"
            js["jit_variant"] = "softfloat"
            js["twin_of"] = sf["id"]
            if isinstance(js.get("twin_options"), list):
                js["twin_options"] = [o for o in js["twin_options"] if o != "fpu_softfloat"]
            out.append(js)
    return out


# (model, cpu variant, fpu, core, ntsc, twins); None = the model's default
SMOKE = [
    ("A500", "68000", "none", "ce", False, ()),
    ("A600", "68000", "none", "compat", True, ("thr",)),
    ("A1200", "68EC020", "none", "fast", False, ("jit",)),
    ("A3000", "68030", "68882", "compat", False, ("thr",)),
    ("A4000", "68040nommu", "internal", "fast", False, ("jit",)),
    ("A4000", "68060nommu", "internal", "fast", False, ("jit",)),
    ("AROS-AGA", "68020", "none", "fast", False, ()),
]

STANDARD = [
    # model presets
    ("A1000", None, None, None, False, ()),
    ("A500", None, None, None, False, ()),
    ("A500", None, None, None, True, ()),
    ("A500+", None, None, None, False, ()),
    ("A600", None, None, None, False, ("thr",)),
    ("A600", None, None, None, True, ()),
    ("A2000", None, None, None, False, ()),
    ("A3000", None, None, None, False, ("thr", "sf")),
    ("A4000", None, None, None, False, ("jit", "jitopts", "thr", "sf")),
    ("A4000T", None, None, None, False, ("jit",)),
    ("A1200", None, None, None, False, ("jit",)),
    ("A1200", None, None, None, True, ("jit",)),
    ("CD32", None, None, None, False, ("jit",)),
    ("CDTV", None, None, None, False, ()),
    ("AROS-ECS", None, None, None, False, ()),
    ("AROS-AGA", None, None, None, False, ("jit",)),
    # CPU variants
    ("A2000", "68010", "none", "compat", False, ("thr",)),
    ("A1200", "68020", "68881", "fast", False, ("jit",)),
    ("A1200", "68020", "68882", "fast", False, ("jit",)),
    ("A4000", "68EC030", "68882", "fast", False, ("jit",)),
    ("A4000", "68030", "68882", "fast", False, ()),
    ("A4000", "68EC040", "none", "fast", False, ("jit",)),
    ("A4000", "68LC040", "none", "fast", False, ()),
    ("A4000", "68040", "internal", "fast", False, ()),
    ("A4000", "68EC060", "none", "fast", False, ("jit",)),
    ("A4000", "68LC060", "none", "fast", False, ()),
    ("A4000", "68060", "internal", "fast", False, ()),
    ("A4000", "68060nommu", "internal", "fast", False, ("jit",)),
    # core modes on 020+
    ("A1200", "68020", "none", "compat", False, ("thr",)),
    ("A1200", "68020", "none", "ce", False, ()),
    ("A4000", "68030", "68882", "ce", False, ()),
    ("A4000", "68040nommu", "internal", "compat", False, ()),
    ("A4000", "68040nommu", "internal", "ce", False, ()),
    ("A4000", "68060nommu", "internal", "ce", False, ()),
]


# ---------------------------------------------------------------------------
# Expansion hardware: one entry per board (CONTRACT §10 modules 16-19 test
# them at register level). Each entry names the disk groups it needs
# (disk_groups: the exp disk holds rtg/expsound/network/misc_hw; core gives
# the probe and the basics on that machine), the ROMs it needs (board_roms:
# romdb ids, copied into the emulator's ROM dir and given to its option), and
# test media files it needs (media_files, "{media}" in option values).
# Option names: src/cfgfile.cpp, board tables src/gfxboard.cpp and
# src/expansion.cpp.
HW_DISKS = ["core", "exp"]
# Module ids (CONTRACT.md §10, the AMT_MODULE declarations; a unit test keeps
# this in step) for the module masks of the board entries.
MODULE_IDS = {"takeover": 0, "rt": 1, "cpu_basic": 2, "flags": 3, "exceptions": 4, "smc": 5, "interrupts": 6,
              "cia": 7, "blitter": 8, "fpu": 9, "timing": 10, "devio": 11, "display": 12, "copper": 13,
              "paula": 14, "input": 15, "rtg": 16, "expsound": 17, "network": 18, "misc_hw": 19, "mmu": 20,
              "memory": 21, "dispatch": 22, "dmacontention": 23, "misc_hw_os": 24, "memory_os": 25,
              "hostspeed": 26}


def module_mask(names):
    """run-parameter module mask (mkadf modules=) for these module names"""
    m = 0
    for n in names:
        m |= 1 << MODULE_IDS[n]
    return f"{m:#x}"


# Board entries run only what tests the board: the exp disk with the board's
# own module plus misc_hw_os (the board's autoconfig data, ~0 s). Measured on
# an A4000 RTG entry: core disk ~16 s, devio ~58 s and misc_hw ~8 s of the exp
# disk, rtg 0.5 s, plus ~13 s per disk launch; the core disk, devio and
# misc_hw gave the same results on every board entry. One entry per bus keeps
# both disks with every module (FULL_BOARD_ENTRIES), so a board that
# disturbs the rest of the machine (its interrupt line, the memory map) is
# still seen.
FULL_BOARD_ENTRIES = {"a4000-rtg-picassoii", "a4000-rtg-cv64z3", "a4000-rtg-s3trio64pci",
                      "a4000-snd-toccata", "a1200-snd-prelude1200", "a4000-snd-uaesndz3", "a4000-snd-es1370",
                      "a4000-net-a2065"}


def _board_only(e, modules):
    if e["id"] in FULL_BOARD_ENTRIES:
        return e
    e["disk_groups"] = ["exp"]
    e.setdefault("params", {})["modules"] = module_mask(list(modules) + ["misc_hw_os"])
    return e
PIV_ROM = {"ids": [91], "what": "Picasso IV flash ROM (picasso_iv_flash.rom)", "option": "picassoiv_rom_file"}
PCI_BRIDGE = {"prometheus_rom_file": ":ENABLED"}      # Matay Prometheus, Zorro III PCI bridge
# (gfxcard_type config name, bus, VRAM MB); every hardware RTG board of
# gfxboard.cpp except those needing a host board the matrix cannot give
# (x86 VGA: bridgeboard + BIOS; Altais: DraCo; EGS_110_24: GVP local bus)
# and the driver-only uaegfx (ZorroII/ZorroIII). The Pixel64 is on an
# AteoBus on real hardware; Amiberry emulates it as an autoconfig board of
# its own (gfxboard.cpp GFXBOARD_ID_PIXEL64, 2026/255+254, map_banks_z2).
RTG_CARDS = [
    ("A2410", "z2", 1), ("Spectrum28/24_Z2", "z2", 2), ("Spectrum28/24_Z3", "z3", 2),
    ("Piccolo_Z2", "z2", 2), ("Piccolo_Z3", "z3", 2), ("PiccoloSD64_Z2", "z2", 4), ("PiccoloSD64_Z3", "z3", 4),
    ("CV64_Z3", "z3", 4), ("CV643D_Z2", "z2", 4), ("CV643D_Z3", "z3", 4),
    ("PicassoII", "z2", 2), ("PicassoII+", "z2", 2), ("PicassoIV_Z2", "z2", 4), ("PicassoIV_Z3", "z3", 4),
    ("Retina_Z2", "z2", 4), ("Retina_Z3", "z3", 4), ("MerlinZ2", "z2", 2), ("MerlinZ3", "z3", 4),
    ("GraffityZ2", "z2", 2), ("GraffityZ3", "z3", 2), ("Visiona", "z2", 4), ("RainbowIII", "z3", 4),
    ("Domino", "z2", 1), ("OmnibusET4000", "z2", 1), ("OmnibusET4000W32", "z2", 1), ("Harlequin_PAL", "z2", 2),
    ("RainbowII", "z2", 2), ("ZZ9000_Z2", "z2", 4), ("ZZ9000_Z3", "z3", 128), ("Pixel64", "z2", 2),
    ("PERMEDIA2_PCI", "pci", 8), ("V3_3000", "pci", 16), ("S3VIRGE_PCI", "pci", 4), ("S3TRIO64_PCI", "pci", 4),
    ("Matrox_Millennium", "pci", 4), ("Matrox_Millennium_II", "pci", 8), ("Matrox_Mystique", "pci", 8),
    ("Matrox_Mystique220", "pci", 8),
]
# (expansion board name, bus, models): <name>_rom_file=:ENABLED (ROMTYPE_NOT
# boards). Each board on every machine that can take it: an A500 68000, an
# A1200 68020 and an A4000 68040, the 020/040 ones with a JIT twin.
SOUND_CARDS = [("toccata", "z2", ("A500", "A1200", "A4000")), ("prelude", "z2", ("A500", "A1200", "A4000")),
               ("prelude1200", "clockport", ("A1200",)), ("uaesnd_z2", "z2", ("A500", "A1200", "A4000")),
               ("uaesnd_z3", "z3", ("A4000",)), ("es1370", "pci", ("A4000",)), ("fm801", "pci", ("A4000",))]
SOUND_CPU = {"A500": "68000", "A1200": "68020", "A4000": None}
# network: A2065 in standard; the others in full only
NET_CARDS = [("a2065", {"a2065": "slirp"}, "A4000", True),
             ("ariadne", {"ariadne_rom_file": ":ENABLED"}, "A4000", False),
             ("ariadne2", {"ariadne2_rom_file": ":ENABLED"}, "A4000", False),
             ("hydra", {"hydra_rom_file": ":ENABLED"}, "A4000", False),
             ("xsurf", {"xsurf_rom_file": ":ENABLED"}, "A4000", False),
             ("xsurf100z3", {"xsurf100z3_rom_file": ":ENABLED"}, "A4000", False),
             ("ne2000_pci", dict(PCI_BRIDGE, ne2000_pci="slirp"), "A4000", False),
             ("ne2000_pcmcia", {"ne2000_pcmcia": "slirp"}, "A1200", False)]


def _slug(name):
    import re as _re
    return _re.sub(r"[^a-z0-9]+", "", name.lower().replace("+", "plus"))


def _hw_entry(model, eid, what, options, disk_groups=HW_DISKS, board_roms=(), media_files=(), cpu_key=None,
              fpu=None, core=None, title=None):
    base = model if model in MODELS else "A4000"
    if cpu_key is None:
        cpu_key = "68040nommu" if base in ("A4000", "A4000T") else None
    e = make_entry(base, cpu_key, fpu, core, False, extra=options, eid=eid, title_extra=f", {what}")
    if title:
        e["title"] = title
    e["hardware"] = what
    e["disk_groups"] = list(disk_groups)
    # one entry per board: no engine twins (the CPU-side matrix has those)
    e["jit_capable"] = e["threaded_capable"] = False
    if board_roms:
        e["board_roms"] = [dict(r) for r in board_roms]
    if media_files:
        e["media_files"] = [dict(f) for f in media_files]
    return e


def hardware_entries(full=False):
    """The expansion/board entries (one per board); `full` adds the boards
    that are only in the full matrix."""
    out = []
    for name, bus, vram in RTG_CARDS:
        o = {"gfxcard_type": name, "gfxcard_size": str(vram)}
        if bus == "pci":
            o.update(PCI_BRIDGE)
        out.append(_board_only(_hw_entry("A4000", f"a4000-rtg-{_slug(name)}", f"RTG {name} ({bus.upper()}, {vram} MB)",
                                         o, board_roms=[PIV_ROM] if name.startswith("PicassoIV") else ()), ["rtg"]))
    for name, bus, models in SOUND_CARDS:
        for model in models:
            o = {f"{name}_rom_file": ":ENABLED"}
            if bus == "pci":
                o.update(PCI_BRIDGE)
            e = _hw_entry(model, f"{model.lower()}-snd-{_slug(name)}", f"sound card {name} ({bus})", o,
                          cpu_key=SOUND_CPU[model], disk_groups=["core", "exp"])
            if name == "prelude1200":
                # expsound probes the clockport only with the unsafe flag (on a
                # real A1200 nothing may be there); this entry configures the card
                e["flags"] = ["unsafe"]
            _board_only(e, ["expsound", "paula"] if name.startswith("uaesnd") else ["expsound"])
            if name.startswith("uaesnd"):
                # paula's uaesnd_mono_keeps_right (a Paula tone next to a
                # uaesnd mono stream) is on the chip disk
                e["disk_groups"] = e["disk_groups"] + ["chip"]
            out.append(e)
            if model != "A500" and bus != "pci":
                e["jit_capable"] = True
                t = make_twin(e, "jit")
                t["disk_groups"] = list(e["disk_groups"])
                out.append(t)
    # ZZ9000 network with the slirp backend (the ARP answer from 10.0.2.2
    # makes network.zz9000_arp testable)
    for e in out:
        if e["options"].get("gfxcard_type", "").startswith("ZZ9000"):
            e["options"]["zz9000_net"] = "slirp"
            if "modules" in e.get("params", {}):
                e["params"]["modules"] = module_mask(["rtg", "network", "misc_hw_os"])
    for name, o, model, std in NET_CARDS:
        if std or full:
            cpu = "68020" if model == "A1200" else None
            out.append(_board_only(_hw_entry(model, f"{model.lower()}-net-{_slug(name)}", f"network {name}", dict(o),
                                             cpu_key=cpu), ["network"]))
    # Gayle: media.py gives every A600/A1200 entry a test-owned IDE hardfile
    # and the PCMCIA SRAM card (4 MB + CIS), so a600-68000-compat (KS 2.05)
    # is the "A600 Gayle IDE + SRAM card" machine; no separate entry.
    # Zorro II + Zorro III RAM and a Zorro III RTG board on the big-box
    # machines: the memory modules (cpux disk) test the RAM, rtg the board
    for model in ("A3000", "A4000"):
        o = {"fastmem_size": "4", "z3mem_size": "16", "gfxcard_type": "CV64_Z3", "gfxcard_size": "4"}
        e = _hw_entry(model, f"{model.lower()}-z2z3-rtg", "Zorro II 4 MB + Zorro III 16 MB RAM, CV64 Z3", o,
                      cpu_key="68030" if model == "A3000" else None,
                      fpu="68882" if model == "A3000" else None, disk_groups=["cpux", "exp"])
        e.setdefault("params", {})["modules"] = module_mask(["memory", "memory_os", "rtg", "misc_hw_os"])
        out.append(e)
    # SCSI controllers with their boot ROMs (media.py attaches a test-owned
    # RDB hardfile to them; the devio module on the exp disk tests it)
    for model, ctl, cpu in (("A2000", "a2091", None), ("A4000", "a4091", None)):
        e = _hw_entry(model, f"{model.lower()}-scsi-{ctl}", f"SCSI {ctl.upper()} (test-owned hardfile)", {},
                      disk_groups=["core", "exp"], cpu_key=cpu)
        e["media"] = {"controller": ctl}
        out.append(e)
    # RTCs: the A500+ (MSM6242B, from chipset_compatible) and an A500 with an
    # A501-style clock; the A3000/A4000 RF5C01 comes with their presets.
    # A preset decides the RTC by design (docs/findings, "Config facts"), so
    # rtc= is ignored here: these are the A500 (slow RAM) and A500+ presets
    # with their own clock. An RTC choice needs chipset_compatible=- (Custom).
    out.append(_board_only(_hw_entry("A500+", "a500plus-rtc", "RTC MSM6242B (A500+ built in)", {"rtc": "MSM6242B"}),
                           ["misc_hw"]))
    out.append(_board_only(_hw_entry("A500", "a500-rtc-a501", "RTC MSM6242B (A501 trapdoor)", {"rtc": "MSM6242B"}),
                           ["misc_hw"]))
    if full:
        out.append(_board_only(_hw_entry("A4000", "a4000-rtc-rp5c01", "RTC RF5C01 (A4000 built in)",
                                         {"rtc": "RP5C01A"}), ["misc_hw"]))
        # PPC accelerators (QEMU PPC plugin; the 68k side only: misc_hw/memory see the board)
        for cb, ids, cpu in (("CyberStormPPC", [327, 326, 98], "68060"), ("BlizzardPPC", [331, 329, 100], "68060")):
            o = {"cpuboard_type": cb, "cpuboardmem1_size": "32", "ppc_implementation": "qemu", "ppc_model": "automatic"}
            if cb == "CyberStormPPC":
                o["mbresmem_size"] = "32"     # the board RAM is the A4000's motherboard-resource RAM slot
            rom = {"ids": ids, "what": f"{cb} flash ROM", "option": "cpuboard_rom_file"}
            model = "A1200" if cb.startswith("Blizzard") else "A4000"
            out.append(_hw_entry(model, f"{model.lower()}-cpu-{_slug(cb)}", f"accelerator {cb}", o,
                                 board_roms=[rom], cpu_key="68060nommu", fpu="internal",
                                 disk_groups=["core", "exp", "cpux", "mmu"]))
    for e in out:
        e["matrices"] = set()
    return out


# hostspeed (docs/cases/hostspeed.md) at cpu_speed=real. On the fast/JIT
# entries above cpu_speed=max: Amiberry then gives the CPU whatever host time
# a frame leaves, so the frames stay on time and a slow instruction only
# lowers the emulated CPU's throughput. At cpu_speed=real the emulated CPU
# has a fixed speed, and an instruction the host cannot emulate fast enough
# makes the emulation fall behind real time, which the runner measures. These
# entries run only the cpux disk with only hostspeed; the 020+ ones have a
# JIT twin (Amiberry keeps the JIT on at cpu_speed=real: fixup_cpu has no
# rule against it, and the run's log says "JIT: ..."). The cycle-exact and
# compatible entries run at real speed anyway and run hostspeed with the rest
# of the cpux disk.
# (model, cpu variant, fpu, JIT twin, matrices)
SPEED_SPECS = [
    ("A1200", "68020", "68882", True, ("smoke", "standard")),
    ("A4000", "68040nommu", "internal", True, ("standard",)),
    ("A4000", "68060nommu", "internal", True, ("standard",)),
    ("A500", "68000", "none", False, ("standard",)),
]


def speed_entries(name):
    """The cpu_speed=real hostspeed entries of matrix `name` (and their JIT twins)."""
    out = []
    for model, cpu_key, fpu, jit, mats in SPEED_SPECS:
        if name not in mats and name != "full":
            continue
        parts = [model.lower(), cpu_key.lower()] + ([fpu[-3:]] if fpu in ("68881", "68882") else []) + ["fast", "real"]
        e = make_entry(model, cpu_key, fpu, "fast", extra={"cpu_speed": "real"}, eid="-".join(parts),
                       title_extra=", cpu_speed=real (hostspeed only)")
        e["disk_groups"] = ["cpux"]
        e.setdefault("params", {})["modules"] = module_mask(["hostspeed"])
        out.append(e)
        if jit and e["jit_capable"]:
            out.append(make_twin(e, "jit"))
    for e in out:
        e["matrices"] = {name}
    return out


def benchmark_entries():
    """CPU-family coverage without the normal max-speed hostspeed exclusion."""
    out = []
    for model, cpu, fpu, core in (
        ("A500", "68000", "none", "compat"),
        ("A2000", "68010", "none", "compat"),
        ("A1200", "68020", "none", "fast"),
        ("A3000", "68030", "68882", "compat"),
        ("A4000", "68EC030", "none", "fast"),
        ("A4000", "68040nommu", "internal", "fast"),
        ("A4000", "68060nommu", "internal", "fast"),
    ):
        base = make_entry(model, cpu, fpu, core, eid=f"bench-{cpu.lower()}",
                          title_extra=", benchmarks only")
        base["disk_groups"] = ["cpux"]
        base["params"] = {"modules": module_mask(["hostspeed"])}
        entries = [base] + _twins_for(base, [""], True, False)
        for entry in entries:
            entry["matrices"] = {"benchmarks"}
        out.extend(entries)
    return out


def strict_policy_entries(name):
    """Hardware-only traps need an interpreter lane without compatibility emulation."""
    out = []
    specs = [("A3000", "68030", "68882", "compat", "a3000-68030-882-compat"),
             ("A4000", "68040nommu", "internal", "fast", "a4000-68040nommu-fast"),
             ("A4000", "68060nommu", "internal", "fast", "a4000-68060nommu-fast")]
    for model, cpu, fpu, core, base_id in specs:
        e = make_entry(model, cpu, fpu, core,
                       extra={"cpu_no_unimplemented": "true", "fpu_no_unimplemented": "true",
                              "fpu_strict": "true"},
                       eid=f"{base_id}-strict",
                       title_extra=", hardware instruction/operand policy")
        e["disk_groups"] = ["core", "fpu"]
        e["params"] = {"modules": module_mask(["exceptions", "fpu"])}
        e["jit_capable"] = e["threaded_capable"] = False
        e["matrices"] = {name}
        out.append(e)
    return out


# MMU configurations (mmu_model = the CPU's own MMU, or the EC variant's TTRs
# only). JIT does not support the MMU, so these have no JIT twin (the
# generator never makes one for mmu != 0); every disk runs (the mmu module
# has its own disk, the exceptions/smc modules on core see the MMU too).
MMU_SPECS = [
    ("A3000", "68030", "68882", "compat", False, ()),
    ("A4000", "68030", "68882", "ce", False, ()),
    ("A4000", "68EC030mmuec", "none", "fast", False, ()),
    ("A4000", "68040", "internal", "compat", False, ()),
    ("A4000", "68LC040", "none", "compat", False, ()),
    ("A4000", "68EC040mmuec", "none", "fast", False, ()),
    ("A4000", "68060", "internal", "compat", False, ()),
    ("A4000", "68LC060", "none", "compat", False, ()),
    ("A4000", "68EC060mmuec", "none", "fast", False, ()),
]


def _from_specs(specs, name):
    out = []
    for model, cpu_key, fpu, core, ntsc, tags in specs:
        b = make_entry(model, cpu_key, fpu, core, ntsc)
        out.append(b)
        jv = list(JIT_VARIANTS) if "jitopts" in tags else ([""] if "jit" in tags else [])
        out.extend(_twins_for(b, jv, "thr" in tags, "sf" in tags))
    out = _dedupe(out)
    for e in out:
        e["matrices"].add(name)
    return out


def _bases_full():
    out = []
    # every model preset, PAL and NTSC, in its natural configuration
    for model in MODELS:
        for ntsc in (False, True):
            out.append(make_entry(model, ntsc=ntsc))
    # every CPU variant x FPU x core mode, on the variant's host model
    for cpu_key, c in CPUS.items():
        for fpu in c["fpus"]:
            for core in CORES:
                out.append(make_entry(cpu_host(cpu_key), cpu_key, fpu, core))
    # 68060 revisions (full 68060, fast core)
    for rev in CPU060_REVISIONS[1:]:
        for ck in ("68060", "68060nommu"):
            out.append(make_entry("A4000", ck, "internal", "fast", extra={"cpu060_revision": str(rev)},
                                  eid=f"a4000-{ck}-fast-rev{rev}", title_extra=f", PCR revision {rev}"))
    # cycle-exact with cpu_multiplier=0 (cpu clock derived per CPU model in
    # newcpu.cpp update_68k_cycles; reaches the 68040/060 cycle-unit branch)
    for ck in ("68020", "68030", "68040nommu", "68060nommu"):
        fpu = "internal" if "internal" in CPUS[ck]["fpus"] else "none"
        out.append(make_entry(cpu_host(ck), ck, fpu, "ce", extra={"cpu_multiplier": "0"},
                              eid=f"{cpu_host(ck).lower()}-{ck.lower()}-ce-mult0", title_extra=", cpu_multiplier=0"))
    # chipset variants on a 68000 A500: ECS Agnus only, ECS Denise only, full ECS
    for chip in ("ecs_agnus", "ecs_denise", "ecs"):
        for ntsc in (False, True):
            e = make_entry("A500", "68000", "none", "ce", ntsc, extra={"chipset": chip},
                           eid=f"a500-68000-ce-{chip.replace('_', '')}" + ("-ntsc" if ntsc else ""),
                           title_extra=f", chipset={chip}")
            e["chipset"] = CHIPSET_LABEL[chip]
            out.append(e)
    return _dedupe(out)


def _full():
    out = []
    for b in _bases_full():
        out.append(b)
        out.extend(_twins_for(b, list(JIT_VARIANTS), True, True))
    out.extend(_from_specs(MMU_SPECS, "full"))
    out.extend(hardware_entries(full=True))
    out = _dedupe(out)
    for e in out:
        e["matrices"].add("full")
    return out


_CACHE = {}


ALL_MODULES = 0xFFFFFFFFFFFFFFFF


def _hostspeed_only_at_real_speed(ents):
    """hostspeed's verdict (does the emulator keep real time?) only means
    something at cpu_speed=real: at max, Amiberry keeps the frames on time
    and gives the CPU whatever is left, so rtf stays about 1 and nothing can
    fail. Its blocks take ~40-120 s per entry, so the max-speed entries leave
    it out (their module mask, ANDed with --param modules=)."""
    bit = 1 << MODULE_IDS["hostspeed"]
    for e in ents:
        if str(e["options"].get("cpu_speed", "")).lower() != "max":
            continue
        p = e.setdefault("params", {})
        cur = int(str(p.get("modules", ALL_MODULES)), 0) if str(p.get("modules", "")).lower() != "all" else ALL_MODULES
        if cur & bit:
            p["modules"] = f"{cur & ~bit:#x}"
    return ents


def expand(name="standard"):
    """The entries of a named matrix, each twin right after its reference."""
    if name not in MATRICES:
        raise ValueError(f"unknown matrix {name!r}; known: {', '.join(MATRICES)}")
    if name not in _CACHE:
        if name == "benchmarks":
            _CACHE[name] = benchmark_entries()
            return copy.deepcopy(_CACHE[name])
        elif name == "smoke":
            _CACHE[name] = _dedupe(_from_specs(SMOKE, "smoke") + speed_entries("smoke"))
        elif name == "standard":
            ents = _from_specs(STANDARD + MMU_SPECS, "standard") + hardware_entries() + speed_entries("standard")
            ents = _dedupe(ents)
            for e in ents:
                e["matrices"].add("standard")
            _CACHE[name] = ents
        else:
            # full = everything systematic, plus whatever smoke/standard have
            ents = _full()
            have = {e["id"] for e in ents}
            keys = {machine_key(e) for e in ents}
            for extra in (_from_specs(SMOKE, "smoke"), _from_specs(STANDARD, "standard"), hardware_entries(),
                          speed_entries("full")):
                for e in extra:
                    k = machine_key(e)
                    if e["id"] not in have and k not in keys:
                        e["matrices"] = {"full"}
                        ents.append(e)
                        have.add(e["id"])
                        keys.add(k)
            _CACHE[name] = ents
        _CACHE[name].extend(strict_policy_entries(name))
        _hostspeed_only_at_real_speed(_CACHE[name])
    return copy.deepcopy(_CACHE[name])


def all_entries():
    """Every entry of every matrix, by id, with the matrices it is in."""
    out = {}
    for n in MATRICES:
        for e in expand(n):
            if e["id"] in out:
                out[e["id"]]["matrices"] |= e["matrices"]
            else:
                out[e["id"]] = e
    return out


def find(eid):
    return all_entries().get(eid)


def chip_bytes(entry):
    return int(entry["options"]["chipmem_size"]) * 512 * 1024


def select(entries, patterns):
    """Filters entries by --config NAME (exact id, or fnmatch pattern)."""
    if not patterns:
        return entries
    return [e for e in entries if any(e["id"] == p or fnmatch.fnmatchcase(e["id"], p) for p in patterns)]


def apply_overrides(entries, sets):
    """--set key=value on every entry (recorded in entry['overrides'])."""
    if not sets:
        return entries
    for e in entries:
        e.setdefault("overrides", {})
        for k, v in sets.items():
            e["options"][k] = v
            e["overrides"][k] = v
    return entries


def option_diff(a, b):
    """Options that differ between two option dicts: [(key, a_value, b_value)]."""
    return [(k, a.get(k), b.get(k)) for k in sorted(set(a) | set(b)) if a.get(k) != b.get(k)
            and k not in TWIN_IGNORE]


# ---------------------------------------------------------------------------
# Neighbours (for narrowing): one axis changed at a time.

def _core_of(o):
    return "ce" if o.get("cycle_exact") == "true" else ("compat" if o.get("cpu_compatible") == "true" else "fast")


def neighbours(entry):
    """Variants of `entry` that change one setting: [(axis, label, entry)].

    Axes: jit (on/off); each comp_* option and the cache size (JIT on); core
    mode; CPU model; FPU native/softfloat; threaded CPU; chipset; video."""
    out = []
    o = entry["options"]
    jit_on = int(o.get("cachesize", "0") or 0) > 0

    def var(axis, label, changes):
        n = copy.deepcopy(entry)
        n["options"].update(changes)
        n["id"] = f"{entry['id']}~{axis}={label}"
        n["title"] = f"{entry['title']} [narrowing: {axis}={label}]"
        n["narrow"] = {"axis": axis, "label": label, "changes": changes, "of": entry["id"]}
        n["twin_of"], n["twin_kind"] = None, None
        n["jit"] = int(n["options"].get("cachesize", "0") or 0) > 0
        out.append((axis, label, n))

    cpu_model = o.get("cpu_model")
    mmu = o.get("mmu_model", "0")
    core = _core_of(o)
    jit_ok = cpu_model not in ("68000", "68010") and mmu == "0" and core == "fast"
    if jit_on:
        var("jit", "off", {"cachesize": "0"})
        for k, v in JIT_DEFAULTS.items():
            if k == "compfpu" and o.get("fpu_model", "0") == "0":
                continue
            cur = o.get(k, v)
            alt = {"direct": "indirect", "indirect": "direct", "true": "false", "false": "true",
                   "soft": "hard", "hard": "soft"}.get(cur)
            if alt:
                var(k, alt, {k: alt})
        for size in ("128", "16384"):
            if o.get("cachesize") != size:
                var("cachesize", size, {"cachesize": size})
    elif jit_ok:
        var("jit", "on", {"cachesize": JIT_CACHE})
    for c in CORES:
        if c == core or jit_on:
            continue
        ch = dict(CORES[c])
        ch["cpu_speed"] = "real" if (c == "ce" or (c == "compat" and cpu_model in ("68000", "68010"))) else "max"
        if o.get("cpu_threaded") == "true":
            ch["cpu_threaded"] = "false"
        var("core", c, ch)
    order = ["68000", "68010"] if cpu_model in ("68000", "68010") else ["68020", "68030", "68040", "68060"]
    if cpu_model in order:
        i = order.index(cpu_model)
        for j in (i - 1, i + 1):
            if not 0 <= j < len(order):
                continue
            nm = order[j]
            ch = {"cpu_model": nm}
            fm = o.get("fpu_model", "0")
            if fm != "0":
                ch["fpu_model"] = nm if nm in ("68040", "68060") else ("68882" if fm in ("68040", "68060") else fm)
            if mmu != "0":
                ch["mmu_model"] = nm if nm in ("68030", "68040", "68060") else "0"
            if nm == "68060":
                ch["cpu060_revision"] = o.get("cpu060_revision", "1")
            if nm in ("68040", "68060"):
                ch["cpu_24bit_addressing"] = "false"
            var("cpu_model", nm, ch)
    if o.get("fpu_model", "0") != "0":
        sf = o.get("fpu_softfloat") == "true"
        ch = {"fpu_softfloat": "false" if sf else "true"}
        if not sf and jit_on and o.get("compfpu", "true") == "true":
            ch["compfpu"] = "false"
        var("fpu_softfloat", ch["fpu_softfloat"], ch)
    if o.get("cpu_threaded") == "true":
        var("cpu_threaded", "false", {"cpu_threaded": "false"})
    elif jit_ok:
        var("cpu_threaded", "true", {"cpu_threaded": "true"})
    for chip in ("ocs", "ecs", "aga"):
        if CHIPSET_LABEL.get(o.get("chipset")) != CHIPSET_LABEL[chip]:
            var("chipset", chip, {"chipset": chip})
    ntsc = o.get("ntsc") == "true"
    var("video", "PAL" if ntsc else "NTSC", {"ntsc": "false" if ntsc else "true"})
    return out


def summary_line(options):
    """One line naming the settings that select emulator code paths."""
    o = options
    jit = int(o.get("cachesize", "0") or 0) > 0
    bits = [f"cpu_model={o.get('cpu_model')}", f"fpu_model={o.get('fpu_model')}", f"mmu_model={o.get('mmu_model')}",
            f"core={_core_of(o)}", f"chipset={o.get('chipset')}", "ntsc" if o.get("ntsc") == "true" else "pal"]
    if o.get("cpu_24bit_addressing") == "true":
        bits.append("24-bit")
    if jit:
        bits.append(f"JIT cachesize={o.get('cachesize')}")
        bits += [f"{k}={o.get(k)}" for k in JIT_DEFAULTS if o.get(k, JIT_DEFAULTS[k]) != JIT_DEFAULTS[k]]
    if o.get("cpu_threaded") == "true":
        bits.append("cpu_threaded=true")
    if o.get("fpu_softfloat") == "true":
        bits.append("fpu_softfloat=true")
    return ", ".join(bits)
