"""Amiberry configuration files, written complete and explicit.

Every option that affects what the guest sees (CPU, chipset, memory, floppy,
serial, sound, display, expansion) is written, so a run never depends on
Amiberry's built-in defaults, on amiberry.conf or on the user's files.
Option names come from Amiberry src/cfgfile.cpp (cfgfile_save_options /
cfgfile_parse_hardware_option) and src/osdep/amiberry.cpp (target options,
written with the ``amiberry.`` prefix).

`verify()` asks the binary itself (``--dump-config``) how it resolved the
file, so options Amiberry ignored or corrected (fixup_prefs) are reported.
"""
from __future__ import annotations

import os
import subprocess

# Same for every entry. Values are strings exactly as written to the file.
COMMON = {
    # file / UI
    "config_description": "amitorture generated",
    "use_gui": "no",
    "use_debugger": "false",
    # ROM extras: none
    "kickstart_ext_rom_file": "",
    "flash_file": "",
    "cart_file": "",
    "rtc_file": "",
    "maprom": "0x0",
    "kickshifter": "false",
    "ks_write_enabled": "false",
    # floppy: one DD 3.5" drive with the test disk, the others off
    "nr_floppies": "1",
    "floppy0type": "0",
    "floppy1type": "-1",
    "floppy2type": "-1",
    "floppy3type": "-1",
    "floppy1": "",
    "floppy2": "",
    "floppy3": "",
    # 100 % = real drive speed. Turbo (800) breaks raw disk-controller DMA
    # (disk.cpp doreaddma's turbo path: DSKBLK 8x too early, raw MFM reads
    # with bad checksums), so it is only an explicit per-entry or --set
    # override for entries that do not test the disk controller.
    "floppy_speed": "100",
    "floppy_volume": "0",
    "floppy0sound": "0",
    "floppy1sound": "0",
    "floppy_write_protect": "false",
    "floppy0wp": "true",
    # serial (port filled per run), parallel off
    "serial_on_demand": "false",
    "serial_hardware_ctsrts": "false",
    "serial_direct": "false",
    "serial_status": "false",
    "serial_ri": "false",
    "serial_stopbits": "0",
    "serial_translate": "disabled",
    "parallel_on_demand": "false",
    "uaeserial": "false",
    # sound: emulated exactly and fixed, so captured audio (SDL disk driver,
    # runner/amitorture/audio.py) is repeatable: 44100 Hz stereo, no
    # interpolation, no filter, full stereo separation (Paula 0/3 left,
    # 1/2 right, no crosstalk), full volume. SDL's dummy driver unless the
    # disk asks for audio marks.
    "sound_output": "exact",
    "sound_channels": "stereo",
    "sound_stereo_separation": "10",
    "sound_stereo_mixing_delay": "0",
    "sound_frequency": "44100",
    "sound_interpol": "none",
    "sound_filter": "off",
    "sound_filter_type": "standard",
    "sound_volume": "0",
    "sound_volume_paula": "0",
    "sound_auto": "false",
    "sound_max_buff": "8192",
    # display: fixed, unscaled, no vsync, every frame drawn (no frame skip)
    "gfx_framerate": "1",
    "gfx_width": "720",
    "gfx_height": "568",
    "gfx_width_windowed": "720",
    "gfx_height_windowed": "568",
    "gfx_fullscreen_amiga": "false",
    "gfx_fullscreen_picasso": "false",
    "gfx_vsync": "false",
    "gfx_vsync_picasso": "false",
    "gfx_resolution": "hires",
    "gfx_linemode": "double2",
    "gfx_lores_mode": "normal",
    "gfx_center_horizontal": "none",
    "gfx_center_vertical": "none",
    "gfx_autoresolution": "0",
    "gfx_colour_mode": "32bit",
    "gfx_flickerfixer": "false",
    "show_leds": "false",
    "immediate_blits": "false",
    "waiting_blits": "disabled",
    "collision_level": "full",
    "display_optimizations": "none",
    "genlock": "false",
    # chipset extras. rtc, ksmirror_*, cia_overlay, fatgary, ramsey, ... are
    # NOT written: chipset_compatible (per entry) sets them the way the real
    # model has them (built_in_chipset_prefs), and --dump-config records it.
    "rndseed": "1",
    # memory not used by any entry (entries set chip/slow/fast/z3/mbres)
    "z3mem2_size": "0",
    "megachipmem_size": "0",
    "mbresmem_size": "0",
    "mem25bit_size": "0",
    "debugmem_size": "0",
    "gfxcard_size": "0",
    "gfxcard_type": "",
    "cpuboard_type": "none",
    "cpuboardmem1_size": "0",
    "cpuboardmem2_size": "0",
    "fastmem2_size": "0",
    # expansions off
    "scsi": "false",
    "sana2": "false",
    "bsdsocket_emu": "false",
    "boot_rom_uae": "disabled",
    "uae_hide_autoconfig": "false",
    "filesys_no_fsdb": "false",
    "synchronize_clock": "false",
    "clipboard_sharing": "false",
    "native_code": "false",
    "magic_mouse": "false",
    # CPU details shared by all entries
    "cpu_throttle": "0.0",
    "cpu_data_cache": "false",
    "cpu_no_unimplemented": "false",
    "fpu_no_unimplemented": "false",
    "fpu_strict": "false",
    "fpu_softfloat": "false",
    "cpu_halt_auto_reset": "false",
    # JIT tuning (only active when cachesize > 0; written for all so that a
    # JIT twin differs from its entry by cachesize alone)
    "compfpu": "true",
    "comp_trustbyte": "direct",
    "comp_trustword": "direct",
    "comp_trustlong": "direct",
    "comp_trustnaddr": "direct",
    "comp_nf": "true",
    "comp_constjump": "true",
    "comp_flushmode": "soft",
    "comp_catchfault": "true",
    # input: nothing attached (nothing should read the user's devices)
    "joyport0": "mouse",
    "joyport1": "none",
    "joyport2": "none",
    "joyport3": "none",
    "kbd_lang": "us",
    # host mouse -> Amiga counts 1:1 (SEND_MOUSE dx/dy are scaled by the
    # speed), no absolute pointer (tablet/mousehack)
    "input.mouse_speed": "100",
    "absolute_mouse": "none",
    "tablet_library": "false",
    # host (Amiberry target options)
    "amiberry.ipc_disable_logging": "true",
    "amiberry.start_minimized": "false",
}

