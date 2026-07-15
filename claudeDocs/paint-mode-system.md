# Paint Mode System

This document surveys how all of Blender's paint and sculpt modes are implemented — data structures, mode entry/exit, stroke execution, brush dispatch, and the Python UI layer. Sculpt mode is considered a paint mode throughout because it shares the same `Paint` base struct and stroke infrastructure.

For brush asset storage and resolution, see **[brush-asset-system.md](./brush-asset-system.md)**.
For the deep dive into Python-side tool/keymap/asset-shelf registration, see **[python-bpy-integration.md](./python-bpy-integration.md)**.

---

## Modes at a Glance

| Mode | Object type | Toggle operator | `PaintMode` enum | `eObjectMode` flag |
|------|-------------|-----------------|------------------|---------------------|
| Sculpt | Mesh | `SCULPT_OT_sculptmode_toggle` | `Sculpt` (0) | `OB_MODE_SCULPT` |
| Vertex Paint | Mesh | `PAINT_OT_vertex_paint_toggle` | `Vertex` (1) | `OB_MODE_VERTEX_PAINT` |
| Weight Paint | Mesh | `PAINT_OT_weight_paint_toggle` | `Weight` (2) | `OB_MODE_WEIGHT_PAINT` |
| Texture Paint (3D) | Mesh | `PAINT_OT_texture_paint_toggle` | `Texture3D` (3) | `OB_MODE_TEXTURE_PAINT` |
| Texture Paint (2D) | — (Image Editor) | same toggle | `Texture2D` (4) | `OB_MODE_TEXTURE_PAINT` |
| GP Draw/Paint | Grease Pencil | `GREASE_PENCIL_OT_paintmode_toggle` | `GPencil` (6) | `OB_MODE_PAINT_GREASE_PENCIL` |
| GP Vertex Paint | Grease Pencil | `GREASE_PENCIL_OT_vertexmode_toggle` | `VertexGPencil` (7) | `OB_MODE_VERTEX_GREASE_PENCIL` |
| GP Sculpt | Grease Pencil | `GREASE_PENCIL_OT_sculptmode_toggle` | `SculptGPencil` (8) | `OB_MODE_SCULPT_GREASE_PENCIL` |
| GP Weight Paint | Grease Pencil | `GREASE_PENCIL_OT_weightmode_toggle` | `WeightGPencil` (9) | `OB_MODE_WEIGHT_GREASE_PENCIL` |
| Curves Sculpt | Curves | `CURVES_OT_sculptmode_toggle` | `SculptCurves` (10) | `OB_MODE_SCULPT_CURVES` |

`PaintMode` is declared in `source/blender/blenkernel/BKE_paint_types.hh` (~line 21).
`eObjectMode` flags are in `source/blender/makesdna/DNA_object_enums.h` (~line 18); the macro `OB_MODE_ALL_PAINT` (~line 47) masks all brush-based modes.

---

## DNA Layer — Paint Structs

All paint modes share a common base struct. Mode-specific structs embed it as their first member so they can be safely cast to `Paint *`.

### `Paint` base struct (`source/blender/makesdna/DNA_scene_types.h`, ~line 1208)

```c
struct Paint {
  Brush *brush;                              /* active brush pointer */
  AssetWeakReference *brush_asset_reference; /* persisted weak ref for reload */
  ToolSystemBrushBindings tool_brush_bindings;
  Palette *palette;
  CurveMapping *cavity_curve;
  ePaintFlags flags;
  ePaintSymmetryFlags symmetry_flags;
  UnifiedPaintSettings unified_paint_settings;
  bke::PaintRuntime *runtime;
};
```

`UnifiedPaintSettings` (`~line 1150`) holds shared brush size/strength/color that can be linked across modes via `eUnifiedPaintSettingsFlags`.

### Mode-specific structs (all in `DNA_scene_types.h`)

