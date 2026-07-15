# Python (`bpy`) Integration and Registration System

This document covers how Blender's Python layer registers classes, how
startup scripts and add-ons are loaded, and — in depth — how **paint/sculpt
mode tools are registered on the Python side** (tool system, brush-type
enums, keymaps, asset shelves). For the C-side DNA/RNA foundation these
mechanisms sit on top of, see [dna-rna-data-system.md](./dna-rna-data-system.md).
For the paint mode's C-side data structures, see [paint-mode-system.md](./paint-mode-system.md).

---

## 1. Class Registration (`bpy.utils.register_class`)

Registration bridges a Python class (subclassing `bpy.types.Operator`,
`Panel`, `Menu`, `PropertyGroup`, etc.) into the RNA type system so the rest
of Blender (C and Python) can call into it uniformly.

### C entry points (`source/blender/python/intern/bpy_rna.cc`)

| Function | Line | Purpose |
|----------|------|---------|
| `pyrna_register_class()` | 10368 | Python-exposed `register_class(cls)` |
| `pyrna_unregister_class()` | 10624 | Python-exposed `unregister_class(cls)` |
| `bpy_class_validate()` | 9776 | Entry point for validating a class before registering |
| `bpy_class_validate_recursive()` | 9544 | Walks the MRO checking required/optional methods (`poll`, `execute`, `draw`, …) declared per-RNA-type |
| `pyrna_deferred_register_class()` | 9488 | Registers `bpy.props.*` properties declared on the class onto the new `StructRNA` |
| `pyrna_deferred_register_class_from_type_hints()` | 9372 | Same, for annotation-style property declarations |
| `bpy_class_call()` | 9782 | The C→Python dispatch trampoline; looks up the live Python class via `RNA_struct_py_type_get(ptr->type)` |

### RNA-side glue (`source/blender/makesrna/intern/rna_wm.cc`)

For `Operator` specifically:
1. `rna_Operator_register()` (line 1802) is the callback invoked when a
   Python `Operator` subclass registers.
2. It validates the class, converts `bl_idname` (`"foo.bar"`) into the
   internal `FOO_OT_bar` form via `WM_operator_bl_idname()`.
3. Creates a `StructRNA` for it (`RNA_def_struct_ptr(..., dummy_ot.idname, RNA_Operator)`).
4. Wires RNA callbacks (`rna_operator_exec_cb`, `rna_operator_invoke_cb`, …).
5. Calls `WM_operatortype_append_ptr(BPY_RNA_operator_wrapper, &dummy_ot)` (line 1931)
   — this registers into the **same** `wmOperatorType` registry
   (`WM_operatortype_append`) used by built-in C operators. See
   [depsgraph-and-operators.md](./depsgraph-and-operators.md) for that registry.
6. `RNA_def_struct_register_funcs(srna, "rna_Operator_register", "rna_Operator_unregister", "rna_Operator_instance")`
   (line 2331) wires these callbacks onto the `RNA_Operator` struct itself
   (similar calls for operator macros at 2379, other registrable types at 3104).
7. `BPY_RNA_operator_wrapper` (declared `rna_wm.cc:1798`, implemented in
   `bpy_operator_wrap.cc`) is the trampoline C uses to invoke the live
   Python operator instance.

### Python-side base classes and wrappers

- `scripts/modules/_bpy_types.py` — metaclasses `_RNAMeta` (line 972) and
  `_RNAMetaPropGroup` (979) drive class-creation-time behavior (auto-handling
  annotations into `bl_rna`). Base classes: `Operator` (1095), `PropertyGroup`
  (1155), `Panel` (1282), `Menu` (1294), `UIList` (1286).
- `scripts/modules/bpy/utils/__init__.py` — `register_class`/`unregister_class`
  are re-exported directly from the `_bpy` C module (imported at lines 58/62).
  `register_classes_factory()` (line 1001) is a helper that generates a
  `register()`/`unregister()` pair from a list of classes — used pervasively
  by `bl_ui` and `bl_operators` submodules.

---

## 2. Startup Script and Add-on Loading

### Boot sequence

