# amitorture output schema (v1)

This document is for whoever reads amitorture output, human or AI. It
describes every record the test disk emits (CONTRACT.md §5, plus the
`hello.probe` object of §8) and every file the host runner writes. The
contract is authoritative for the wire format; this file explains meaning.

---------------------------------------------------------------------------
## 1. Records (what the guest says)

### 1.1 Wire format

One record per line:

```
{"v":1,"seq":3,"t":"mod","mod":"takeover","ev":"end","pass":1,"fail":0,"skip":0} *fc7b5d9d
```

- Before ` *` is a JSON object (ASCII, no newlines). After it are 8 lowercase
  hex digits: the CRC-32 (zlib/IEEE) of the exact JSON bytes. The runner
  recomputes it, and **a line with a bad CRC is never trusted**: it is
  reported as corrupt, and its seq can only be filled from the other
  transport.
- Lines end in `\n`. A `\r` before it is tolerated.
- Every record has `v` (schema version, 1), `seq` (0, 1, 2, ... per boot)
  and `t` (type).
- Integers are decimal. Register-like values are hex strings without `0x`
  (`"pc":"0001a2c4"`), zero-padded to their natural width.
- The same lines go to both transports, in the same order: serial (Paula
  UART) and the chip-RAM mailbox ring. The runner merges them by
  `(boot, seq)`. `seq` restarting at 0 means a new boot.
- Text that is not a record (e.g. the AROS ROM's `ROMInfo: ...` lines on
  serial before the disk boots) is counted as noise and otherwise ignored.

### 1.2 `hello`: first record after the probe

| Field | Type | Meaning |
|---|---|---|
| `build` | string | build id of the disk (git rev + date) |
| `mode` | int | 0 full, 1 quick (run parameter, CONTRACT §4; 2 soak is reserved, not implemented) |
| `seed` | int | random seed used by the tests |
| `exec` | "ver.rev" | exec.library version.revision as the OS reports it |
| `cpu` | string | CPU found by the probe (same as `probe.cpu`) |
| `fpu` | string | FPU found by the probe (same as `probe.fpu`) |
| `mmu` | 0/1 | MMU found by the probe |
| `probe` | object | everything the probe found, see 1.3 |

### 1.3 `hello.probe`: what the hardware looks like from inside

The probe runs with exec alive, before the disk takes over the machine. It
decides each value with architectural tests (trapping instructions, register
read-back) rather than trusting exec, and lists in `how` which test decided
what. A difference between `probe` and the emulator config is itself a
finding (the runner reports it as `probe_mismatch`).

| Field | Type | Meaning |
|---|---|---|
| `cpu` | string | `68000`, `68010`, `68EC020`, `68020`, `68EC030`, `68030`, `68EC040`, `68LC040`, `68040`, `68EC060`, `68LC060`, `68060` |
| `cpu_rev` | int | 68060 PCR revision, else -1 |
| `fpu` | string | `none`, `68881`, `68882`, `68040`, `68060` |
| `fpu_frame` | hex | first long of the FSAVE idle frame (68881/2: version<<24 \| size<<16), 0 without FPU |
| `mmu` | 0/1 | an MMU answered (PMOVE / MOVEC TC/URP worked) |
| `addr24` | 0/1 | 24-bit address bus (68000/010/EC020; for others decided from memory above 16 MB; `how` says when it was only assumed) |
| `vbr` | hex | vector base register (0 on 68000) |
| `cacr` | hex | cache control register as found (0 on 68000/010) |
| `how` | [string] | short notes of the tests that decided each value, e.g. `"movec-cacr:ok cacr=00000001"`, `"addr24:no-highmem assume 24"`, `"fsave:41000000"`, `"agnus:23 alice pal"` |
| `agnus_id` | hex | VPOSR bits 14-8 (Agnus/Alice id) |
| `denise_id` | hex | DENISEID ($DFF07C) as read; `ffff` if unstable (OCS Denise has no id) |
| `chipset` | string | `OCS`, `ECS` or `AGA` |
| `ecs_agnus`, `ecs_denise` | 0/1 | ECS (or AGA) Agnus / Denise found |
| `video` | string | `PAL` or `NTSC` |
| `exec_attn` | hex | SysBase->AttnFlags |
| `kick` | "ver.rev" | Kickstart (exec) version and SoftVer |
| `rom_base` | hex | where the ROM is mapped |
| `mem` | [object] | exec's memory list: `{"name", "lower", "upper", "attr" (hex MEMF_ flags), "free" (bytes)}` |
| `boards` | [object] | autoconfig boards from expansion.library: `{"man", "prod", "addr" (hex), "size"}` |

Example (abbreviated):

```json
{"v":1,"seq":0,"t":"hello","build":"nogit 2026-11-03","mode":0,"seed":1,"exec":"40.10","cpu":"68040","fpu":"68040","mmu":1,
 "probe":{"cpu":"68040","cpu_rev":-1,"fpu":"68040","fpu_frame":"41000000","mmu":1,"addr24":0,"vbr":"00000000","cacr":"00008000",
  "how":["attn:804f","movec-vbr:ok vbr=00000000","movec-cacr:ok cacr=00008000","fsave:41000000","tc:p-rw urp:rw","addr24:32-bit cpu","agnus:23 alice pal","denise:fcf8 lisa"],
  "agnus_id":"0023","denise_id":"fcf8","chipset":"AGA","ecs_agnus":1,"ecs_denise":1,"video":"PAL","exec_attn":"804f","kick":"40.68","rom_base":"00f80000",
  "mem":[{"name":"expansion memory","lower":"08000020","upper":"08800000","attr":"0505","free":8313192},
         {"name":"chip memory","lower":"00001020","upper":"00200000","attr":"0703","free":2067224}],
  "boards":[]}} *dadeb270
```