| Struct | ~Line | Extra notable fields |
|--------|-------|----------------------|
| `Sculpt` | 1480 | `eSculptFlags flags`, `float detail_size`, `float constant_detail`, `float gravity_factor`, `Object *gravity_object`, `eSculptTransformMode transform_mode` |
| `VPaint` | 1573 | (minimal — reused for both vertex color and weight paint, distinguished at runtime) |
| `ImagePaintSettings` | 1297 | `Image *stencil`, `Image *clone`, `Image *canvas`, `float clone_alpha`, `int mode` (material vs. image) |
| `GpPaint` | 1538 | `int mode` (materials or vertex color) |
| `GpVertexPaint` | 1546 | — |
| `GpSculptPaint` | 1553 | — |
| `GpWeightPaint` | 1560 | — |
| `CurvesSculpt` | 1525 | — |

`UvSculpt` (~line 1529) is separate: it is a 2D sculpt overlay in the UV editor and does **not** embed `Paint`.

All instances are stored in `ToolSettings` (accessed as `scene->toolsettings`):
- `ts->sculpt` — mesh sculpt
- `ts->vpaint` / `ts->wpaint` — vertex/weight paint (both `VPaint *`)
- `ts->imapaint` — texture paint (inline, not a pointer)
- `ts->gp_paint`, `ts->gp_vertexpaint`, `ts->gp_sculptpaint`, `ts->gp_weightpaint`
- `ts->curves_sculpt`

### Runtime state — `PaintRuntime` (`source/blender/blenkernel/BKE_paint_types.hh`, ~line 44)

```cpp
struct bke::PaintRuntime : NonCopyable, NonMovable {
  bool initialized;
  uint16_t ob_mode;           /* current eObjectMode */
  PaintMode paint_mode;
  float2 last_rake;           /* for rake angle tracking */
  float brush_rotation;
  bool stroke_active;
  float3 last_location;
  float pixel_radius;
  /* cursor overlay data: draw_anchored, anchored_size, overlap_factor … */
  AssetWeakReference *previous_active_brush_reference; /* brush toggle */
};
```

---

## Key BKE Paint Functions

Declared in `source/blender/blenkernel/BKE_paint.hh` (~line 178):

| Function | Purpose |
|----------|---------|
| `BKE_paint_get_active_from_paintmode(Scene*, PaintMode)` | Return `Paint*` for a given mode |
| `BKE_paint_get_active(Main&, Scene*, ViewLayer*)` | Return active `Paint*` from current object mode |
| `BKE_paint_get_active_from_context(bContext*)` | Context-aware active paint lookup |
| `BKE_paintmode_get_active_from_context(bContext*)` | Active `PaintMode` enum value |
| `BKE_paintmode_get_from_tool(bToolRef*)` | `PaintMode` for a given tool reference |
| `BKE_paint_init(Main*, Scene*, PaintMode, uint)` | Initialize/reset a paint session |

---

## Mode Entry and Exit

### Central dispatcher (`source/blender/editors/object/object_modes.cc`)

- `object_mode_op_string(eObjectMode)` (~line 62) maps mode enum values to their toggle operator names.
- `mode_compat_test(Object*, eObjectMode)` (~line 103) checks whether a mode is valid for an object type.
- `mode_compat_set()` (~line 156) performs a safe mode switch with undo support.
- `mode_set_ex()` (~line 186) is the lower-level setter.

Valid object-type / mode pairings:
- **Mesh**: Edit, Sculpt, Vertex Paint, Weight Paint, Texture Paint, Particle Edit
- **Grease Pencil**: Edit, GP Draw/Paint, GP Sculpt, GP Vertex Paint, GP Weight Paint
- **Curves**: Edit, Curves Sculpt
- **Armature**: Edit, Pose

### Per-mode toggle operators

**Mesh paint modes** — `source/blender/editors/sculpt_paint/mesh/`:
- `sculpt_ops.cc` ~line 595: `sculpt_mode_toggle_exec()` / `SCULPT_OT_sculptmode_toggle`
- `paint_vertex.cc` ~line 851: `PAINT_OT_vertex_paint_toggle`
- `paint_weight.cc` ~line 1723: `PAINT_OT_weight_paint_toggle`
- `paint_image.cc` ~line 828: `PAINT_OT_texture_paint_toggle`

