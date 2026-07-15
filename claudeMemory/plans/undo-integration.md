# Plan — Undo Integration (Wrapped Undo Type + MeshLog Coupling)

**Goal.** Correct, memory-bounded undo for the SculptCore mode. Tier 1
(memfile via the flush contract) comes free with
[mode-infra.md](./mode-infra.md) C3/C4; this plan is **Tier 2**: the
C-implemented `CUSTOM_MODE` undo type that wraps opaque addon state, plus the
addon-side coupling to SculptCore's meshlog so per-stroke undo is a delta, not
a mesh snapshot.

**Dependencies.** [mode-infra.md](./mode-infra.md) (mode + flush/refresh),
[mesh-convert.md](./mesh-convert.md) (session/flush),
[python-bindings.md](./python-bindings.md) (meshlog is driven from Python).

---

## 1. Background (validated 2026-07-15)

### Blender side (from the design doc, Part 1/2)

- `UndoType` contract: `BKE_undo_system.hh:112-173`; registration
  `BKE_undosys_type_append` (`undo_system.cc:915`). Not Python-exposable
  (threading, `Main` lifetime, memfile-interop protocol) — hence one C
  wrapper written once.
- Memfile interop requirements (all in the C wrapper, never the addon):
  `ED_undosys_stack_memfile_id_changed_tag` on object + mesh at push
  (`memfile_undo.cc:366-380`), `bmain->is_memfile_undo_flush_needed` (drives
  `flush()` through `ED_editors_flush_edits_for_object_ex`,
  `ed_util.cc:269`), name-based `UndoRefID` for `step_foreach_ID_ref`,
  `use_memfile_step` on mode-boundary steps (sculpt's `DyntopoEnd` pattern,
  `sculpt_undo.cc:2204`).
- Sculpt's model: session data flushed to the Mesh ID on demand
  (`needs_flush_to_id`); its undo steps are deltas against that baseline.

### SculptCore side (meshlog, `extern/sculptcore/source/meshlog/`)

- `MeshLog` (`meshlog_base.h:1262`): linear history of steps (one per
  stroke), cursor-based. Step = heterogeneous chunks: `LogChunkElems`
  (sparse attribute deltas, `:370`), `LogChunkTopo` (topology events, one per
  dyntopo dab, `:657`), `LogChunkReorder` (`:1129`), External chunks
  (`appendChunk`, `:1635` — e.g. VDM tile deltas, multires store blobs).
- Host-drivable: `beginStep(hasDyntopo)` (`:1409`) / `endStep()` (`:1533`) /
  `undo(Mesh*, SpatialTree*)` / `redo(...)`; `pushTopoChunk()` per dyntopo dab
  (`:1557`) — normally driven by `brush::CommandExecutor`
  (`brush_executor.h:1544-1556`), which the addon uses anyway.
- Memory accounting maps 1:1 onto Blender's undo limiter: `stepMemSize(id)`
  (`:1447`), `freeStep(id)` (`:1474`), `setMaxUndoSteps(n)` (`:1542`),
  `lastStepId()` (`:1441`).
- No whole-step byte serialization (steps are RAM-only) — a Blender undo step
  therefore stores a **step id**, not bytes; the engine session must outlive
  the steps that reference it.

## 2. Design

### Step mapping

One Blender `CustomModeUndoStep` per stroke/operator, holding an opaque
`PyObject *` returned by the addon's `undo_encode()`. For SculptCore that
object is small: `{meshlog_step_id, mem_size, object_name, session_token}`.
`undo_decode(state, direction)` calls `MeshLog.undo/redo` (via
CommandExecutor), tags draw-provider nodes dirty, and requests redraw. The
heavy data stays inside the engine; Blender's undo stack sees accurate sizes
via a `step_size` callback sourced from `stepMemSize`.

### Protocol responsibilities