1. `BPY_python_start()` (`source/blender/python/intern/bpy_interface.cc:370`)
   — initializes the embedded Python interpreter.
2. `wm_init_exit.cc:426` runs
   `BPY_run_string_eval(C, imports, "bpy.utils.load_scripts_extensions()")`
   during window-manager init. (Unregister side: `wm_init_exit.cc:558` calls
   `addon_utils.disable_all()`.)
3. `load_scripts()` (`scripts/modules/bpy/utils/__init__.py:231`) iterates
   `script_paths()` × `_script_module_dirs = ("startup", "modules")` (line 78).
   For the `"startup"` subdirectory it walks every module via
   `modules_from_path()` (177), then calls `test_register()` (278) →
   `_register_module_call(mod)`, i.e. it calls each startup module's
   top-level `register()` function.
4. `scripts/startup/bl_ui/__init__.py` is one such startup module: it
   defines `_modules` — a long list of `properties_*`/`space_*` submodules
   (lines 13-98) — imports them all, and its own `register()`/`unregister()`
   (118/188) loop over each submodule's `classes` tuple calling
   `bpy.utils.register_class` / `unregister_class`.
5. `load_scripts_extensions()` (`bpy/utils/__init__.py:379`, called from
   `load_scripts()` at 343-344) separately loads add-ons/extensions, via
   `addon_utils._initialize_once()` / `addon_utils.reset_all()`.

### Add-on convention (`scripts/modules/addon_utils.py`)

Add-ons follow a module-level `register()`/`unregister()` convention:

