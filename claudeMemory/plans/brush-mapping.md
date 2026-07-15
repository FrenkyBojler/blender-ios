# Plan — Brush & Settings Mapping (Blender Brush → SculptCore)

**Goal.** Reuse Blender's existing `Brush` datablocks (and per-mode
`Paint`/unified settings) as the user-facing brush model: the addon reads
Blender brush properties per stroke and configures SculptCore's `Brush` +
`CommandExecutor`. Settings with no Blender equivalent live in addon-defined
custom properties on `Brush`/`Scene` (auto-generated from SculptCore's
reflected uniform manifest where possible).

**Dependencies.** [python-bindings.md](./python-bindings.md) (reflected
`Brush`/executor access), [addon-skeleton.md](./addon-skeleton.md) (stroke
operator consumes this mapping). No Blender C changes in this plan.

---

## 1. Background (validated 2026-07-15)

### Blender Brush (DNA `DNA_brush_types.h`, RNA `rna_brush.cc`)

- `Brush` is a full ID (`ID_BR`) and an **asset** — addons can attach
  `bpy.props` custom properties (`PointerProperty` → `PropertyGroup`) and
  IDProperties; they serialize with the datablock, including into
  asset-library files. Caveat: edits to a linked brush asset live on the
  local working copy until re-saved to the library (`has_unsaved_changes`
  workflow, DNA `:294`, `BKE_brush_tag_unsaved_changes`).
- Key sculpt fields: `size` (`:204`, pixel diameter) / `unprojected_size`
  (`:425`), `alpha` (`:249`), `hardness` (`:251`), falloff
  `curve_distance_falloff` + `_preset` (`:184`, `:353`), `falloff_shape`
  (`:297`), `spacing` % (`:226`), `jitter` (+`jitter_absolute`, `:221-223`),
  `autosmooth_factor` (`:323`), `normal_weight` (`:193`), plane fields
  (`sculpt_plane :279`, `plane_offset :282`, `plane_trim :335`,
  `plane_height/depth/inversion_mode :340-344`), `rate` (airbrush `:232`),
  `stroke_method` (`:213`), `input_samples` (`:216`),
  **`sculpt_brush_type`** (`:301` — renamed from `sculpt_tool`).
- Pressure flags on `flag` (`DNA_brush_enums.h:353`): `BRUSH_ALPHA_PRESSURE`,
  `BRUSH_SIZE_PRESSURE`, `BRUSH_JITTER_PRESSURE`, `BRUSH_SPACING_PRESSURE`,
  `BRUSH_OFFSET_PRESSURE`; direction `BRUSH_DIR_IN`; `BRUSH_ACCUMULATE`,
  `BRUSH_FRONTFACE`, `BRUSH_ORIGINAL_NORMAL`.
- Unified settings: `UnifiedPaintSettings` now embedded **per-Paint**
  (`Paint::unified_paint_settings`, `DNA_scene_types.h:1258`); honor
  `UNIFIED_PAINT_SIZE`/`_ALPHA` when reading size/strength.
- Active brush per mode: `Paint::brush` + `brush_asset_reference`
  (`:1226-1233`); per-brush-type memory in `ToolSystemBrushBindings`
  (`:1158-1170`). The brush-assets system handles activation — the addon
  only *reads* `context.tool_settings.sculpt`-analogue... **note:** our mode
  has no `Paint` slot of its own; see §2 decision 1.
- Dyntopo settings live on `Sculpt` (`DNA_scene_types.h:1491`):
  `detail_size :1507`, `constant_detail :1517`, `detail_percent :1518`,
  `SCULPT_DYNTOPO_*` flags (`:1453-1480`).

### SculptCore brush surface (`extern/sculptcore/source/brush/`)