| Concern | Owner |
|---|---|
| Stroke bracketing (`beginStep`/`applyDab.../`endStep`) | addon stroke operator (via CommandExecutor) |
| Push `CustomModeUndoStep` after `endStep` | addon calls `bpy` seam (`ObjectModeType`-provided `undo_push(name, state)`) |
| memfile-interop tags/flags, `UndoRefID`, `use_memfile_step` at mode boundaries | C wrapper |
| `step_free` → addon `undo_free(state)` → `MeshLog.freeStep(id)` | C wrapper → addon |
| Memory accounting (`step_size`) | C wrapper reads a `size` attr on the state object |
| Session death (mode exit) with live steps in the stack | see §4 |

### Suppressing per-property undo noise

`ED_undo_is_legacy_compatible_for_property` (`ed_undo.cc:446`) suppresses
property-edit undo pushes during sculpt mode, keyed on `OB_MODE_SCULPT`;
custom modes don't inherit it (by design). Brush-property tweaks mid-mode will
each push a memfile step — acceptable v1; if it proves noisy, extend that
check to modes flagged `bl_use_custom_undo` (tiny, separately reviewable).

## 3. Change list

### Workstream A — Blender C wrapper

| # | File | Change | Size |
|---|---|---|---|
| A1 | `editors/undo/custom_mode_undo.cc` (new) + `undo_system_types.cc` | `BKE_UNDOSYS_TYPE_CUSTOM_MODE` registered **before** memfile: `poll` (active object in `OB_MODE_CUSTOM` + type has `bl_use_custom_undo`), `step_encode` (calls `undo_encode` trampoline, GIL held, stores PyObject + object `UndoRefID`), `step_decode` (ensure object still in mode — re-enter if needed, mirroring sculpt undo's mode-restore behavior — then `undo_decode(state, dir)`), `step_free` (`undo_free`, GIL), `step_foreach_ID_ref`. Memfile-interop calls per Background §1. | L (~300) |
| A2 | `makesrna/intern/rna_object_mode.cc` | `undo_encode`/`undo_decode`/`undo_free` trampolines + `bl_use_custom_undo` validation; a `undo_push_custom(name)` runtime function the addon calls at stroke end (wraps `BKE_undosys_step_push_with_type`). | M |
| A3 | `ed_undo.cc` | Ensure `refresh` (mode-infra C4) ordering vs. custom-step decode: custom steps decode *without* touching `Main`, so refresh must NOT fire for them (only for memfile steps). Gate the undo_post callback on step type. | S |

### Workstream B — Addon coupling

| # | Change | Size |
|---|---|---|
| B1 | Stroke operator: `executor.beginStep(dyntopo)` on stroke start; `endStep()` + `undo_push_custom("Sculpt Stroke")` with state `{step_id: log.lastStepId(), size: log.stepMemSize(id), token}` on stroke end. | S |
| B2 | `undo_decode(state, direction)`: validate session token (object still in mode with same session); `executor`/`MeshLog` `undo()`/`redo()` with the active `SpatialTree` (required for topo re-owning); mark touched nodes for the draw provider; on multires, External-chunk replay covers the grids store (see [multires-convert.md](./multires-convert.md) C4). Set `flush`-dirty so the next memfile encode/save sees the change. | M |
| B3 | `undo_free(state)`: `log.freeStep(step_id)` if session alive; no-op otherwise. | S |
| B4 | Undo-limit plumbing: read Blender's undo memory limit (`bpy.context.preferences.edit.undo_memory_limit`) → `setMaxUndoSteps`/manual eviction policy; keep Blender-side `step_size` truthful so the global limiter works too. | S |

## 4. The hard part — history coherence across session boundaries

Interleavings to handle explicitly (protocol, tested one by one):

1. **Foreign memfile step while in mode** (user tweaks a scene setting):
   stack = [...custom, memfile, custom...]. Decoding backwards through the
   memfile step replaces `Main` → `refresh` rebuilds the engine mesh from the
   Mesh ID. Meshlog steps *older* than the rebuild are now against dead
   engine state. Rule: **`refresh` invalidates the meshlog past** — the addon
   marks the session generation; `undo_decode` for a state from an older
   generation falls back to "flush already restored by memfile; treat as
   no-op for engine, rebuild from Mesh". This works *because* each custom
   step's mesh effect is also captured by the flush contract in the
   *next* memfile snapshot... it is **not** — flush only runs when a memfile
   step encodes. Mitigation: A1 sets `use_memfile_step=false` normally but
   the **first custom step after any foreign step** forces a flush + tags so
   the adjacent memfile snapshot brackets engine state. Model this on how
   sculpt undo coexists with memfile steps today (`sculpt_undo.cc` steps
   interleave with memfile pushes routinely); copy its tag discipline
   (`ED_undosys_stack_memfile_id_changed_tag`) which is what keeps the
   neighboring memfile snapshots correct.
2. **Mode exit with custom steps in the stack:** exit pushes a
   `use_memfile_step` boundary step (DyntopoEnd pattern); decoding back into
   the mode region re-enters the mode (A1 decode) and `refresh` rebuilds; but
   the engine session was freed at exit → meshlog gone → those steps decode
   as "rebuild from the memfile-restored Mesh" (their `undo_decode` no-ops at
   engine level). Undo across the boundary must restore the *mesh* correctly
   via memfile; per-stroke redo into a dead session degrades to coarse
   (mesh-level) redo. Vanilla sculpt keeps undo data alive across exit;
   v1 accepts coarser cross-boundary behavior — document it.
3. **Object switch within the mode** (multi-object): session per object;
   state carries the object name via `UndoRefID` remapping (renames handled
   by name-based refs).

Write these three as explicit test scenarios before implementing A1.

## 5. Order of work

1. A1/A2 with a stub addon state (no engine) — push/undo/redo/free lifecycle
   + memfile interleaving tests pass.
2. B1–B3 against real strokes; single-object happy path.
3. §4 scenarios: foreign-step interleave, exit-boundary, ASAN + generation
   tokens.
4. B4 memory limits; A3 refresh gating; multires External-chunk hookup.

## 6. Verification

- Stroke → undo → redo restores positions exactly (compare arrays) with no
  Mesh rebuild (fast path); dyntopo stroke → undo restores topology +
  attributes (meshlog topo replay) and draw updates.
- Interleave: stroke, rename object (memfile), stroke, then undo ×4 / redo ×4
  — coherent at every stop, object rename tracked by `UndoRefID`.
- Exit-boundary: strokes → exit mode → undo past the boundary → mesh correct;
  redo back in — re-enters mode, mesh correct (coarse redo acceptable).
- Undo memory: sculpt 200 strokes on a dense mesh with a small undo limit —
  memory stays bounded (`stepMemSize` totals + Blender stack accounting),
  oldest steps evicted, no crash undoing to the eviction horizon.
- Save + quit mid-mode → reload → file contains flushed state (Tier-1
  guarantee unaffected).
- ASAN over the whole matrix; Python exceptions inside `undo_decode` must not
  corrupt the stack (C wrapper catches, reports, degrades to memfile).

## 7. Risks

- §4 scenario 1 is genuinely subtle — sculpt's existing interop is the
  reference implementation; when in doubt, copy its ordering exactly.
- GIL + undo timing: `step_decode` runs during `WM_operator` undo handling on
  the main thread — safe for Python, but keep decode work bounded (meshlog
  replay is C++; Python only orchestrates).
- Meshlog steps referencing a reordered/compacted mesh (LogChunkReorder
  exists for engine-internal reorders) — ensure the addon never compacts the
  engine mesh mid-session except through logged ops.