- `enable(module_name)` (line 337) imports the module and calls `mod.register()` (543).
- `disable(module_name)` (line 566) calls `mod.unregister()` (594).
- `_initialize_once()` (56), `modules()` (256), `check()` (276),
  `reset_all()` (619), `disable_all()` (656), `module_bl_info()` (707, reads
  the addon's `bl_info` dict) round out the module.

There is no bundled `auto_load.py` helper in this checkout — core add-ons
under `scripts/addons_core/` register classes manually or via
`bpy.utils.register_classes_factory()` rather than a generic auto-loader.

---

## 3. Paint Mode Tool Registration — Python-Side Deep Dive

This section traces exactly how a paint/sculpt-mode tool (e.g. the Sculpt
"Draw" brush, or the "Mask" tool) becomes a clickable entry in the toolbar,
gets a keymap, and filters the asset shelf to compatible brushes.

### 3.1 `ToolDef` and `ToolSelectPanelHelper` (`scripts/startup/bl_ui/space_toolsystem_common.py`)

- `ToolDef` (lines 52-116) is a `namedtuple` describing one tool entry:
  `idname, label, icon, cursor, keymap, brush_type, data_block, operator, options`.
  Line 101-103 documents `brush_type`: *"Optional brush type this tool is
  limited to. Ignored if `'USE_BRUSHES'` isn't set in options."*
- `ToolDef.from_dict()` (120-150) fills defaults; a tuple `keymap` value is
  wrapped into a callback via `_keymap_fn_from_seq`.
- `ToolSelectPanelHelper` (191+) is the base class each space's tool panel
  subclasses (e.g. `VIEW3D_PT_tools_active`), implementing `tools_all()` and
  `tools_from_context()`.
- `_tools_flatten` / `_tools_flatten_with_dynamic` (276-315) expand tuples
  (click-drag tool groups) and callables (dynamic/conditional tool lists,
  e.g. `lambda context: ...` entries gated on `poll_dyntopo`/`poll_multires`).

### 3.2 Generating tool entries — NOT via enum expansion for brushes

`generate_from_enum_ex()` (`scripts/startup/bl_ui/space_toolsystem_toolbar.py:42-96`)
is a generic helper that builds one `ToolDef` per RNA enum item (reading
`type.bl_rna.properties[attr].enum_items_static_ui`). It still exists but is
used only once today, for legacy particle-edit brushes
(`_defs_particle.generate_from_brushes`, lines 1460-1468) — **not** for
sculpt/paint brushes.

Instead, each paint mode exposes a single generic brush tool:

```python
# space_toolsystem_toolbar.py ~3699-3724
_brush_tool  = ToolDef.from_dict(dict(idname="builtin.brush", options={'USE_BRUSHES'}, ...))
_sculpt_tool = ...  # same pattern for sculpt
_draw_tool   = ...  # same pattern for GP draw
```

Additional hand-written `ToolDef`s cover non-brush-list tools (mask/hide/trim
gestures, etc.), each optionally restricted to one brush type via
`brush_type=`, e.g. `_defs_sculpt.mask` (`space_toolsystem_toolbar.py:1472-1480`,
`brush_type='MASK'`).

The individual per-brush-type entries a user actually sees and picks
(Draw, Clay, Smooth, …) are **not** generated as separate `ToolDef`s at all —
they are populated at runtime by the **asset shelf** (§3.4), which lists
brush assets filtered by type.

### 3.3 Per-mode tool tables (`VIEW3D_PT_tools_active._tools`, `space_toolsystem_toolbar.py:3954-4126`)

| Object/paint mode key | Base tool | Def module | ~Line |
|---|---|---|---|
| `'SCULPT'` | `_sculpt_tool` | `_defs_sculpt.*` | 3954 |
| `'SCULPT_GREASE_PENCIL'` | `_sculpt_tool` | `_defs_grease_pencil_sculpt.clone` | 4017 |
| `'PAINT_TEXTURE'` | `_brush_tool` | `_defs_texture_paint.*` | 4028 |
| `'PAINT_VERTEX'` | `_brush_tool` | `_defs_vertex_paint.*` | 4043 |
| `'PAINT_WEIGHT'` | `_brush_tool` | `_defs_weight_paint.*` | 4056 |
| `'PAINT_GREASE_PENCIL'` | `_draw_tool` | `_defs_grease_pencil_paint.*` | 4081 |
| `'WEIGHT_GREASE_PENCIL'` | `_brush_tool` | `_defs_grease_pencil_weight.*` | 4095 |
| `'VERTEX_GREASE_PENCIL'` | `_brush_tool` | `_defs_grease_pencil_vertex.*` | 4103 |
| `'SCULPT_CURVES'` | `_sculpt_tool` | `_defs_curves_sculpt.*` | 4118 |

### 3.4 Brush type enums drive asset-shelf filtering (`source/blender/makesrna/intern/rna_brush.cc`)

The RNA enums that define brush "types" per mode:

| Enum items array | RNA property | ~Line |
|---|---|---|
| `rna_enum_brush_sculpt_brush_type_items` | `sculpt_brush_type` | 148-193 / 2656 |
| `rna_enum_brush_vertex_brush_type_items` | `vertex_brush_type` | 195-201 / 2663 |
| `rna_enum_brush_weight_brush_type_items` | `weight_brush_type` | 203-209 / 2669 |
| `rna_enum_brush_image_brush_type_items` | `image_brush_type` | 211+ / 2675 |
| GP variants (`gpencil_brush_type`, `gpencil_vertex_brush_type`, `gpencil_sculpt_brush_type`, `gpencil_weight_brush_type`) | — | 2682-2711 |
| `rna_enum_brush_curves_sculpt_brush_type_items` | `curves_sculpt_brush_type` | 314 / 2712 |

Python consumes these enums **indirectly**, not through
`generate_from_enum_ex`. `BrushAssetShelf.has_tool_with_brush_type()` /
`brush_type_poll()` (`scripts/startup/bl_ui/properties_paint_common.py:49,105`)
read `bpy.types.Brush.bl_rna.properties[cls.brush_type_prop].enum_items` and
match a `ToolDef.brush_type` string against an asset's stored brush-type
metadata. **This enum-matching is the real filtering mechanism** that makes
the asset shelf show only brushes compatible with the active tool.

### 3.5 Keymaps for paint tools (`ToolSelectPanelHelper.register()`, `space_toolsystem_common.py:518-549`)

For every `ToolDef` with a non-`None` `.keymap`, `_km_action_simple`
(494-506) builds
`km_idname = "{keymap_prefix} {context_descr}, {label}"`
and creates/populates the keymap via `keymap_init_from_data`.
`keymap_from_id()` / `description_from_id()` (~1225-1238 / ~1141) look tools
up by `idname` via `_tool_get_by_id`, resolving through
`_keymap_from_item()` (1241-1245) against `wm.keyconfigs.user`.

Importantly, `_brush_tool` / `_sculpt_tool` / `_draw_tool` and most
`_defs_*` brush-type-restricted entries leave `keymap=None` (the default in
`ToolDef.from_dict`, line 133) — **they have no tool-specific keymap**. The
actual left-click-to-paint bindings come from the paint mode's own keymap
(e.g. the "Sculpt", "Vertex Paint" keymaps defined in the default keyconfig
under `scripts/presets`), not from `ToolDef.keymap`. `ToolDef.keymap` is used
for tools that need dedicated bindings beyond the mode's stroke keymap (e.g.
gesture tools).