- `Brush` struct (`brush.h:131`), fully reflected (`defineBindings :296`):
  `strength :135`, `radius :136`, `spacing :137` (fraction of radius),
  `planeoff :142`, `autosmooth :147`, `planeSide :150`, `invert :152`,
  `falloff_kind :24` × `falloff_shape :41`, LUT `falloff_curve[256]`
  (`setFalloffCurveEntry :267`, presets `:715`), texture fields + 5
  coord-spaces (`:75,:189`), cavity automask (`:228-294`), per-brush uniforms
  (kelvinlet `mu`/`nu`, `pinch`, `rake`, `wingAngle`, …).
- **Per-kernel uniform manifest** — `BrushUniformManifestEntry`
  (`brush_command.h:48`): name, type, default, range, dynamic-able;
  enumerable via `queryUniformManifest`/`queriedUniformEntry`
  (`brush_executor.h:969-982`). Pen dynamics per uniform by name
  (`addPropDynamicByName`, `brush.h:435`); pressure pushed per dab via
  `pushDeviceInput` (`brush.h:415`).
- 22 kernels (`brushes/types.h:6`); CLAY/SCRAPE/FILL share one plane kernel
  selected by `planeoff`/`planeSide`; autosmooth is a host-built
  `BrushProgram` `[main, SMOOTH]` (`execProgram`, `brush_executor.h:1093`).

## 2. Design decisions

1. **Brush source = the sculpt-mode `Paint` slot.** Our mode reuses
   `tool_settings.sculpt` (`Sculpt` struct) as its brush container rather
   than growing a new `Paint` slot in DNA — zero DNA change, brush assets /
   brush management UI work unchanged, and the semantic match is exact (it
   *is* sculpting). The addon reads `context.tool_settings.sculpt.brush`.
   Downside: vanilla sculpt mode and SculptCore mode share brush selection —
   acceptable (arguably desirable). Revisit only if a conflict emerges.
2. **Property mapping is a declarative table, not scattered code** — one
   module (`mapping.py`) with per-`sculpt_brush_type` entries: target
   SculptCore kernel + field map + fixed uniforms. Unknown/unsupported
   Blender brush types grey out in the UI (poll on the tool).
3. **Engine-only parameters get auto-generated addon properties.** At
   registration, walk the uniform manifest per kernel; for uniforms not
   covered by the Blender mapping, generate a `FloatProperty`/`IntProperty`
   (name/default/range from the manifest) inside
   `Brush.sculptcore` (`PointerProperty` → generated `PropertyGroup`).
   These serialize with the brush (asset-compatible) and draw in an
   auto-generated "SculptCore" panel section. Curve-typed engine props use
   Blender `CurveMapping` via a small owned `PropertyGroup` if needed later.
4. **Scene-level engine settings** (dyntopo tuning beyond Blender's fields,
   spatial-tree budgets, undo caps, draw budgets) live in
   `Scene.sculptcore` (`PropertyGroup`), not on `WindowManager` — they should
   save with the file.

## 3. The core mapping table (v1)

