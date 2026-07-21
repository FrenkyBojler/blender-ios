# Performance Profiling — a catalog of mini-skills

Practical recipes for finding where time goes in this project's native code —
the SculptCore engine DLL (`sculptcore_capi.dll`) and `blender.exe` itself.
Each numbered **skill** is self-contained: read the decision guide, jump to the
one you need.

Two complementary families of technique:

- **Targeted in-code instrumentation** — you already suspect a region and want
  wall-clock numbers for named phases, or want to catch a periodic hitch. Cheap,
  precise, requires a rebuild, sees only what you instrument. Skills 1–3.
- **Sampling profilers (AMD uProf)** — you *don't* know where the cost is and
  want the whole call tree attributed automatically. No code changes, needs
  debug symbols, statistical. Skills 4–8.

Rule of thumb: reach for uProf first to find *which* function is hot, then add
instrumentation to that function to understand *why* and to A/B a fix.

---

## Decision guide

| Situation | Skill |
|---|---|
| "Sculpting stutters ~once a second" (live-path-only hitch) | 1 (StrokeProfiler + SPIKE logs) |
| "This one function/loop is slow, how slow?" | 2 (scoped `chrono` timer) |
| "Add a permanent phase counter to the stroke path" | 3 (extend `StrokeProfiler`) |
| "Where is all the CPU time going?" (unknown hotspot) | 5 (uProf CLI, `tbp` + call stacks) or 4 (GUI) |
| "Why is *this* function hot — cache misses? branches?" | 6 (uProf IBS / `assess` / `memory`) |
| "Attach to an already-running blender.exe" | 7 |
| "Nothing resolves to function names, just addresses" | 8 (symbols) |
| "uProf says access denied / driver failed to start" | 9 (admin elevation) |

---

## Skill 1 — In-code instrumentation for a periodic hitch

The engine already ships a lightweight wall-clock profiler,
`StrokeProfiler` in `extern/sculptcore/source/debug/profile.h`, gated behind the
debug app's `--profile` flag (`scene.profiler.enabled`). It accumulates
count/total/min/max per phase (`dab` split into `cpu`/`gpu`/`read`, plus
`begin`/`end`), prints per-stroke lines at `endStroke()`, and a cumulative table
at exit via `printSummary()`. A *max* far above the *avg* is the signature of a
"random lag" spike.

