# Insights from the webgl-app-framework reports

Source: `webgl-app-framework-reports/integrationReport.md` (how the reference
TypeScript app integrates the engine) and `strokeDriverReport.md` (its stroke
driver). Both describe the engine's *other* production consumer, so they are a
map of engine seams that already exist, are battle-tested, and that our plans
either haven't reached yet or solved differently. This note records what
transfers to the plans in `../plans/` and what does not.

All engine APIs referenced below were verified reflected in
`extern/sculptcore/source/brush/brush_executor.h` (`BIND_STRUCT_METHOD`), so
they are reachable from the Python runtime without new binding work.

---

## High-value, directly actionable

### 1. Symmetry recipe (P4/P7 — not yet implemented here)

`addon-skeleton.md` defers symmetry to a "v1 X-mirror double-dab"; nothing has
landed. The stroke report §4 is a complete, debugged host-side design:

- Mirroring happens **above** the stroke driver (driver stays mirror-agnostic);
  the ToolOp applies the primary dab then one reflected dab per entry in a
  `SymAxisMap` table (8 axis-bit combos → lists of per-component sign-flip
  vectors; X+Y+Z = 7 reflections).
- The mirrored sample folds `diag(mul)` into the projection matrix so
  pixel-radius math stays flip-invariant, flips every direction vector and
  angle by the product of signs, then **re-raycasts** to snap the mirrored
  center onto the surface (except anchored/grab, which use the resolved
  position directly).
- Per-mirror state is kept separate (grab deltas, stroke direction per image)
  but the **dyntopo-due decision is made once on the primary dab and cached**
  so all mirror images remesh on the same samples — otherwise the primary
  starves the mirrors of the spacing budget.

### 2. Anchored / Drag-Dot stroke methods via the engine preview-dab API (P7)

`brush-mapping.md` lists `stroke_method` as "host stroke sampler behavior,
engine unaware" — but the no-compounding mechanism is an **engine seam**, not
host logic: `CommandExecutor.beginPreviewDab(center, radius)` /
`extendPreviewDab` / `rollbackPreviewDab` / `commitPreviewDab`
(`brush_executor.h:1708`–1753, reflected; exercised in
`source/debug/script.cc:1527`). The reference flow for any non-PATH method:

- each new input rolls back the previous provisional dab, applies the new one
  inside a preview bracket; only stroke end commits. At any moment exactly one
  live dab exists — Blender's ANCHORED and DRAG_DOT `Brush.stroke_method`
  semantics fall out directly.
- Anchored specifics: first input must hit the surface; later inputs project
  onto the **camera-facing plane captured at anchor time** (our grab path
  already does this); `RADIUS` live mode = radius grows with screen-space drag
  length, `ANGLE` = rotation follows drag angle.
- Regression pattern worth porting (`sculptcore_anchored_dragdot.test.ts`):
  a 2-point "direct" stroke vs a 5-point "wander" stroke ending at the same
  place must land within 10 % of each other (checksum = Σ|v|²); compounding
  makes the wander result far larger. Cheap headless gate for grab too.

### 3. `setNeighborMode(1)` — the addon never sets it (P4/P7)

The reference app calls `wasmExec.setNeighborMode(1)` (CSR ring-1 neighbor
cache) right after constructing every executor
(`integrationReport.md` §4). Our `_ensure_executor` (`stroke.py:81`) leaves
the default. Check what the default is and whether smooth-family/dyntopo dabs
get faster (or behave differently) with the CSR cache; one line if it helps.

### 4. Autosmooth / smooth kernel choice: BSMOOTH vs SMOOTH (P7)

The reference maps its interactive smooth tool **and** the chained autosmooth
command to `BSMOOTH`, not `SMOOTH` (`TOOL_TO_SCULPTBRUSH`; program =
`[main, BSMOOTH]`). Our mapping uses `SMOOTH` for both (`mapping.py:45`,
`stroke.py:202`); `brush-mapping.md` itself hedges "SMOOTH→SMOOTH/BSMOOTH".
Blender's smooth is boundary-preserving — if BSMOOTH is the boundary-aware
variant it is likely the better parity match. A/B on an open-boundary grid
(the P8 corpus has one) and pick.

### 5. Dyntopo cadence decoupled from dab cadence (P4 stroke quality)

The reference remeshes at its own `dynTopoSpacing` along the stroke arc-length,
not every dab (`strokeDriverReport.md` §7): `dynTopoDue = strokeS −
lastDynTopoS >= dynTopoSpacing`, applied via `applyDab(..., params ?? 0, …)`
(params=0 → plain dab). Our operator remeshes **every** dab when dyntopo is on
(`stroke.py:423`). Adopting a separate spacing knob cuts remesh cost and
matches vanilla detail-spacing behavior. (Our monotonic `_dab_count + 1` seed
matches their `dabSeed++` contract — keep.)