| Blender | SculptCore | Notes |
|---|---|---|
| `size`/`unprojected_size` (+unified) | `radius` | pixel→world unprojection done by the stroke operator per view (vanilla `paint_calc_object_space_radius` semantics reimplemented in addon). |
| `alpha` (+unified) | `strength` | |
| `spacing` (%) | `spacing` (fraction) | ÷100 |
| `hardness` | falloff LUT shaping | fold into curve bake |
| `curve_distance_falloff`(+preset) | `falloff_kind` + `falloff_curve[256]` | presets map 1:1 where names match (SMOOTH, LIN, SHARP, SPHERE, ROOT, …); CUSTOM bakes via `CurveMap.evaluate` into the LUT |
| `falloff_shape` (sphere/projected) | `falloff_shape` (Spherical/…) | |
| `flag & BRUSH_DIR_IN` / ctrl-invert | `invert` | |
| `autosmooth_factor` | `autosmooth` + host-built `[main, SMOOTH]` program | program construction in stroke op |
| `plane_offset`/`plane_trim`/`sculpt_plane` | `planeoff`/`planeSide` (+ plane kernel selection) | CLAY/SCRAPE/FILL |
| `jitter`(+absolute) | dab placement jitter in stroke sampler | host-side, engine unaware |
| `stroke_method`, `rate`, `input_samples`, `smooth_stroke_*` | host stroke sampler behavior | engine unaware |
| pressure flags | `pushDeviceInput` + `addPropDynamicByName("strength"/"radius"/…)` | per-dab |
| `sculpt_brush_type` | `SculptBrushes` kernel | DRAW→DRAW, CLAY→CLAY, GRAB→GRAB, SNAKE_HOOK→SNAKEHOOK, SMOOTH→SMOOTH/BSMOOTH, INFLATE, PINCH, SCRAPE, FILL, MASK, ELASTIC_DEFORM→KELVINLET, POSE→POSE, DRAW_SHARP→SHARP, PAINT→COLOR, ENHANCE_DETAILS→ENHANCE, LAYER→LAYERDRAW, … |
| `Sculpt.detail_size`/`constant_detail`/flags | dyntopo params to `applyDab`/`applyDynTopoDab` | |
| texture (`mtex`) | `tex_*` + `coord_space` | phase 2; requires pixel upload |
| cavity automask (`MeshAutomaskingSettings`, `DNA_scene_types.h:1199`) | `automask_cavity*` fields | phase 2 |

Unmapped-for-now Blender features (document in UI): cloth/boundary/multiplane
brush families, `topology_rake_factor`, front-face-only, accumulate,
original-normal, tip shape. Track as a parity checklist in taskList.

## 4. Change list (all addon-side)

| # | Module | Change | Size |
|---|---|---|---|
| M1 | `mapping.py` | Declarative table + `apply_brush(bl_brush, ups, sc_brush, executor)` called on stroke start (and on-the-fly for radius/strength during pressure). Curve bake w/ hardness fold-in; preset fast path. | M |
| M2 | `props.py` | Manifest walk → generated `PropertyGroup` per kernel + `Brush.sculptcore` / `Scene.sculptcore` registration; unregister cleanly. | M |
| M3 | `ui.py` | Brush panel: reuse Blender's brush UI layout conventions (`bl_ui.properties_paint_common` helpers where importable); auto-section for engine-only props from the manifest; dyntopo panel reading `Sculpt` fields. | M |
| M4 | Stroke operator hookup | Pressure→`pushDeviceInput`; program construction (autosmooth); pixel-radius unprojection. | S |
| M5 | Parity tests | Headless script: for each mapped brush type, one dab on a sphere via the addon vs. expected displacement direction/magnitude envelope (sanity, not pixel-perfect). | M |

## 5. Order of work

1. M1 minimal (DRAW only: radius/strength/spacing/invert/falloff) — first
   stroke works end-to-end (feeds addon-skeleton).
2. M2 manifest properties + M3 basic panel.
3. Broaden M1 table brush-by-brush; M4 pressure/autosmooth.
4. M5 parity harness; texture + automask phase 2.

## 6. Risks / open questions

- Unified-settings duplication: honor `UNIFIED_PAINT_SIZE/ALPHA` exactly or
  users get inconsistent radius between modes — read through one helper.
- Curve baking cost per stroke start is trivial (256 evals), but the RNA
  `CurveMapping` must be `initialized` (call `update()` after edits).
- Generated property groups must be stable across addon reloads (idempotent
  register; names derived from manifest names — sanitize collisions with
  Python keywords/builtins).
- Brush-asset save flow: engine props edited on a library brush need the
  user to re-save the asset; surface `has_unsaved_changes` state in the panel
  like vanilla brush UI does.
- Sharing `tool_settings.sculpt` (decision 1) means our mode's UI must not
  fight vanilla sculpt panels — panels poll on `context.mode` == our idname.