# Options Amiberry may legitimately rewrite in --dump-config without that
# meaning the guest machine differs (paths, file names, host bits).
VERIFY_IGNORE = {
    "config_description", "floppy0", "kickstart_rom_file", "kickstart_ext_rom_file", "serial_port",
    "amiberry.rom_path", "amiberry.floppy_path", "amiberry.hardfile_path", "amiberry.cd_path",
    "cdimage0", "flash_file",
}

# Options --dump-config writes only when they differ from the default
# (cfgfile_dwrite*): when one is missing from the dump, Amiberry resolved it
# to this default. Used to see e.g. cpu_threaded=true silently reset.
DWRITE_DEFAULTS = {
    "cpu_threaded": "false", "fpu_softfloat": "false", "mmu_model": "0", "fpu_model": "0",
    "debugging_features": "", "cpu_data_cache": "false", "fpu_strict": "false",
    "cpu_no_unimplemented": "false", "fpu_no_unimplemented": "false",
}

# Which fixup_cpu()/fixup_prefs() message (main.cpp) explains which option.
FIXUP_HINTS = {
    "cpu_threaded": ("Threaded CPU",),
    "mmu_model": ("MMU",),
    "cachesize": ("JIT",),
    "fpu_model": ("FPU",),
    "fpu_softfloat": ("softfloat",),
    "cpu_24bit_addressing": ("24-bit",),
    "cpu_cycle_exact": ("ycle-exact", "ycle exact"),
    "cpu_memory_cycle_exact": ("ycle-exact", "ycle exact"),
    "blitter_cycle_exact": ("litter",),
    "cycle_exact": ("ycle-exact", "ycle exact"),
    "cpu_compatible": ("ompatible",),
    "cpu_data_cache": ("Data cache",),
    "compfpu": ("FPU",),
    "waiting_blits": ("blit",),
}