**Grease Pencil** — `source/blender/editors/grease_pencil/intern/grease_pencil_modes.cc`:
- `paintmode_toggle_exec()` ~line 56 → `GREASE_PENCIL_OT_paintmode_toggle` ~line 118
- `sculptmode_toggle_exec()` ~line 148 → `GREASE_PENCIL_OT_sculptmode_toggle` ~line 220
- `weightmode_toggle_exec()` ~line 243 → `GREASE_PENCIL_OT_weightmode_toggle` ~line 307
- `vertexmode_toggle_exec()` ~line 340 → `GREASE_PENCIL_OT_vertexmode_toggle` ~line 404

All GP toggles call `BKE_paint_init()` with the appropriate `PaintMode`.

**Curves Sculpt** — `source/blender/editors/sculpt_paint/curves/sculpt_ops.cc`:
- `curves_sculptmode_enter()` ~line 316, `curves_sculptmode_exit()` ~line 341
- `curves_sculptmode_toggle_exec()` ~line 347 → `CURVES_OT_sculptmode_toggle` ~line 376

---

## Stroke Execution System

All modes share a single stroke pipeline implemented in `source/blender/editors/sculpt_paint/paint_stroke.cc`.

### `PaintStroke` (`source/blender/editors/sculpt_paint/paint_intern.hh`, ~line 109)

```cpp
struct PaintStroke : NonCopyable, NonMovable {
  Paint *paint;
  Brush *brush;
  std::unique_ptr<PaintModeData> mode_data_; /* mode-specific state */
  PaintSample samples_[];                    /* input sample buffer */
  float stroke_distance_;
  bool stroke_started_;
  /* callbacks set by mode: */
  StrokeTestStart   test_start;
  StrokeGetLocation get_location;
  StrokeUpdateStep  update_step;
  StrokeDone        done;
};
```

The modal loop (`PaintStroke::modal()`, ~line 198) drives the whole pipeline:
1. `test_start` — confirm stroke should begin (e.g. check for mesh under cursor)
2. `get_location` — project cursor to 3D hit point
3. `update_step` — apply the brush at the current sample
4. `done` — finalize and push undo

### Mode-specific state (`PaintModeData` subclasses)

Each mode provides a `PaintModeData` implementation:

**`VPaintData`** (`mesh/paint_vertex.cc`, ~line 903)
```cpp
struct VPaintData : PaintModeData {
  ViewContext vc;
  AttrDomain domain;           /* FACE / CORNER / POINT */
  bke::AttrType type;
  ColorPaint4f paintcol;
  bool is_texbrush;
  GArray<> prev_colors;        /* smear anti-feedback */
  GArray<> stroke_buffer;
};
```

**`WPaintData`** (`mesh/paint_weight.cc`, ~line 102)
```cpp
struct WPaintData : PaintModeData {
  ViewContext vc;
  WeightPaintGroupData active, mirror;
  const bool *vgroup_validmap;
  const bool *lock_flags;
  float *precomputed_weight;
  Array<float> alpha_weight;       /* per-vertex accumulation */
  Array<MDeformVert> dvert_prev;   /* weight backup for non-accumulate brushes */
};
```

**`PaintOperation`** (`mesh/paint_image_ops_paint.cc`, ~line 229)
```cpp
struct PaintOperation : PaintModeData {
  AbstractPaintMode *mode;
  void *stroke_handle;
  float prevmouse[2], startmouse[2];
  wmPaintCursor *cursor;
  ViewContext vc;
};
```

**`GreasePencilStrokeOperation`** (`grease_pencil/grease_pencil_intern.hh`, ~line 43)
```cpp
class GreasePencilStrokeOperation {
  virtual void on_stroke_begin(bContext &C, const InputSample &start) = 0;
  virtual void on_stroke_extended(bContext &C, const InputSample &sample) = 0;
  virtual void on_stroke_done(bContext &C) = 0;
};
```
Concrete implementations are created per-brush-type in the files listed in the Grease Pencil section below.

**`CurvesSculptStrokeOperation`** (`curves/sculpt_intern.hh`, ~line 62) — same virtual interface pattern as GP. Factories: `new_add_operation()`, `new_comb_operation()`, `new_delete_operation()`, `new_snake_hook_operation()`, etc. (~lines 69–89).

### Stroke operators (top-level per mode)