### 6. Stroke smoothing: centripetal Catmull-Rom + arc-length walk (P4 S2)

Our 2D screen-space `StrokeSpacer` walks a **linear polyline** with residual
carry. The reference feeds the same screen-space spacing rule
(`spacing × 2 × radiusPx`) through a centripetal Catmull-Rom → Bezier
conversion (α=0.5, avoids cusps/loops on jittery input) with a 32-chord
arc-length walk and cross-segment carry, plus a 1-segment lookahead and a
trailing flush on release. This is the natural upgrade path for stroke feel
and the parity route to Blender's `use_smooth_stroke` (stabilize stroke).
Pure math, ~150 lines, unit-testable without the engine
(`stroke_math.ts` is the port source).

### 7. Stroke direction for oriented falloffs (P7 checklist: tip shape / rake)

The engine's oriented Box/wing falloffs are fed by a per-dab stroke tangent
(consecutive primary dab centers, mirror-reflected per image). We compute no
stroke direction today. When tip shape / topology rake come up on the parity
checklist, this is the input they need; the reference also slices the world
Bezier around each dab (`ps.curve = subCubic(worldB, t ± 0.15)`) for
curve-following falloffs.

---

## Worth knowing / medium value

- **`validateAndRepair` on ingest** (P3): the reference calls
  `mesh.repairMesh()` (bound `validateAndRepair`) after every deserialize and
  keeps a user-visible `repairLog`. Our `enter` path builds from
  `Mesh_fromArrays` and only pre-warns (loose edges). Running repair after
  conversion + surfacing its log in the enter report is cheap robustness.
- **`meshRevision` as the dirty key** (P3 A4): the reference's serialization
  cache is keyed off an engine `meshRevision` counter. If that revision (or a
  topology-specific sibling) is already maintained engine-side, P3 A4
  `Mesh_topologyDirty` may be a read of an existing counter rather than a new
  C-API.
- **Whole-mesh blob serialization exists** (P6/P4): `serial::writeMesh` —
  versioned, lz4hc, `SCULPT00` header — behind `Mesh_serialize` /
  `Mesh_deserialize` c-api names in the TS backends. Same shape as the
  `Multires_serializeStore` blobs P8 C4 uses for level-crossing undo. If
  non-multires undo ever needs a topology-crossing root (e.g. re-entering a
  session across the mode-exit boundary), this is the ready-made snapshot
  primitive — including a raw/uncompressed split (`Mesh_serializeRaw`) for
  off-thread compression.
- **Pen tilt/twist**: the reference pushes TILTX/TILTY/TWIST device samples
  (normalized /90, /360) alongside pressure; they are inert until a channel
  maps to them. Blender events carry `event.tilt` — when tilt-mapped dynamics
  come up, the device stack is already plumbed (our M4 int-keyed ids extend
  directly).
- **Uniform dynamics**: beyond the common props we bake (M4), the manifest
  path (`queryUniformManifest` → `addUniformDynamic` / 32-sample
  `setUniformDynamicSample`) lets **engine-only kernel uniforms** respond to
  pressure. Natural extension of `engine_props.py` if per-uniform pressure
  toggles are ever wanted.
- **GPU brush path** (future/S5): a full GPU stroke seam exists
  (`GpuBrush_beginStroke/marshalDab/data/applyCo/endStroke`,
  `source/brush/gpu_brush_c_api.cc`). Reference constraints to inherit if we
  ever wire it: decided once per stroke on the first primary dab; incompatible
  with dyntopo and autosmooth; KELVINLET/GRAB kernels only; falls back to CPU
  if init fails before any dispatch; a **shadow-verify mode** (CPU
  authoritative, GPU diffed per dab) was how they validated it — a good
  bring-up pattern under Blender too.
- **Plane dab normal**: the reference may swap the dab normal to the view
  vector for plane-family brushes (`resolvePlaneDabNormal`). Maps to Blender's
  `Brush.sculpt_plane` (AREA/VIEW/X/Y/Z) — currently unmapped; the engine
  evidently supports a caller-chosen plane normal, so this is mapping-table
  work only.

## Not applicable (and why)

- The **"4-place change"** N-API threading, `IWasmInterface` dual-backend
  seam, esbuild/emscripten packaging, and the **V8 sandbox copy fallback**:
  all NW.js/WASM-specific. Our ctypes runtime already has genuine zero-copy
  numpy views, so the "bulk seam must never throw / copy fallback" rules
  don't transfer.
- **5 ms timer-decoupled input queue**: Blender's WM already coalesces
  MOUSEMOVE and delivers INBETWEEN_MOUSEMOVE; the modal operator model makes
  a private tick unnecessary.
- **nstructjs blob-in-datablock persistence**: Blender's Mesh ID stays
  authoritative by design (P3); we deliberately do not persist engine blobs
  in the file.