def build(entry, *, rom_path, floppy, serial_port, media_dir, ext_rom_path=None, extra=None):
    """Returns (ordered dict of options, text of the .uae file).

    `extra`: options added last (test media from media.py, the GDB server
    for repro packs, ...)."""
    opts = {"config_version": "8.5.0"}
    opts.update(COMMON)
    opts.update(entry["options"])
    opts["kickstart_rom_file"] = rom_path
    opts["kickstart_ext_rom_file"] = ext_rom_path or ""
    opts["floppy0"] = floppy or ""
    if not floppy:
        opts["floppy0type"] = "0"
    opts["serial_port"] = f"TCP://127.0.0.1:{serial_port}/wait" if serial_port else "none"
    opts["amiberry.rom_path"] = os.path.join(media_dir, "roms") + os.sep
    opts["amiberry.floppy_path"] = media_dir + os.sep
    opts["amiberry.hardfile_path"] = media_dir + os.sep
    opts["amiberry.cd_path"] = media_dir + os.sep
    if extra:
        opts.update(extra)
    # No comment lines: Amiberry's parser reports ';' lines as unknown entries.
    opts["config_description"] = f"amitorture {entry['id']}: {entry['title']}"
    # "key#tag" lets an entry write an option that may appear several times
    # (hardfile2, ...) next to the same key from elsewhere: written as key=.
    lines = [f"{k.split('#', 1)[0]}={v}" for k, v in opts.items()]
    return opts, "\n".join(lines) + "\n"


def parse(text):
    out = {}
    for line in text.splitlines():
        if not line or line.startswith(";") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        out[k.strip()] = v.strip()
    return out


def _norm(v):
    v = v.strip().lower()
    return {"yes": "true", "no": "false", "1": "1"}.get(v, v)


def verify(binary, config_path, env, timeout=60):
    """Runs ``amiberry --config FILE --dump-config`` in the isolated env.

    Returns dict(ok, resolved_text, differences=[{option, wrote, resolved}], stderr).
    `resolved` is None when Amiberry did not write the option back (dwrite
    options are only written when they differ from the default, so a missing
    option is reported only if what we wrote is not the default).
    """
    try:
        from .emulator import run_amiberry   # one machine-wide emulator slot (tools/amislot.py)
        p = run_amiberry([binary, "--config", config_path, "--dump-config"], env=env, capture_output=True,
                         timeout=timeout, stdin=subprocess.DEVNULL)
    except (OSError, subprocess.TimeoutExpired) as e:
        return {"ok": False, "error": str(e), "differences": [], "resolved_text": ""}
    text = p.stdout.decode(errors="replace")
    resolved = parse(text)
    with open(config_path, encoding="utf-8") as fh:
        wrote = parse(fh.read())
    err = p.stderr.decode(errors="replace")
    unknown, notes, in_unknown = [], [], False
    for line in err.splitlines():
        if line.startswith("; unknown configuration entries"):
            in_unknown = True
            continue
        if in_unknown and line.strip():
            unknown.append(line.strip())
        elif line.strip() and not line.startswith(";"):
            notes.append(line.strip())   # e.g. fixup_prefs messages
    diffs = differences(wrote, resolved, notes)
    return {"ok": p.returncode == 0 and not unknown, "returncode": p.returncode, "resolved_text": text,
            "resolved": resolved, "differences": diffs, "unknown_options": unknown, "fixup_messages": notes,
            "stderr": err[-4000:]}


def differences(wrote, resolved, notes=()):
    """[{option, wrote, resolved, because}] for every option Amiberry changed.

    `resolved` None means "not in the dump" for a DWRITE option, i.e. reset
    to its default (shown as resolved="<default>")."""
    diffs = []
    for k, v in wrote.items():
        if k in VERIFY_IGNORE or k.startswith("config_"):
            continue
        if k in resolved:
            got = resolved[k]
        elif k in DWRITE_DEFAULTS:
            got = DWRITE_DEFAULTS[k]
        else:
            continue
        if _norm(got) != _norm(v):
            if k.endswith("rom_file") and v and got and os.path.basename(str(v)) == os.path.basename(str(got)):
                continue    # Amiberry writes a board ROM back as its file name: the same file
            hints = FIXUP_HINTS.get(k, ())
            because = [n for n in notes if any(h in n for h in hints)]
            diffs.append({"option": k, "wrote": v, "resolved": got if k in resolved else f"{got} (absent: default)",
                          "because": because})
    return diffs