For the C-side operator/keymap registry these Python keyconfigs plug into,
see [depsgraph-and-operators.md](./depsgraph-and-operators.md).

### 3.6 Asset shelves (`scripts/startup/bl_ui/properties_paint_common.py`, `space_view3d.py`)

- `BrushAssetShelf` (`properties_paint_common.py:16`) is the shared mixin:
  `bl_activate_operator = "BRUSH_OT_asset_activate"` (see
  [brush-asset-system.md](./brush-asset-system.md) for what that operator
  does), plus class attributes `brush_type_prop` and `mode_prop`.
  `asset_poll()` / `brush_type_poll()` (75-124) filter displayed brush
  assets against the active `ToolDef.brush_type`.
- `View3DAssetShelf(BrushAssetShelf)` (`space_view3d.py:9201`) is the
  `SpaceView3D`-specific base.
- Per-mode subclasses, each pairing a `mode`, `mode_prop` (RNA bool checked
  in `asset_poll`), and `brush_type_prop` (the RNA enum name matched against
  `ToolDef.brush_type`):

  | Class | ~Line | `brush_type_prop` |
  |---|---|---|
  | `VIEW3D_AST_brush_sculpt` | 9210 | `sculpt_brush_type` |
  | `VIEW3D_AST_brush_sculpt_curves` | 9216 | `curves_sculpt_brush_type` |
  | `VIEW3D_AST_brush_vertex_paint` | 9222 | `vertex_brush_type` |
  | `VIEW3D_AST_brush_weight_paint` | 9228 | `weight_brush_type` |
  | `VIEW3D_AST_brush_texture_paint` | 9234 | `image_brush_type` |
  | `VIEW3D_AST_brush_gpencil_paint` | 9250 | `gpencil_brush_type` |
  | `VIEW3D_AST_brush_gpencil_sculpt` | 9256 | `gpencil_sculpt_brush_type` |
  | `VIEW3D_AST_brush_gpencil_vertex` | 9262 | `gpencil_vertex_brush_type` |
  | `VIEW3D_AST_brush_gpencil_weight` | 9268 | `gpencil_weight_brush_type` |

  All are appended to the module's `classes` tuple and registered generically
  at the bottom of `space_view3d.py` via
  `for cls in classes: register_class(cls)` (lines 9548-9550).

### 3.7 Full picture — from enum to click

```
RNA brush-type enum (rna_brush.cc)
        │  e.g. sculpt_brush_type = 'CLAY_STRIPS'
        ▼
Brush asset's stored brush-type metadata (set when the asset was saved)
        │
        ▼
BrushAssetShelf.brush_type_poll()  (properties_paint_common.py)
  filters visible assets in the shelf against ToolDef.brush_type
        │
        ▼
Asset shelf shows compatible brushes for the active tool
        │  user clicks a brush asset
        ▼
BRUSH_OT_asset_activate  (see brush-asset-system.md)
        │
        ▼
WM_toolsystem_activate_brush_and_tool  (wm_toolsystem.cc)
  switches tool if the brush's type doesn't match builtin.brush/_sculpt_tool
        │
        ▼
ToolDef.keymap resolution (usually None for brush tools — mode keymap applies)
        │
        ▼
Stroke operator runs (PAINT_OT_vertex_paint, SCULPT stroke, etc.)
```