| Mode | Operator | File |
|------|----------|------|
| Sculpt | _(stroke driven internally)_ | `mesh/sculpt.cc` |
| Vertex Paint | `PAINT_OT_vertex_paint` | `mesh/paint_vertex.cc` ~2138 |
| Weight Paint | `PAINT_OT_weight_paint` | `mesh/paint_weight.cc` ~1993 |
| Texture Paint | `PAINT_OT_image_paint` | `mesh/paint_image_ops_paint.cc` ~625 |
| GP (all) | `GREASE_PENCIL_OT_brush_stroke` | `grease_pencil/paint.cc` |
| Curves Sculpt | `SCULPT_CURVES_OT_brush_stroke` | `curves/sculpt_ops.cc` |

---

## Mesh Sculpt Mode

The sculpt mode is the largest and most complex of the modes.

### Session state — `SculptSession` (`source/blender/blenkernel/BKE_paint.hh`, ~line 382)

```cpp
struct SculptSession : NonCopyable, NonMovable {
  KeyBlock *shapekey_active;
  BMesh *bm;                            /* dynamic topology mesh */
  BMLog *bm_log;                        /* undo log for dyntopo */
  MultiresModifierData *multires_modifier;
  SubdivCCG *subdiv_ccg;                /* subdivision CCG */
  std::unique_ptr<bke::pbvh::Tree> pbvh; /* BVH acceleration structure */
  ed::sculpt_paint::StrokeCache *cache;  /* active stroke state */
  ed::sculpt_paint::filter::Cache *filter_cache;
  ed::sculpt_paint::expand::Cache *expand_cache;
  float3 cursor_location, cursor_normal, cursor_view_normal;
  float3 pivot_pos; float4 pivot_rot; float3 pivot_scale;
  eObjectMode mode_type;
};
```

### Brush application (`source/blender/editors/sculpt_paint/mesh/sculpt.cc`)

- `do_brush_action()` (~line 3325) — dispatches to the appropriate brush implementation
- `sculpt_apply_texture()` (~line 2476) — applies brush texture mask

### Brush implementations (`source/blender/editors/sculpt_paint/mesh/brushes/`)

Each brush type is in its own file:
`clay.cc`, `crease.cc`, `draw.cc`, `draw_sharp.cc`, `draw_vector_displacement.cc`, `flatten.cc`, `fill.cc`, `grab.cc`, `grab_silhouette.cc`, `grab_tracing.cc`, `rotate.cc`, `scrape.cc`, `slide_relax.cc`, `smooth.cc`, `snake_hook.cc`, `thumb.cc`, `topology_rake.cc`, `boundary.cc`, `cloth_sim.cc`

All implement the interface declared in `brushes/brushes.hh`.

---

## Mesh Vertex and Weight Paint

Both modes reuse the `VPaint` struct (stored as `ts->vpaint` / `ts->wpaint`) and share much of `paint_vertex.cc`. Vertex paint writes to face corner or point color attributes; weight paint writes to `MDeformVert` weight arrays.

Key source files:
- `source/blender/editors/sculpt_paint/mesh/paint_vertex.cc` — vertex paint stroke, color sampling, smear
- `source/blender/editors/sculpt_paint/mesh/paint_weight.cc` — weight paint, normalize, multi-paint logic
- `source/blender/editors/sculpt_paint/mesh/paint_vertex_color_ops.cc` — vertex color utility operators (fill, invert, etc.)
- `source/blender/editors/sculpt_paint/mesh/paint_vertex_weight_ops.cc` — weight utility operators (assign, remove, etc.)
- `source/blender/editors/sculpt_paint/mesh/paint_vertex_weight_utils.cc` — normalization helpers

---

## Texture / Image Paint

The mode can operate in 3D (projection painting onto a mesh) or 2D (directly editing an `Image` in the Image Editor). Both share `ImagePaintSettings` and `PaintOperation`.

Key source files:
- `source/blender/editors/sculpt_paint/mesh/paint_image.cc` — mode toggle, canvas management, UV unwrap
- `source/blender/editors/sculpt_paint/mesh/paint_image_ops.cc` — operators (fill, grab clone, etc.)
- `source/blender/editors/sculpt_paint/mesh/paint_image_ops_paint.cc` — stroke apply, `PaintOperation`
- `source/blender/editors/sculpt_paint/mesh/paint_image_proj.cc` — 3D projection logic