### 1.4 `log`: free text

| Field | Type | Meaning |
|---|---|---|
| `msg` | string | a note from the disk (progress, a detail a test wants to keep) |

```
{"v":1,"seq":7,"t":"log","msg":"blitter: 64 random fills"} *xxxxxxxx
```

### 1.5 `mod`: module boundary

| Field | Type | Meaning |
|---|---|---|
| `mod` | string | module name, e.g. `takeover`, `cpu`, `blitter` |
| `ev` | `begin` / `end` | |
| `pass`, `fail`, `skip` | int | on `end` only: case counts of this module |

```
{"v":1,"seq":1,"t":"mod","mod":"takeover","ev":"begin"} *f9f071af
{"v":1,"seq":3,"t":"mod","mod":"takeover","ev":"end","pass":1,"fail":0,"skip":0} *fc7b5d9d
```

### 1.6 `case`: one test result (the important one)

| Field | Type | Meaning |
|---|---|---|
| `mod` | string | module |
| `case` | string | case name, unique within the module |
| `st` | string | `pass`, `fail`, `skip` (not run, e.g. needs another CPU), `untestable` (the test could not decide, e.g. timing too noisy) |
| `expect` | any | on fail: what real hardware does (value or object) |
| `got` | any | on fail: what this machine did (same shape as `expect`) |
| `why` | string | on fail: one sentence saying what is wrong |
| `ctx` | object | optional: machine state that helps diagnosis (registers, addresses) |
| `in` | any | optional: the inputs of the case (operands, seed-derived values), enough to re-run it |

```
{"v":1,"seq":2,"t":"case","mod":"takeover","case":"takeover_restore","st":"pass","ctx":{"vbr":"00000000","intena":"202c","dmacon":"03d0","adkcon":"1100"}} *7c3cfc78
{"v":1,"seq":9,"t":"case","mod":"cpu","case":"divu_overflow","st":"fail","expect":{"sr":"0002"},"got":{"sr":"0000"},"why":"V flag not set on DIVU overflow","in":{"d0":"ffffffff","d1":"00000001"},"ctx":{"pc":"0001a2c4"}} *xxxxxxxx
```

### 1.7 `capture`: host capture request (best effort)

| Field | Type | Meaning |
|---|---|---|
| `what` | string | `screen`, `audio_start`, `audio_stop` |
| `tag` | string | name for the capture, used in file names |

```
{"v":1,"seq":20,"t":"capture","what":"screen","tag":"copper_bars"} *xxxxxxxx
```

(Milestone 1 runner: recorded, not acted on yet.)

### 1.8 `perf`: throughput

| Field | Type | Meaning |
|---|---|---|
| `mod` | string | module |
| `ops` | int | operations done |
| `ticks` | int | CIA E-clock ticks taken (709379 Hz PAL, 715909 Hz NTSC) |

```
{"v":1,"seq":30,"t":"perf","mod":"blitter","ops":4096,"ticks":35210} *xxxxxxxx
```

### 1.9 `done`: last record of a boot

| Field | Type | Meaning |
|---|---|---|
| `pass`, `fail`, `skip`, `untestable` | int | totals over all modules |
| `aborted` | int | modules that did not finish (non-zero means the totals are partial) |
| `records` | int | records sent this boot, **including** `done` itself; the runner checks it against what arrived |

```
{"v":1,"seq":4,"t":"done","pass":1,"fail":0,"skip":0,"untestable":0,"aborted":0,"records":5} *7b717812
```

### 1.10 `speed`: a hostspeed timed block (not a case)

The hostspeed module (docs/cases/hostspeed.md) runs one block per instruction class for about 1 s of
emulated time and brackets it with two records; the runner's verdict on it is the pseudo case
`host.speed_<insn>` (5.1). Skipped classes get one `skip` record.

| Field | Type | Meaning |
|---|---|---|
| `mod` | string | `hostspeed` |
| `ev` | `begin` / `end` / `skip` | before the block, after it, or not run on this CPU/FPU |
| `insn` | string | the block: `add_l`, `fsin`, ... (`stop`, `empty`, `empty_end` are the references) |
| `cls` | `int` / `fpu` | the class (its cost reference: `add_l` or `fadd`) |
| `pos` | int | the mailbox case number (offset 40) while the block runs: 1000 + block index; module (offset 36) is 26 |
| `n` | int | `end`: instructions executed (`stop`: STOPs = frames; `empty`: DBF iterations) |
| `timing` | object | `end`: `cck` emulated colour clocks of the block, `empty_cck` the loop alone for the same units, `frames` VBLs counted, `units` outer loop count |
| `why` | string | `skip`: why it does not run here |

```
{"v":1,"seq":2,"t":"speed","mod":"hostspeed","ev":"begin","insn":"fsin","cls":"fpu","pos":1031} *xxxxxxxx
{"v":1,"seq":3,"t":"speed","mod":"hostspeed","ev":"end","insn":"fsin","cls":"fpu","pos":1031,"n":3465216,"timing":{"cck":3537250,"empty_cck":69522,"frames":50,"units":3384}} *xxxxxxxx
```

`n` and `timing` depend on the engine's emulated timing (the JIT charges cycles per block), so they
differ between a JIT twin and its reference. No fingerprint or parity check looks at `speed` records.

---------------------------------------------------------------------------
## 2. Mailbox (how the host reads the records out of memory)