For the underlying `Paint`/`Brush` DNA structs and stroke pipeline this
feeds into, see [paint-mode-system.md](./paint-mode-system.md).

---

## 4. Other Notable Python Layer Pieces

- `scripts/modules/rna_info.py` — introspects the RNA database for doc
  generation (`doc/python_api`) and consistency checks.
- `scripts/modules/bpy_extras/` — shared helper modules for importers,
  exporters, view3d utilities, image utilities used by many add-ons.
- `bpy.props.*` (e.g. `FloatProperty`, `PointerProperty`) are the Python
  functions used to declare RNA properties inline on a class; they are
  processed by `pyrna_deferred_register_class()` at registration time (§1).
- Operators defined purely in Python (`bpy.types.Operator` subclasses in
  `scripts/startup/bl_operators/`) coexist in the *same* `wmOperatorType`
  registry as C operators — from the caller's perspective (`bpy.ops.*`,
  keymaps, menus) there is no difference between a Python- and C-implemented
  operator.

---

## Key File:Line Reference Table

| Topic | File | Line |
|---|---|---|
| `pyrna_register_class` | `source/blender/python/intern/bpy_rna.cc` | 10368 |
| `pyrna_unregister_class` | `source/blender/python/intern/bpy_rna.cc` | 10624 |
| `bpy_class_validate_recursive` | `source/blender/python/intern/bpy_rna.cc` | 9544 |
| `pyrna_deferred_register_class` | `source/blender/python/intern/bpy_rna.cc` | 9488 |
| `bpy_class_call` (C→Py dispatch) | `source/blender/python/intern/bpy_rna.cc` | 9782 |
| `load_scripts()` | `scripts/modules/bpy/utils/__init__.py` | 231 |
| `load_scripts_extensions()` | `scripts/modules/bpy/utils/__init__.py` | 379 |
| `register_classes_factory()` | `scripts/modules/bpy/utils/__init__.py` | 1001 |
| `BPY_python_start` | `source/blender/python/intern/bpy_interface.cc` | 370 |
| WM calls `load_scripts_extensions` | `source/blender/windowmanager/intern/wm_init_exit.cc` | 426 |
| `bl_ui` module list + `register()` | `scripts/startup/bl_ui/__init__.py` | 13, 118 |
| `addon_utils.enable()` / `mod.register()` | `scripts/modules/addon_utils.py` | 337 / 543 |
| `rna_Operator_register` | `source/blender/makesrna/intern/rna_wm.cc` | 1802 |
| `WM_operatortype_append_ptr(...)` call | `source/blender/makesrna/intern/rna_wm.cc` | 1931 |
| `RNA_def_struct_register_funcs` (Operator) | `source/blender/makesrna/intern/rna_wm.cc` | 2331 |
| `Operator`/`Panel`/`Menu`/`PropertyGroup` base classes | `scripts/modules/_bpy_types.py` | 1095 / 1282 / 1294 / 1155 |
| `generate_from_enum_ex` | `scripts/startup/bl_ui/space_toolsystem_toolbar.py` | 42-96 |
| `_brush_tool`/`_sculpt_tool`/`_draw_tool` | `scripts/startup/bl_ui/space_toolsystem_toolbar.py` | 3699-3724 |
| `VIEW3D_PT_tools_active._tools` | `scripts/startup/bl_ui/space_toolsystem_toolbar.py` | 3954-4126 |
| `ToolDef` namedtuple | `scripts/startup/bl_ui/space_toolsystem_common.py` | 52-116 |
| `ToolSelectPanelHelper.register()` (keymap wiring) | `scripts/startup/bl_ui/space_toolsystem_common.py` | 518-549 |
| Brush-type RNA enums | `source/blender/makesrna/intern/rna_brush.cc` | 148-314 |
| `BrushAssetShelf` | `scripts/startup/bl_ui/properties_paint_common.py` | 16 |
| `View3DAssetShelf` + per-mode subclasses | `scripts/startup/bl_ui/space_view3d.py` | 9201-9268 |