---

## Grease Pencil Paint Modes

All four GP modes live in `source/blender/editors/grease_pencil/intern/` and share the `GreasePencilStrokeOperation` virtual interface.

| Sub-system | Files |
|------------|-------|
| Draw/Paint | `paint.cc`, `paint_common.cc`, `paint_cursor.cc`, `draw_ops.cc`, `erase.cc`, `fill.cc`, `interpolate.cc`, `trace.cc`, `trace_util.cc` |
| Sculpt | `sculpt_clone.cc`, `sculpt_grab.cc`, `sculpt_pinch.cc`, `sculpt_push.cc`, `sculpt_randomize.cc`, `sculpt_smooth.cc`, `sculpt_strength.cc`, `sculpt_thickness.cc`, `sculpt_twist.cc` |
| Vertex Paint | `vertex_paint.cc`, `vertex_average.cc`, `vertex_blur.cc`, `vertex_replace.cc`, `vertex_smear.cc` |
| Weight Paint | `weight_draw.cc`, `weight_average.cc`, `weight_blur.cc`, `weight_smear.cc` |

Stroke parameters for all GP operations are passed via `GreasePencilStrokeParams` (`grease_pencil_intern.hh`, ~line 99).

---

## Curves Sculpt

Operates on the `Curves` object type. The session state is lightweight compared to mesh sculpt (no PBVH or dyntopo); each brush is a self-contained `CurvesSculptStrokeOperation`.

Source directory: `source/blender/editors/sculpt_paint/curves/`

| File | Brush |
|------|-------|
| `sculpt_add.cc` | Add curves |
| `sculpt_comb.cc` | Comb |
| `sculpt_delete.cc` | Delete |
| `sculpt_density.cc` | Density |
| `sculpt_grow_shrink.cc` | Grow/Shrink |
| `sculpt_pinch.cc` | Pinch |
| `sculpt_puff.cc` | Puff |
| `sculpt_selection_paint.cc` | Selection paint |
| `sculpt_slide.cc` | Slide |
| `sculpt_smooth.cc` | Smooth |
| `sculpt_snake_hook.cc` | Snake Hook |
| `sculpt_brush.cc` | Base brush utilities |

---

## Shared Infrastructure

### Paint cursor (`source/blender/editors/sculpt_paint/paint_cursor.cc`)
Draws the brush circle, symmetry markers, and overlay feedback for all modes.

### Paint stroke utilities (`source/blender/editors/sculpt_paint/paint_stroke.cc`)
Houses `PaintStroke` and the complete modal input loop shared by all modes.

### Paint curve (`source/blender/editors/sculpt_paint/paint_curve.cc`, `paint_curve_undo.cc`)
The falloff spline editor used for custom brush falloff curves.

### Sample color (`source/blender/editors/sculpt_paint/paint_sample_color.cc`)
Eye-dropper operator (`PAINT_OT_sample_color`) used in vertex, texture, and GP paint modes.

### Brush asset ops (`source/blender/editors/sculpt_paint/brush_asset_ops.cc`)
`BRUSH_OT_asset_activate`, `BRUSH_OT_asset_save`, `BRUSH_OT_asset_delete`, `BRUSH_OT_asset_revert`. See [brushAssetSystem.md](./brushAssetSystem.md) for details.

### Operator registration (`source/blender/editors/sculpt_paint/paint_ops.cc`)
Registers all paint/sculpt operators with the window manager and maps them to keymap entries.

---

## Python UI Layer

### Common brush panels (`scripts/startup/bl_ui/properties_paint_common.py`)

This file defines the panel mix-ins shared across all paint mode toolbars:

| Class | Purpose |
|-------|---------|
| `BrushAssetShelf` (~line 16) | Base for brush asset shelf panels |
| `UnifiedPaintPanel` (~line 216) | Draw unified size/strength/color controls |
| `BrushPanel` (~line 368) | Brush settings panel base |
| `BrushSelectPanel` (~line 374) | Brush picker |
| `ColorPalettePanel` (~line 416) | Color palette |
| `ClonePanel` (~line 451) | Clone/stencil image controls |
| `TextureMaskPanel` (~line 511) | Texture mask settings |
| `StrokePanel` (~line 553) | Stroke method (dot/drag/airbrush/etc.) |
| `SmoothStrokePanel` (~line 640) | Stabilizer settings |
| `FalloffPanel` (~line 675) | Brush falloff curve |
| `DisplayPanel` (~line 732) | Cursor display options |