**Workflow** (this is the sanctioned pattern — see the "Profiling a periodic
hitch" section of `extern/sculptcore/CLAUDE.md`):

1. Reproduce with `--profile`. The min/max columns localize the spike to a phase
   (e.g. the WGSL read-phase regression showed up as a huge `dab-gpu`/`read`
   max).
2. If per-stroke stats aren't fine-grained enough, add **throwaway** instrumentation:
   - A **SPIKE log** in the hot path: when one dab exceeds a threshold, print
     `index + time-since-stroke-start + phase split`. Turns "lags every second"
     into evenly-spaced, phase-labeled lines.
   - A **FRAME-SPIKE log** wrapping the interactive frame loop, for stalls that
     land in render/present rather than a brush dab (so they never reach `addDab`).
3. Gate everything behind `scene.profiler.enabled` so normal runs stay inert.
4. Fix, confirm, then **delete every SPIKE / FRAME-SPIKE counter and printf you
   added.** `StrokeProfiler` (the phase counters) is the permanent part and stays;
   the temporary scaffolding is not meant to live in the tree. Mark it
   `CLAUDENOTE:` while it's in place so it's trivially greppable.

**Gotcha:** a live-path-only hitch will not reproduce in a scripted/batch run.
Hand the user a setup-only script plus `--interactive`.

---

## Skill 2 — Ad-hoc scoped wall-clock timer

For a one-off "how long does this take" measurement anywhere in engine code, use
`std::chrono::steady_clock` the same way `StrokeProfiler::ms()` does:

```cpp
#include <chrono>
const auto t0 = std::chrono::steady_clock::now();
expensive_thing();
const double ms = std::chrono::duration<double, std::milli>(
    std::chrono::steady_clock::now() - t0).count();
std::printf("[t] expensive_thing: %.3f ms\n", ms);
std::fflush(stdout);  // flush — the process may abort before buffered output lands.
```

Notes:
- `steady_clock`, not `system_clock` — monotonic, immune to wall-clock adjustments.
- Wrap several sites with a one-letter tag per file (`P`, `B`, ...) so interleaved
  output is readable, matching the source-line-print convention in
  `extern/sculptcore/CLAUDE.md`.
- This is scaffolding: strip it when done (`CLAUDENOTE:` while present).
- A single measurement is noise. Loop the operation, or read min/max over many
  calls, before trusting a number — a cold first call, page faults, and turbo
  ramp all distort a lone sample.

---

## Skill 3 — Add a permanent phase counter to `StrokeProfiler`

When a phase genuinely deserves to be measured on every run (not throwaway),
extend `StrokeProfiler` rather than bolting on a parallel timer:

1. Add a `PhaseStat` member (per-stroke) and a matching `all*` member (session
   cumulative) — mirror `dabCpu_` / `allDabCpu_`.
2. Add an `addX()` method that early-outs on `!enabled`, records into both.
3. Print it in `endStroke()` and `printSummary()` via `printPhase()`.
4. Reset the per-stroke member in `beginStroke()`.

`PhaseStat::add()`/`merge()` already handle count/total/min/max and the empty-set
edge cases, so a new phase is a few lines. Keep it `enabled`-gated so it's free
in normal runs.

---

## Skill 4 — AMD uProf (GUI): find the hotspot

[AMD uProf](https://www.amd.com/en/developer/uprof.html) is a statistical
sampling profiler for AMD x86 CPUs. It periodically samples the instruction
pointer (and optionally the call stack) driven by an OS timer, core performance
counters (PMC), or Instruction-Based Sampling (IBS), then attributes samples to
functions/lines. The GUI is the fastest way to get oriented.

Quickest path for this project:

1. `bundle --pdb` the engine (Skill 8) so `sculptcore_capi.dll` has a `.pdb`
   beside it, and build Blender `relwithdebinfo` (ships `blender_private.pdb`).
2. Launch uProf → **PROFILE** → select **Launch Application** and point it at
   `blender.exe` (working dir set so the addon's vendored DLL resolves), or
   **Attach to Process** for an already-running Blender (Skill 7).
3. Pick a profile type: **CPU Profile → Time-based Sampling (TBP)** to start, or
   **Assess Performance** for a broad first look. Enable **Call Stack Sampling**
   (needed for the Flame Graph and the Call Graph / butterfly view).
4. Run the scenario (do the sculpt strokes), stop, let it translate.
5. Read the results:
   - **Hot Spots / Functions** table — self vs. total time per function.
   - **Flame Graph** — call-stack visualizer; wide frames are where time pools.
   - **Call Graph** — butterfly caller/callee view around a function.
   - **Source/Assembly** — line-level attribution (needs symbols + source paths).

The **Thread Concurrency** graph is Windows-only and **requires admin
privileges** (Skill 9).

---

## Skill 5 — AMD uProf (CLI): scriptable collect + report

`AMDuProfCLI.exe` (in the uProf `bin/` — add it to `PATH`, or call by full path)
is the headless equivalent; use it for repeatable, diffable runs. It lives on
Windows, Linux, and FreeBSD. General shape:

```
AMDuProfCLI [--version] [--help] COMMAND [<options>] <PROGRAM> [<args>]
```

Core commands: **`collect`** (run program, gather raw samples), **`report`**
(turn a raw collection into a CSV/DB report), **`translate`** (raw → intermediate,
usually implicit), **`profile`** (collect + report in one shot), **`timechart`**
(power/thermal/frequency over time), **`compare`** (diff two collections).

### Time-based sampling with call stacks (the everyday recipe)

```
AMDuProfCLI collect --config tbp -g -o C:\temp\uprof-tbp "C:\dev\blender\build_windows_x64_clang_RelWithDebInfo\bin\blender.exe"
```

- `--config tbp` — time-based profiling. `--config` has **no** short alias
  (`-m` is `--data-buffer-count`, not the config). Multiple `--config` are
  allowed. The predefined configs on this install (5.3) are: `tbp`, `hotspots`
  (TBP **with** callstacks), `assess` / `assess_ext` (overall assessment &
  likely-issue triage), `inst_access` / `data_access` (I-cache/ITLB and
  D-cache/DTLB locality), `cpi` (CPI/IPC), `branch`, `energy`. List them with
  `AMDuProfCLI info --list collect-configs`. (IBS and cache-line analysis are
  **not** configs here — see Skill 6.)
- `-g` — enable **call-stack** collection with defaults (unwind interval 1 ms,
  depth 128, scope `user`, mode `fp`); required for flame/call graphs. Tune with
  `--call-graph-depth <2-392>`, `--call-graph-mode <fp|fpo>`,
  `--call-graph-type <user|kernel|all>`, `--call-graph-interval <1-100>`, or the
  combined `--call-graph <I:D:S:F>`. `fpo` adds DWARF/unwind-info stacks when
  frame pointers are absent — worth it for optimized builds.
- `-o <dir>` — output base dir. If it exists, an auto-named session subdirectory
  is created inside it; if not, data is written directly into it.
- The program to profile is the **last** positional argument (no `--` separator;
  AMD's own examples put it directly after the options).

Then generate a report from the raw collection (session dir is the one `collect`
created under `-o`):

```
AMDuProfCLI report -i C:\temp\uprof-tbp\<SESSION-DIR> --report-output C:\temp\uprof-tbp\report.csv
```

- `-i` / `--input-dir` — the raw collection (session) directory. **There is no
  `-o` on `report`.** Write output with `--report-output <path>` (a `.csv` path
  is treated as a file; otherwise a directory) or `--stdout`; with no flag the
  report lands in the session dir.
- Add `-g` (with `--detail` or `-p`) to print callstacks, `--inline` to attribute
  to inlined functions, `--disasm` for source/assembly. Open the same session
  directory in the GUI for the flame graph.

### Useful variants

- **Fixed duration** (let it sample then stop the app for you):
  `-d` / `--duration <seconds>`.
- **Start paused**, resume from the app via the profile-control APIs
  (`AMDProfileController.h`): `--start-paused`. `--start-delay <n>` instead
  begins after `n` seconds.
- **Whole system** vs. single launched process, and **attach by PID**: see
  Skill 7.
- **Compare** two runs (before/after a fix):
  `AMDuProfCLI compare --baseline <base-dir> --with <new-dir> -o <out>` (compare
  *does* take `-o`; report does not). Output is Markdown by default (`--html` for
  HTML, `--stdout` to print). Requires identical events/duration across sessions
  and rejects system-wide (`-a`) sessions.

> Flags above were verified against the installed **AMDuProfCLI 5.3.518.0**.
> Spellings drift between versions — trust `AMDuProfCLI <command> --help` on the
> box over this document if they ever disagree.

---

## Skill 6 — Explain *why* a function is hot (IBS / cache / branches)

Once Skill 4/5 names a hot function, switch profile type to get
micro-architectural detail instead of just "where":

- **`--config assess`** (or `assess_ext`) — broad counters (retired
  instructions, IPC/CPI, mispredicts, cache activity) to see if the function is
  compute-bound, memory-bound, or branch-bound. `--config cpi` is the narrow
  IPC/CPI-only version; `--config branch` targets misprediction.
- **`--config data_access` / `inst_access`** — poor D-cache/DTLB and
  I-cache/ITLB locality; valuable for the spatial-tree / attribute-heavy hot
  loops where the guideline to avoid STL containers in hot paths
  (`litestl::util` equivalents) matters. For **false cache-line sharing**, add
  `--metric memory` on collect, then report with `--show-all-cachelines` /
  `--limit-cacheinfo <n>`.
- **IBS (Instruction-Based Sampling)** tags individual ops with precise
  latency/cache/branch data (AMD's answer to event skid) — the most accurate
  line-level attribution, at higher overhead. On this 5.3 build IBS is a
  **custom event**, not a `--config`: use `-e event=ibs-op` / `-e event=ibs-fetch`
  (optionally `,interval=<n>`, and Zen4+ filters like `ibsop-l3miss=1`).
  **Note:** IBS is unavailable on this machine — it fails with `ERROR: IBS
  counters are not available` even when elevated (confirmed; see Skill 9). Fall
  back to the core-PMC configs above here.

Example (IBS OP with call stacks):

```
AMDuProfCLI collect -e event=ibs-op,interval=250000 -g -o C:\temp\uprof-ibs "C:\...\blender.exe"
```

Report sorted on the IBS event: `AMDuProfCLI report -s event=ibs-op -i <SESSION-DIR>`.
Interpretation stays in the GUI's Source/Assembly and Metrics views; the CLI
report emits the same metrics as CSV columns.

---

## Skill 7 — Profile an already-running or system-wide

Launching `blender.exe` under the profiler is cleanest, but for a hitch that only
shows after a big scene is loaded, attach instead:

- **Attach to a running process:** GUI → **Attach to Process** → pick
  `blender.exe`. CLI takes `-p` / `--pid <PID,..>` (comma-separated PIDs). Get the
  PID from Task Manager or `(Get-Process blender).Id`.
- **System-wide:** collect with no target program, `-a` / `--system-wide`, and a
  duration, e.g.
  `AMDuProfCLI collect --config tbp -a -d 15 -o C:\temp\uprof-sys`.
  On this machine system-wide collection runs **without** admin; thread
  concurrency (next bullet) does not. Whether either needs elevation depends on
  the driver install — see Skill 9.
- **Thread concurrency** (Windows-only): add `--thread thread=concurrency` to the
  collect line.

Because the SculptCore engine is a **runtime-loaded DLL**, attach *after* the
addon has loaded and a session is active — otherwise `sculptcore_capi.dll` isn't
mapped yet and its symbols won't resolve.

---

## Skill 8 — Make sure symbols resolve

A profile full of raw addresses or `[unknown]` frames means the profiler can't
find debug info. Ensure it before collecting:

- **SculptCore DLL:** build/stage with PDBs. `node make.mjs bundle --pdb` (in
  `extern/sculptcore`) vendors `sculptcore_capi.dll` **and** its `.pdb` into the
  addon copy blender actually runs. The engine's default build type is
  `RelWithDebInfo` (optimized + symbols) — keep it; a `release` build strips
  detail and inlines away frames. Optimized builds still inline aggressively, so
  expect some functions to be folded into callers.
- **blender.exe:** use the **`relwithdebinfo`** preset. Symbols ship in
  `../build_windows_x64_clang_RelWithDebInfo/source/creator/RelWithDebInfo/blender_private.pdb`.
- **Symbol / source paths (CLI `report`):** point at the directories holding the
  `.pdb`s and sources explicitly —
  `--symbol-path <dir>[;<dir>]`, `--bin-path <dir>`, `--src-path <dir>`
  (each repeatable). Use the Blender build tree and the addon's
  `lib/sculptcore/` for the staged DLL, and the source roots
  `C:\dev\blender\main` + `extern\sculptcore\source`.
- **System (OS/driver) frames:** let uProf pull them from Microsoft's symbol
  server —
  `--symbol-server https://msdl.microsoft.com/download/symbols --symbol-cache-dir C:\symbols`
  (or set `_NT_SYMBOL_PATH=srv*C:\symbols*https://msdl.microsoft.com/download/symbols`
  before launching the GUI).

Sanity check: if the hot frames are named and map to your `.cc` files, symbols
are good. If they're `sculptcore_capi.dll+0x...`, the PDB isn't being found.

---

## Skill 9 — Admin elevation on Windows

How much of uProf needs administrator rights **depends on how the
`AMDPowerProfiler` driver was installed and configured** — it is not a fixed
list. Where the driver is installed as a started service with non-elevated
access (as on this machine), timer-based, core-PMC, and system-wide sampling
all run from an ordinary user shell; where it is not, those fall back to
"access denied" / "failed to start driver" / empty results / greyed-out
profile types, and you must elevate.

**Verified on this machine (AMDuProfCLI 5.3.518.0, non-elevated PowerShell):**

| Operation | Non-elevated result |
|---|---|
| `collect --config tbp` (launched process) | **works** |
| `collect --config assess` (core PMC) | **works** |
| `collect --config tbp -a` (system-wide) | **works** |
| `report` / `translate` | **works** |
| `collect -e event=ibs-op` (IBS) | **fails** — `ERROR: IBS counters are not available` |
| `collect --thread thread=concurrency` | **fails** — `ERROR: administrator access is required for option (--thread)` |

So on this box the driver already grants non-admin access, and only two things
are gated:

- **Thread Concurrency** (`--thread`) is a hard admin gate — the CLI rejects it
  by name without elevation. Elevate for it.
- **IBS is unavailable on this machine — and it is *not* an elevation problem.**
  A confirmed **elevated** run (verified High Mandatory Level +
  `BUILTIN\Administrators` via `whoami /groups`) still fails with the identical
  `ERROR: IBS counters are not available`. So IBS is disabled at the
  hardware/BIOS/driver level (the IBS MSRs aren't exposed), not gated on
  permissions. Do not reach for IBS (Skill 6's `-e event=ibs-op`) on this box;
  use core-PMC configs (`assess`, `data_access`, `cpi`, `branch`) instead.

Treat the table as this machine's baseline, not a universal rule: on a box
where the driver lacks non-admin access, even plain TBP collect needs
elevation.

**Elevate the GUI:** right-click **AMDuProf** → **Run as administrator** (or set
it permanently via the shortcut's *Properties → Compatibility → Run this program
as an administrator*).

**Elevate the CLI:** run `AMDuProfCLI.exe` from an **elevated** terminal. From a
normal PowerShell you can spawn one:

```powershell
Start-Process powershell -Verb RunAs
# then, in the elevated window:
& 'C:\Program Files\AMD\AMDuProf\bin\AMDuProfCLI.exe' collect --config tbp -g -o C:\temp\uprof-tbp -- 'C:\dev\blender\build_windows_x64_clang_RelWithDebInfo\bin\blender.exe'
```

Or launch a single elevated command directly:

```powershell
Start-Process -Verb RunAs -FilePath 'C:\Program Files\AMD\AMDuProf\bin\AMDuProfCLI.exe' `
  -ArgumentList 'collect','--config','tbp','-g','-o','C:\temp\uprof-tbp','--','C:\dev\blender\build_windows_x64_clang_RelWithDebInfo\bin\blender.exe'
```

> In this session, an interactive UAC prompt must be accepted by the user — the
> non-interactive tools can't click it. If elevation is needed, ask the user to
> run the `Start-Process ... -Verb RunAs` line themselves (the `! <command>`
> prompt prefix runs it in-session so the output lands here).

**Driver troubleshooting:**
- The `AMDPowerProfiler.sys` driver must be installed and started (it's set up by
  the uProf installer — run the **installer** as administrator too).
- If Windows refuses it with *"cannot verify the digital signature"*, the driver
  signature isn't trusted on this machine — reboot after install, update to a
  current uProf build (older AMD-only-signed drivers fail on hardened Windows 10/11),
  and reinstall elevated. Timer-based sampling of a launched process may still
  work without the driver, but PMC/IBS/system-wide will not.

---

## Cleanup reminder

Everything in Skills 1–3 that isn't the permanent `StrokeProfiler` is throwaway
scaffolding: mark it `CLAUDENOTE:` while present and strip it once the fix is
confirmed, per the project comment rules. uProf collections land in
`C:\temp\uprof-*` (or your chosen `-o` dir), never in the repo.

---

## Sources

- **CLI flags in Skills 5–8 were verified live against the locally installed
  `AMDuProfCLI.exe` 5.3.518.0** (`collect`/`report`/`compare --help`,
  `info --list collect-configs`) at
  `C:\Program Files\AMD\AMDuProf\bin\`, on 2026-07-21.
- [AMD uProf — product page](https://www.amd.com/en/developer/uprof.html) and
  [performance analysis overview](https://www.amd.com/en/developer/uprof/uprof-performance-analysis.html)
- [AMD uProf User Guide (57368) — CPU Profiling](https://docs.amd.com/r/en-US/57368-uProf-user-guide/CPU-Profiling)
  and the [guide index](https://docs.amd.com/r/en-US/57368-uProf-user-guide)
- [AMD uProf User Guide v4.2 (PDF)](https://www.amd.com/content/dam/amd/en/documents/developer/version-4-2-documents/uprof/uprof-user-guide-v4.2.pdf)
- [Purdue RCAC — AMD uProf CLI examples](https://www.rcac.purdue.edu/knowledge/profilers/amduprof)
- [SURF User KB — AMD uProf commands](https://servicedesk.surf.nl/wiki/spaces/WIKI/pages/74227822/AMD+uProf)
- [AMD community — Windows driver signature issue](https://community.amd.com/t5/server-gurus-discussions/amd-uprof-services-failed-to-start-because-quot-windows-cannot/m-p/379078)
- In-tree: `extern/sculptcore/source/debug/profile.h`, and the "Profiling a
  periodic hitch" section of `extern/sculptcore/CLAUDE.md`.