A 48-byte header plus a ring in chip RAM (CONTRACT §7), found by scanning
chip RAM in 16-byte steps for `AMITORTURE-MBOX!`, then accepted only if the
header is plausible (version 1, a known state, ring inside chip RAM).
`state`: 0 init, 1 running, 2 done, 3 aborted, 4 halted-after-done (the
machine stays alive after `done`). `heartbeat` changing tells running from
hung. `total` (bytes ever written) gives the new bytes and overruns: if more
than `ring_size` bytes were written since the last read, the oldest are lost
and reported as such.

---------------------------------------------------------------------------
## 3. Result files (what the runner writes)

`run` writes `WORKDIR/results/<run-id>/` (run id = UTC time,
`YYYYMMDD-HHMMSSZ`); `import` writes the same layout.

### 3.1 `REPORT.md`: start here

Sections (milestone 2): verdict line; **Coverage** (modules × entries with
pass/fail/skip/untestable/xfail/engine_mismatch/unstable counts and where a
module failed or was missing; one row per entry: machine, ROM, engine/twin,
status, as designed, counts, why; skipped entries and why; configs adjusted
by the emulator; engine parity per twin pair; xfail/fixed); **Problems**
(grouped by module + narrowing signature; each with title, scope, the
representative failure in full including the docs/cases intent/rule line,
the narrowing result, code pointers, the repro pack, history, then every
other failure); Notes (low severity); Environment; Transports and process;
Files. docs/RUNNER.md describes the layout in detail.

### 3.2 `summary.json`

```json
{
 "run_id": "20261103-110351Z", "source": "run",
 "verdict": "PASS | PROBLEMS (...) | ISOLATION VIOLATED | NOTHING RAN, prefixed INTERRUPTED (...): after Ctrl+C",
 "interrupted": null,
 "status_counts": {"pass": 9, "skip": 1},
 "entries": [{"id": "a1200-jit", "title": "...", "status": "pass", "reason": "done, no failures",
              "jit": true, "twin_of": "a1200", "machine": {"cpu": "68020", "fpu": "none", "chipset": "AGA"},
              "rom": "KS ROM v3.1 (A1200)", "cases": {"pass": 1, "fail": 0, "skip": 0, "untestable": 0},
              "records": 5, "duration_s": 4.9, "ended": "done"}],
 "problems": [{"id": "P01", "title": "case cpu/divu_overflow fails", "kind": "case_fail", "severity": "normal",
               "entries": ["a1200-jit"], "passes_on": ["a1200"], "pattern": "JIT only: ...", "failures": ["F001"]}],
 "isolation": {"ok": true, "changes": [], "user_amiberry_seen": []},
 "files": {"report": "REPORT.md", "failures": "failures.jsonl", "environment": "environment.json",
           "configs": "configs/", "raw": "raw/"}
}
```

Entry `status`:

| status | meaning |
|---|---|
| `pass` | `done` arrived, no failed case, records complete and consistent |
| `fail` | at least one `case` with `st:"fail"` (or `done.fail` > 0) |
| `crash` | the emulator process exited by itself (exit code / signal in `raw/<entry>/result.json`) |
| `hang` | the mailbox heartbeat (or, without a mailbox, the serial output) stopped for `--hang` seconds before `done` |
| `timeout` | still running at `--timeout` without `done` |
| `incomplete` | the run ended but records are missing (seq gaps), corrupt, inconsistent, or `done.aborted` > 0 |
| `error` | a host-side problem (no IPC endpoint, ...), not a finding about the emulator |
| `skip` | not run; `reason` says why (usually a missing ROM) |

`ended` says how observation stopped: `done`, `hang`, `timeout`, `crash`,
`error`, `selftest`.

Problem `kind` / failure `kind`:

| kind | meaning |
|---|---|
| `case_fail` | a failed case (the guest's own verdict), or a failed host pseudo case `host.tone_<tag>` (`mod` `host`, section 5.1) |
| `probe_mismatch` | the probe saw a different CPU/FPU/chipset than the config asked for (`confidence` low when the probe only assumed it, or the config explains it) |
| `crash`, `hang`, `timeout`, `incomplete`, `error` | the entry did not finish cleanly |
| `records_missing` | seq range that arrived on no transport |
| `records_corrupt` | lines with bad CRC / unparsable JSON |
| `records_conflict` | serial and mailbox disagree on the content of a seq |
| `schema` | a CRC-valid record that breaks this schema (a disk bug) |
| `config_not_applied` | Amiberry resolved an option differently from what the config wrote (`--dump-config`), or did not know it (low severity: the entry is also listed under "Configs adjusted by the emulator" and has `as_designed: false`) |
| `engine_mismatch` | a twin (JIT or threaded CPU) reported a case differently from its reference entry, or one of them lacks the case (milestone 2, §4.1) |
| `fpu_mode_difference` | a softfloat twin differs from its native-FPU reference (informational: native is 64-bit double on this host) |

`pattern` is a hint from comparing failing and passing entries: "JIT only"
(every failing entry is a `-jit` twin whose base entry passes),
"interpreter only", "only cpu=...", "only chipset=...", "only with ROM ...".

### 3.3 `failures.jsonl`

One JSON object per line, self-contained. Common fields: `id` (`F001` ...),
`kind`, `entry`, `jit`, `twin_of`, `machine`, `rom`, `config` (path of the
config in `configs/`), `why`. Per kind:

- `case_fail`: `mod`, `case`, `expect`, `got`, `why`, `ctx`, `in`, `seq`,
  `boot`, `sources` (which transports delivered it), `record` (the whole
  record), `preceding` (up to 6 records before it), `hello` (summary of the
  guest: build, exec, cpu, fpu, mmu, chipset, video, kick, chip ids).
- `probe_mismatch`: `what` (cpu/fpu/chipset), `configured`, `probe`,
  `confidence`, `note`, `hello`.
- `crash` / `hang` / `timeout` / `incomplete` / `error`: `exit` (returncode,
  signal), `mailbox` (last header: state, heartbeat, current module/case),
  `hang`, `last_records`, `log_tail` (last emulator log lines).
- `records_missing`: `boot`, `from`, `to`, `count`.
- `records_corrupt`: `count`, `samples`. `records_conflict`: `boot`, `seq`,
  `kept`, `other`. `schema`: `problems`, `rec`. `config_not_applied`:
  `differences` (`option`, `wrote`, `resolved`), `unknown_options`.

### 3.4 `environment.json`

`run_id`; `amiberry` (`path`, `sha256`, `size`, `mtime`, `version`,
`version_output`); `host` (`os`, `release`, `machine`, `distro`, `board`,
`python`, `cpu_count`); `amitorture` (`rev`, `dirty`); `kickstarts_dir`;
`roms` (every file in it: `file`, `status` = `ok` / `byteswapped` /
`needs rom.key` / `unknown kickstart` / `unknown`, `crc32`, `sha1`,
`sha256_file`, `rom` = the romdb entry it matched: `id`, `name`, `category`,
`version`, `exec`, `models`; `header` for unknown Kickstarts; `encrypted`,
`key`, `doubled`); `disk_source` (ADF header: build id, sizes, blob CRC,
params); `params` (run parameters baked into the disk); `options`;
`isolation` (`ok`, `watched` dirs, `changes`, `user_amiberry_seen`: Amiberry processes seen on the user's real
HOME during the run, `pid`, `cmd`, `first_seen`, `last_seen`); `interrupted` after Ctrl+C (`during`:
`entries` or `analysis`, `finished`, `selected`, `not_run`: entry ids; the same object is in `summary.json`).

### 3.5 `configs/`

`<entry>.uae`: the complete Amiberry config as written (every option that
affects the guest is set explicitly). `<entry>.resolved.uae`: the same file
as Amiberry resolved it (`amiberry --config FILE --dump-config`, after its
fixups); options derived from `chipset_compatible` (RTC, Gary, ROM mirrors)
are visible there.

### 3.6 `raw/<entry>/`

| File | Content |
|---|---|
| `amiberry.log` | the emulator's console output (`--log`), each line prefixed with seconds since launch |
| `serial.bin` | the raw serial byte stream (including non-record noise) |
| `mailbox.jsonl` | one event per line: `{"event":"scan","range","reads","seconds","found"}`, `{"event":"data","header",lost","lost_at","data_b64"}` (new ring bytes, base64), `{"event":"vanished"}` |
| `records.jsonl` | the merged records: `{"boot","seq","rec","sources"}` |
| `result.json` | everything the runner knows about the entry: options, ROM, disk, config check, command line and environment, IPC endpoint and request count, serial and mailbox statistics, process exit and log tail, record statistics (gaps, bad lines, conflicts, noise), status |
| `attemptN.*` | logs of launches that failed at SDL start-up and were retried |

`serial.bin` and `mailbox.jsonl` can be fed back to `python3 -m amitorture
import` to rebuild the report; captures from real hardware or an FPGA use
the same command (a serial capture is just the bytes).

---------------------------------------------------------------------------
## 4. Milestone-2 additions

### 4.1 Fingerprints, case keys, timing fields

- **Case key:** `<mod>.<case>`. If an id repeats within one boot, the copies are `<mod>.<case>#2`, `#3`, ...
  The pseudo case `probe.hello` is the hello record (without `build`) and covers the probe result.
- **Fingerprint:** the case record minus `v`, `seq` and `t`, and minus every **timing field**: any key named
  `timing` or `ticks`, or starting with `latency`, at any depth. Serialised canonically (sorted keys).
- Timing fields are kept apart by path (e.g. `ctx.latency_ticks`) and compared with bands. `perf` records are
  never part of a fingerprint. Their value is `ticks/ops`, keyed `<mod>.<name>`.
- Fingerprints are what JIT parity, baselines and narrowing compare. A module that wants a value compared with
  tolerance instead of equality must put it under such a key.

- **Excluded markers.** Keys starting with `undef_` (e.g. `undef_observed`, `undef_changed_at`), at any depth, are also not part of the
  fingerprint. They record which of several architecturally allowed results happened, e.g. smc without a cache
  flush. Extraction keeps them as `excluded` (`{path: value}`), they stay in the record and in the report, and a
  parity item shows them as `not_compared_fields`.

### 4.1a Disk sets

- **Per-disk results.** With a disk set, `raw/<entry>/result.json` has a `disks` list with one result per disk
  (same fields as a single-disk result, plus `disk_name` and `modules` `{mod: id}`). Each disk's files are in
  `raw/<entry>/<disk>/`.
- **Entry-level fields:**
  - `status`: the worst over the disks;
  - `reason`: the disks that did not pass;
  - `disk`: `{"set": [{name, path, build_id, sha256, params}]}`;
  - `records`: `{merged, per_disk, gaps, bad_count, hello, done: {disk: done}}`;
  - `probe_consistency`: `{ok, compared, reference_disk, differences: [{disk, path, reference, got}]}`.
- **Fingerprints.** A case fingerprint has `disk`; case keys stay `<mod>.<case>`.
- **Failure items** from one disk (case_fail, crash/hang/timeout/incomplete, records_*) have `disk`.
- **New failure kind** `probe_inconsistent` (`reference_disk`, `differences`): two disks of the same entry saw
  different machines.
- **environment.json.** `disk_source` is `{set, path, build_id ("b1 | b2 | b3"), disks: [header info + name]}`.
- **Baselines.** `meta.disk.set` names the set dir, and `check` reuses it.

### 4.2 summary.json (added fields)

- `entries[]`:
  - `twin_kind` (`jit` / `threaded` / `softfloat` / null);
  - `as_designed` (false when `--dump-config` showed Amiberry changed an option);
  - `adjusted` (`[{option, wrote, resolved, because}]`);
  - `cases` with `xfail`, `engine_mismatch`, `unstable` counts.
- `modules`: `{mod: {id, cases, entries, pass, fail, skip, untestable, xfail, engine_mismatch, unstable,
  failing_on, missing_on}}`.
- `parity`: `[{ref, twin, twin_kind, kind, status (compared|not compared), compared, mismatches, why}]`.
- `xfail` / `fixed`: `[{entry, case_key, st, ref}]`.
- `problems[]`:
  - `mod`, `cases`, `case_keys`, `representative` (failure id);
  - `signature` (grouping key);
  - `narrowing` (the statement);
  - `pointers` (`[{file, function, line, why, confidence}]`);
  - `repro` (`{dir, files, run}`);
  - `history`.
- `narrowing`: `{budget_s, used_s, launches, exhausted}`.

### 4.3 failures.jsonl (added kinds and fields)

- `case_fail` items also have:
  - `case_key`;
  - `xfail`, when the case is a known bug. An xfail item is not part of any problem.
- `engine_mismatch` / `fpu_mode_difference` have:
  - `entry` (the twin), `ref_entry`, `twin_kind`, `case_key`, `mod`, `case`;
  - `subkind`: `value` or `missing_case`;
  - `ref`, `twin`: the two fingerprinted records;
  - `paths`: `[{path, ref, twin}]`;
  - `differing_options`: `[{option, ref, twin}]`;
  - `why`.

### 4.4 raw/<entry>/result.json (added fields)

- `resolved_options`: options as Amiberry resolved them.
- `engine`: what the emulator log says ran: `{line, cpu_model, fpu_model, fpu_mode, mmu_model, jit_kind,
  cachesize, mode, cpu_thread_started, cpu_thread_refused, jit_refused}`. Differences from the config end up in
  `adjusted` with `"source": "emulator log"`.
- `adjusted`.
- `twin_kind`, `twin_options`, `overrides` (`--set`), `params` (per-entry run parameters, e.g. the narrowing
  module mask), `narrow` (for narrowing launches: `{axis, label, changes, of}`).
- `media`: what media.py returned, or why there is no test media.
- `ext_rom`.
- `emulator_slot`, `slot_wait_s`: the machine-wide emulator slot (tools/amislot.py) and how long the run waited
  for it.
- `log_counts`: JIT fallback/flush/fault and illegal-instruction lines counted in the emulator log.
- `binary`.

Entries whose id contains `~` are narrowing or repro launches (`<entry>~<axis>=<label>`, `<entry>~rerun=N`). Their
raw files use `_` in place of `~` and `-` in place of `=`.

### 4.5 narrowing.json

```json
{"budget_s": 1800, "used_s": 412.0, "launches": 23, "exhausted": false,
 "groups": [{"entry": "a4000-68040nommu-fast-jit", "mod": "cpu_basic", "module_id": 2,
             "launches": [{"axis": "rerun", "label": "1", "entry": "...~rerun=1", "status": "fail", "seconds": 14.2}],
             "not_tried": [], "isolation_note": null,
             "cases": {"cpu_basic.addx_l": {"case_key": "cpu_basic.addx_l", "kind": "case_fail",
                 "determinism": "deterministic", "fail_rate": "3/3", "reruns": ["bad", "bad", "bad"],
                 "flips": [{"axis": "jit", "label": "off", "changes": {"cachesize": "0"}}],
                 "holds": [{"axis": "cpu_model", "label": "68060", "changes": {}}],
                 "inconclusive": [{"axis": "chipset", "label": "ocs", "changes": {}, "why": "run failed"}],
                 "signature": ["comp_nf=false", "jit=off"],
                 "statement": "fails only with JIT on and comp_nf=true; passes with jit=off, comp_nf=false; ..."}}}]}
```
- A launch entry with `"reused": "<entry id>"` was not launched: the variant's options equal that entry of the
  main run, whose result was used.
- Outcomes: `bad` means the case failed (`case_fail`) or still differs from the reference (`engine_mismatch`).
  `good` means it passed or matched. Anything else is `inconclusive:<why>`.
- `determinism`: `deterministic`, `intermittent`, `not reproduced`.

### 4.6 repro/<problem>/

| file | content |
|---|---|
| `case.adf` | the test disk with only the module enabled (`modules=<1<<id>`) and the run's seed/mode |
| `config.uae` | standalone config: absolute ROM path, case.adf in DF0, serial `TCP://127.0.0.1:23598` (no /wait), `debugging_features=gdbserver gdb_port=23597` (not with cpu_threaded) |
| `pack.json` | problem, case key, kind, module id, params, entry (all options), ROM/ext ROM, binary, expect/got, reference record (engine mismatch) |
| `run.sh` | reruns it via `amitorture repro` and prints the case record; exit 0 = passes / matches the reference |
| `gdb.cmd` | m68k-amigaos-gdb: target remote to the GDB server, break at the case's `ctx.pc` if it gave one |
| `host-gdb.cmd` | host gdb: isolated environment, breakpoints on the suggested emulator functions, run |

### 4.7 Baselines: WORKDIR/baselines/<name>/

`baseline.json`:
```json
{"format": 1,
 "meta": {"name": "...", "created": "...", "repeat": 3, "matrix": "smoke", "configs": null, "sets": {},
          "params": {...}, "binary": {"path", "sha256", "version"}, "disk": {"path", "build_id", "sha256"},
          "kickstarts": "...", "roms": {"<entry>": {"rom", "rom_name", "rom_crc32", "ext_rom"}}, "unstable_cases": 0},
 "entries": {"<entry>": {"entry": {...full entry incl. options...}, "statuses": ["pass", "pass", "pass"],
   "cases": {"<case key>": {"mod", "case", "stable": true, "present": "3/3", "st": ["pass", ...], "fp": "...",
                            "rec": {...}, "variants": ["..."]}},
   "timing": {"<case key>": {"<path>": {"n", "mean", "sd", "lo", "hi", "values", "k", "pct", "abs_min"}}},
   "perf": {"<mod>.<name>": {"n", "mean", "sd", "lo", "hi", "values", "ops": [...], "unit": "ticks/op"}},
   "meta": {"rom", "rom_name", "rom_crc32", "ext_rom", "disk_build", "binary_sha256", "adjusted", "resolved_options"},
   "runs_raw": [{"status", "reason", "adjusted", "raw"}]}}}
```
- Bands are mean ± max(k·σ, pct·|mean|, abs_min), with defaults k=3, pct=5 %, abs_min=1.
- `fp` is null when the case is unstable; `variants` then lists the fingerprints seen.
- `BASELINE.md` is the summary; `runs/run<N>/` holds the full result dirs.

### 4.8 check: WORKDIR/results/check-<id>/

`check.json`:
```json
{"baseline": "...", "baseline_meta": {...}, "repeat": 1, "sets": {"cpu_model": "68030"},
 "binaries": [{"label", "path", "sha256", "version", "counts": {"regression": 2}, "regressions": 2}],
 "per_binary": [{"label", "entries": {"<entry>": {"config_changes": [{"option", "baseline", "now"}],
                                                  "meta_changes": [{"what", "baseline", "now"}],
                                                  "baseline_status", "status"}},
                 "findings": [{"kind", "entry", "case_key", "mod", "case", "transition": "pass->fail",
                               "baseline": {"st", "rec"}, "now": {"st", "rec"}, "paths": [{"path", "baseline", "now"}],
                               "config_changes": [...], "xfail": {...}, "why": "..."}]}],
 "first_changes": [{"entry", "key", "kind", "first_binary", "on": ["#2 ...", "#3 ..."]}]}
```
Finding kinds: see docs/RUNNER.md (check). `REPORT.md` presents the same data.


---------------------------------------------------------------------------
## 5. Milestone-3 additions

### 5.1 `request` (guest) and `host` (runner) records

The guest asks the host for something it can't do itself (CONTRACT §13):
```
{"v":1,"seq":16,"t":"request","id":8,"what":"audio_mark","tag":"tone440","args":{"edge":"start"}} *xxxxxxxx
```
`id` counts up per boot. `what` is screenshot | audio_mark | key | mouse | joy | cd32pad | wait. `args` is optional.
The guest then waits for the host to write `id` into the mailbox's `host_ack` field (offset 44).

For every request it takes (from either transport, the first copy to arrive), the runner writes a **host record**.
Host records go to `raw/<entry>/<disk>/host.jsonl`, and into `records.jsonl` right after the request they answer, as
`{"boot","seq":<the request's seq>,"rec":{...},"sources":["host"],"host":true}`:

| Field | Meaning |
|---|---|
| `t` | `"host"` |
| `id`, `what`, `tag` | from the request (`id` is null for a v1 `capture` record) |
| `acked` | true: done, and `id` written to mailbox+44. false: not done (`result.error` says why); the guest reports the dependent case untestable. null: a `capture` record, which wants no ack |
| `result` | per `what`, below |
| `req_seq`, `req_t`, `source` | the request's seq, its type (`request`/`capture`), the transport it came from first |
| `t_seen`, `t_done`, `latency_s` | host seconds since launch: when the request was seen, when it was finished, and how long it took |

`result` per `what`:
- `screenshot`: `{"path":"raw/<entry>/<disk>/shot-<id>-<tag>.png","sha256","size","width","height","waited_frames"}`.
- `audio_mark`, edge start: `{"edge":"start","offset":<byte offset in the stream>,"t_s","stream_s"}`.
- `audio_mark`, edge stop: `{"edge":"stop","offset","start":{"id","offset","t_s"},"bytes","settle_s","wav":"raw/.../audio-<id>-<tag>.wav","analysis":{...}}`, where the analysis is:
  - `rate`, `frames`, `seconds`, `dominant_hz`, `loudest` ("L"/"R"), `silent`;
  - with `expect_hz` in the args, also `expect_hz`, `tol_pct`, `freq_ok` (judged on `dominant_hz`, the loudest
    channel), and `expect_side`, `side_amp`, `side_ok` with `side`;
  - `expect_ms` (the args' `ms`: how long the guest played the tone) and `source_ch` (the args' `ch`: the guest's
    channel, e.g. Paula 0-3), when the marks give them;
  - `channels.L` / `channels.R`: `samples`, `rms`, `peak`, `dc` (full scale = 1.0), `clipped`, `silent`, `onset_s`,
    `active_s`, `active_rms`, `freq_zc_hz` (zero crossings), `freq_fft_hz` (FFT peak, searched from 20 Hz up),
    `dropout_fraction`. The active part ends where the sound does: a held output level (Paula keeps its last
    sample after its DMA stops) is trimmed off in 10 ms blocks whose own AC level is below 10 % of the loudest.
- `key`: `{"injected":{"key","up"},"ipc":"SEND_KEY 69 1"}`; `mouse`: `{"injected":{"dx","dy","buttons","port"}}`; `wait`: `{}`.
- Not done: `{"error":"..."}`, plus `ack_error` if the WRITE_MEM failed.

Host records are not guest records. `done.records`, the seq gap check and fingerprints all ignore them, with
these exceptions (runner/amitorture/cases.py), all pseudo cases, which are not counted in the per-entry and
per-module case totals:
- on cycle-exact entries every acknowledged screenshot becomes the pseudo case `host.shot_<tag>`,
  fingerprinted as `{"mod":"host","case":"shot_<tag>","st":"pass","sha256":...}`, so baselines compare the picture.
- on **every** entry that captured audio, each analysed stop mark with an expected frequency (`expect_hz`,
  `freq_mhz` or `clock`+`period`+`samples` in either mark) becomes the pseudo case `host.tone_<tag>` (a leading
  `tone_` in the tag is not doubled: tag `tone_aud0` gives `host.tone_aud0`). Fingerprint:
  `{"mod":"host","case":"tone_<tag>","st","expect_hz","tol_pct","channel","freq_hz"}`.
  - `channel`: the output channel measured, `L`/`R`: the mark's `side`, else (also for `side` `both`) the loudest one. The frequency is
    that channel's `freq_fft_hz` (`freq_zc_hz` if there is no FFT peak).
  - `st`: `pass` when the frequency is within `tol_pct` of `expect_hz` and, with a `side`, `side_ok` holds.
    `fail` when it is not, or when that channel is silent. `untestable` when the segment has no audio, or when a
    mismatch comes from a capture whose tone does not last what the guest played (`expect_ms` +-20 %): SDL
    records in real time, so an emulator slower than real time on a loaded host stretches the sound, leaves gaps
    or repeats old buffers.
  - `freq_hz`: the measured frequency rounded to the tolerance band, anchored at the expected one:
    `expect_hz + k * 2 * tol_hz` with `k = round((measured - expect_hz) / (2 * tol_hz))`, `tol_hz = expect_hz *
    tol_pct / 100`. So it is `expect_hz` itself on a pass, and a baseline, a check or a JIT twin compares it
    stably; a fail shows how far off it is. Absent when the tone was not measured (silent, untestable).
  - Why every entry, not only cycle-exact ones: the rates come from the emulated clocks (Paula's period counter,
    the sound cards' sample rates), not from CPU timing, and measured within 0.02 % of the expected frequency on
    fast, JIT, compatible and cycle-exact cores alike, against a 2 % tolerance (the guest's `tol_pct`; 3 % by
    default).
  - A failed `host.tone_<tag>` is a `case_fail` item (`mod` `host`) in `failures.jsonl` and REPORT.md: `expect`
    has `freq_hz`, `tol_pct`, `tol_hz`, `channel`, `source_ch` and `active_s`; `got` has the measured `freq_hz`,
    `freq_fft_hz`, `freq_zc_hz`, `channel`, `active_s`, `dropout_fraction`, the per-channel frequencies and
    `side_ok`; `why` says both frequencies, the channel, the tolerance and the offset in %; `ctx.wav` is the
    segment. The host requests section of REPORT.md shows each tone's verdict under its audio line.

- on **every** entry whose launch ran the hostspeed module, each timed block becomes the pseudo case
  `host.speed_<insn>` (docs/cases/hostspeed.md). Fingerprint: `{"mod":"host","case":"speed_<insn>","st"}`
  only, because the measurement depends on the host. Its `timing` holds `rtf` (host seconds per emulated
  second), `sigma`, `fps`, `cpu_per_emu_s`, `cpu_total_per_emu_s`, `ns_per_op`, `gross_ns_per_op`,
  `ratio_ref` (banded like any timing value).
  - Measurement: the runner keeps, at every mailbox poll while a block runs (mailbox module 26, case
    1000 + block), `[t, heartbeat, case, cpu_main_ns, cpu_total_ns, cpu_top_ns]`: host seconds since
    launch, the guest's frame count, and /proc/<pid>/task/*/schedstat CPU times. They are in the disk's
    `result.json` as `hostspeed: {"fields","samples","poll_s","pos0"}`, with `host_context:
    {"cpu_model","cpus","board","jobs","loadavg_start","loadavg_end","slot_wait_s"}`.
  - `st`: `fail` when rtf > 1.01 + 3 sigma (`cases.SPEED_RTF_LIMIT`); `pass` when not, and either rtf + 3
    sigma <= 1.01 or 3 sigma <= 0.5 %; `untestable` without samples, with fewer than 3 in the block, or
    when the noise is too large to tell. A `skip` record makes no case.
  - Parity skips these cases (`engine_dependent`): the JIT and the interpreter run at different speeds by
    design.
  - A failed `host.speed_<insn>` is a `case_fail` item (`mod` `host`): `expect` `{"rtf_max","limit"}`,
    `got` `{"rtf","sigma","fps","frame_hz","samples","span_s","cpu_per_emu_s","ns_per_op","ratio_ref",
    "ipc_bound"}`, `ctx.host` the host context, `ctx.block` the guest's block; `why` reads like "FSIN: the
    emulator ran at 0.75x real time while executing it (rtf 1.337, ...); 250 ns host per instruction,
    3.5x FADD. Measured on Cortex-A76 x4 (Raspberry Pi 500 Rev 1.0), --jobs 1, ...".
  - `summary.json` `hostspeed`: `[{"entry","disk","jit","cpu_speed","host","blocks":[{"insn","cls","st",
    "rtf","sigma","fps","frame_hz","samples","cpu_per_emu_s","cpu_total_per_emu_s","ns_per_op",
    "ns_per_iter","ratio_ref","ref","why"}]}]`; REPORT.md shows it as "Host speed per instruction".

### 5.2 `raw/<entry>/<disk>/` (added files)
| File | Content |
|---|---|
| `host.jsonl` | the host records of this launch |
| `shot-<id>-<tag>.png` | screenshots taken for `screenshot` requests (`shot-c<seq>-<tag>.png` for v1 `capture` records) |
| `audio-<id>-<tag>.wav` | each audio segment between a start and a stop mark (S16 stereo, 44100 Hz) |

### 5.3 `result.json` (added fields)
| Field | Meaning |
|---|---|
| `host_requests` | `{"requests","acked","not_acked","captures","duplicates","by_what":{what:{"n","acked"}},"unserved","raw"}`: per launch. The entry result has the totals over its disks |
| `audio` | only when this launch was captured: `{"file","bytes","format":"s16le","rate","channels","seconds_written","seconds_elapsed","segments","open_marks"}` |
| `cd` | CD-booted entries (CD32/CDTV): `{"iso","iso_bytes","files","startup_sequence","trademarks","tm_tag","exe_bytes","image_bytes","bss_bytes","relocs","header_slot"}` |
| `board_roms` | ROMs a board entry needed: ROM identity plus the `option` it was given in |
| `mailbox.found_after_s` | seconds from launch until the mailbox was found |
| `command.env.SDL_AUDIO_DISK_OUTPUT_FILE` | present when the audio was captured (`SDL_AUDIO_DRIVER=disk`) |

Matrix entries (`summary.json` → `configs/`, and `matrix --json`) may have these fields:
- `boot` (`"cd"`);
- `hardware` (the board, in words);
- `disk_groups` (the disk-set groups the entry runs);
- `board_roms` (`[{"ids","what","option"}]`);
- `media_files` (`[{"name","size"}]`).

### 5.4 `summary.json` (added fields)
- `catalogue`: `{"file":"docs/catalogue.yaml","totals":{"covered","partial","not_covered"},"rows":[{"id","area","what","status","why","pass","fail","skip","untestable","entries":[...],"cases":<distinct case keys matched>}]}`. It is null when the file is missing, or `{"error"}` when it can't be parsed. Statuses are explained in docs/RUNNER.md, Coverage catalogue.
- `host_requests`: `[{"entry","requests","acked","not_acked":[{"id","what","tag","disk","why"}],"screenshots":[{"id","tag","path","sha256","disk"}],"audio":[{"id","tag","disk","seconds","dominant_hz","expect_hz","freq_ok","expect_side","side_ok","rms":{"L","R"},"freq":{"L","R"},"wav","tone":{"case","st","expect","measured_hz","why"}}]}]`, one item per entry that made requests (`tone`: the `host.tone_<tag>` verdict, when the mark had an expected frequency).

### 5.5 check: finding kind `fail_detail_changed`
This is a case that failed in the baseline and still fails, and one of these is true:
- the `expect`, `got`, `why` and `in` are the same, and only paths under `ctx` differ, for example absolute
  addresses after a disk rebuild moved the payload;
- the case is a known bug registered in xfail.json, and its measured values changed.

The finding carries the
same fields as a `regression` (`transition: "fail->fail"`, `baseline`, `now`, `paths`). It is informational: it is
not in `REGRESSION_KINDS`, so it never makes the check exit with status 1.

### 5.6 failures.jsonl: kind `hrm_timing_mismatch`
`{"id","kind":"hrm_timing_mismatch","entry","mod","case","case_key","path":"timing.<x>","hrm":<HRM figure>,"measured":<value>,"confidence":"low","machine","why"}`.
This item is made only for cycle-exact entries, when a case's `timing.hrm_<x>` differs from its `timing.<x>`. It is
grouped into a low-severity problem (REPORT.md Notes) and never affects the verdict or a baseline check.

### 5.7 Diagnosis fields (problems in summary.json; result.json)
Problems of kind case_fail/engine_mismatch carry these fields:
- `fields`: the expect/got field names the problem is about.
- `case_table`: `[{"case_key","entries","why","doc","doc_where","evidence","undefined_fields","defined_fields":{name:"docs/cases/<mod>.md:<line>"}}]`.
- `config_flags`: `{"options":[...],"per_entry":[{"entry","values":{option:value}}],"conflicts":[{"option","case_names","config_has","precondition","cases","entries"}],"all_conflict"}`.
  A value `x (default)` was left out by `--dump-config` because it is Amiberry's default.
- `passes_diff`: `{"passes_on","not_run_or_skipped_on","separating":[{"option","failing","passing","affects_module"}],"within":[{"among","passing","separating"}],"nearest":[{"entry","vs","n","differences":[{"option","failing","passing"}]}]}`.
- `emulator_log`: `[{"entry","disk","raw","counts":{category:n},"groups":[{"category","pattern","modules","count","examples","addresses","first_t","last_t"}],"more","exact","time_source","ambiguous"}]`.

Severity `low` with the title prefix "[config contradicts the cases' stated condition]" means every failure of the
problem has a precondition conflict.

Per launch (`raw/<entry>/<disk>/`):
- `serial-times.json`: `[[t, bytes received so far], ...]`, the arrival time of every serial chunk in seconds since
  launch.
- `result.json` `log_counts`: categories dummy_bank, jit_fault, bus_timeout, unimplemented, illegal, trap, mmu,
  cycleunit, warning (plus the older jit_fallback/jit_flush/jit_segv/illegal regex counts). The entry result has
  the sums.
- `log_mining`: `{"time_source","exact","groups":[...]}`.

Narrowing results (`narrowing.json` cases) may have `skipped_noop`: the variants that were not launched because they
run the same CPU core table.