### Asset shelves (`scripts/startup/bl_ui/space_view3d.py`)

Each mode has its own `AssetShelf` subclass that filters to mode-appropriate brushes:

| Class (~line) | Mode |
|---------------|------|
| `VIEW3D_AST_brush_sculpt` (~9199) | Mesh Sculpt |
| `VIEW3D_AST_brush_sculpt_curves` (~9205) | Curves Sculpt |
| `VIEW3D_AST_brush_vertex_paint` (~9211) | Vertex Paint |
| `VIEW3D_AST_brush_weight_paint` (~9217) | Weight Paint |
| `VIEW3D_AST_brush_texture_paint` (~9223) | Texture Paint |
| `VIEW3D_AST_brush_gpencil_paint` (~9239) | GP Draw |
| `VIEW3D_AST_brush_gpencil_sculpt` (~9245) | GP Sculpt |

### Mode menus (header bar, `space_view3d.py`)

| Class (~line) | Mode |
|---------------|------|
| `VIEW3D_MT_sculpt` (~3684) | Mesh Sculpt |
| `VIEW3D_MT_paint_vertex` (~3500) | Vertex Paint |
| `VIEW3D_MT_paint_weight` (~3624) | Weight Paint |
| `VIEW3D_MT_sculpt_curves` (~3886) | Curves Sculpt |
| `VIEW3D_MT_paint_grease_pencil` (~2227) | GP Draw |

### Overlay panels (N-panel Overlays, `space_view3d.py`)

| Class (~line) | Mode |
|---------------|------|
| `VIEW3D_PT_overlay_sculpt` (~7558) | Mesh Sculpt |
| `VIEW3D_PT_overlay_texture_paint` (~7681) | Texture Paint |
| `VIEW3D_PT_overlay_vertex_paint` (~7703) | Vertex Paint |
| `VIEW3D_PT_overlay_weight_paint` (~7727) | Weight Paint |

### Toolbar panels (`scripts/startup/bl_ui/space_view3d_toolbar.py`)

| Class (~line) | Content |
|---------------|---------|
| `VIEW3D_PT_tools_brush_select` (~318) | Brush picker |
| `VIEW3D_PT_tools_brush_settings` (~323) | Brush parameters |
| `VIEW3D_PT_tools_brush_color` (~373) | Color controls |
| `VIEW3D_PT_tools_weightpaint_*` (~1171+) | Weight paint tool panels |
| `VIEW3D_PT_tools_vertexpaint_*` (~1232+) | Vertex paint tool panels |
| `VIEW3D_PT_tools_imagepaint_*` (~1279+) | Image paint tool panels |

### Tool registration (`scripts/startup/bl_ui/space_toolsystem_toolbar.py`)

`generate_from_enum_ex()` (~line 42) auto-generates tool entries from brush-type enum values. `ToolDef` (from `space_toolsystem_common`) describes each tool. This drives the N-panel and workspace tool selector for all paint modes.

---

## Data Flow Summary

```
User clicks brush / switches tool
         │
         ▼
BRUSH_OT_asset_activate  (brush_asset_ops.cc)
         │
         ▼
WM_toolsystem_activate_brush_and_tool  (wm_toolsystem.cc)
  ├─ Resolve AssetWeakReference → Brush*
  ├─ Switch tool if brush type differs
  └─ Update ToolSystemBrushBindings
         │
         ▼
User draws stroke
         │
         ▼
Mode stroke operator  (e.g. PAINT_OT_vertex_paint)
         │
         ▼
PaintStroke modal loop  (paint_stroke.cc)
  ├─ test_start callback
  ├─ get_location callback → 3D hit point
  └─ update_step callback
         │
         ▼
Mode PaintModeData  (VPaintData / WPaintData / PaintOperation / …)
  └─ applies brush to mesh / image / GP strokes / curves
         │
         ▼
Undo push  (ed::sculpt_paint or BKE_undo_push)
```
