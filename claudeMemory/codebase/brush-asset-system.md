# Brush Asset System

This document explains how brushes are stored, resolved, and activated in Blender's paint/sculpt modes. The system was redesigned to support asset libraries and cross-session brush memory.

See also **[./paint-mode-system.md](./paint-mode-system.md)** for the full paint/sculpt mode survey and **[./python-bpy-integration.md](./python-bpy-integration.md)** for how the Python-side asset shelves consume this data.

---

## Overview

The brush asset system replaces simple `Brush*` pointers with **weak asset references** that survive file reloads and can point into external asset libraries. It also adds **per-tool-type brush memory**: switching tools remembers which brush was last used for each brush type.

---

## Data Structures

### `Paint` struct (`source/blender/makesdna/DNA_scene_types.h`, ~line 1219)

```c
struct Paint {
  Brush *brush;                              /* active brush pointer, may be NULL */
  AssetWeakReference *brush_asset_reference; /* persisted reference, used to restore brush */
  ToolSystemBrushBindings tool_brush_bindings;
  /* ... */
};
```

The two references serve different roles:
- `brush` — fast runtime pointer, populated after resolution; cleared to `NULL` when the file is first loaded.
- `brush_asset_reference` — persisted identifier used to re-resolve `brush` after a file open or library reload.

### `ToolSystemBrushBindings` (`DNA_scene_types.h`, ~line 1158)

```c
struct ToolSystemBrushBindings {
  AssetWeakReference *main_brush_asset_reference;
  ListBaseT<NamedBrushAssetReference> active_brush_per_brush_type;
};

struct NamedBrushAssetReference {   /* a ListBase node: has next/prev pointers */
  const char *name;                        /* brush-type string ID, e.g. "DRAW" */
  AssetWeakReference *brush_asset_reference;
};
```

`active_brush_per_brush_type` is the list that makes brush memory work: each entry maps a brush-type name to the last brush the user activated for that type. When the user switches to a different tool, the system looks up this list to restore the correct brush.

### `AssetWeakReference` (`source/blender/makesdna/DNA_asset_types.h`, ~line 197)

```c
struct AssetWeakReference {
  char _pad[6];
  eAssetLibraryType asset_library_type;    /* LOCAL, ESSENTIALS, CUSTOM, … */
  const char *asset_library_identifier;   /* further location info for CUSTOM libraries */
  const char *relative_asset_identifier;  /* path within that library */
};
```

Called "weak" because it can break if the library path changes, the asset is moved, or the file is opened on a different machine. The resolution code handles all of these failure modes gracefully.

---

## How the Active Brush Is Resolved

Resolution happens in priority order:

1. **In-memory pointer** (`paint->brush`): if already set, used as-is (fast path).
2. **Tool-type binding**: look up `active_brush_per_brush_type` for the current tool's brush type.
3. **Main brush binding** (`tool_brush_bindings.main_brush_asset_reference`): fallback when no type-specific entry exists.
4. **Essentials library default** (`BKE_paint_brush_type_default_reference()`): guaranteed fallback built into Blender.

The resolution function is `paint_brush_update_from_asset_reference()` in `source/blender/blenkernel/intern/paint.cc` (~line 643). It is called post-link during file open (skipped while `bmain->is_locked_for_linking` is true) and after tool switches.

---

## Brush Activation Flow

When a user clicks a brush in the asset browser or shelf:

1. `BRUSH_OT_asset_activate` operator runs; its `exec` callback is `brush_asset_activate_exec()` (`source/blender/editors/sculpt_paint/brush_asset_ops.cc`, ~line 53; the operator type registration `BRUSH_OT_asset_activate()` is at ~line 127).
2. An `AssetWeakReference` is created from the selected asset's metadata (`asset->make_weak_reference()`).
3. If the same brush is clicked twice (toggle mode, `use_toggle`), the system restores the previous brush instead.
4. `WM_toolsystem_activate_brush_and_tool()` is called (`source/blender/windowmanager/intern/wm_toolsystem.cc`, ~line 301):
   - The brush was already resolved from the weak reference via `bke::asset_edit_id_from_weak_reference()` in the operator.
   - If the brush type doesn't match the current tool, switches to a compatible tool.
   - Calls `BKE_paint_brush_set(Main*, Paint*, const AssetWeakReference&)` (~line 702 in `paint.cc`).
   - Updates the tool-brush bindings (`toolsystem_brush_type_binding_update()`, ~line 270).
5. The previous brush reference is saved in `PaintRuntime::previous_active_brush_reference` (via `BKE_paint_previous_asset_reference_set()`) for toggle support.
6. UI notified with `NC_ASSET | NA_ACTIVATED` (and `NC_SCENE | ND_TOOLSETTINGS`).

---

## Tool Switching and Brush Memory

When a tool switch occurs, `toolsystem_brush_activate_from_toolref_for_object_paint()` (`wm_toolsystem.cc`, ~line 392) runs:

1. `WM_toolsystem_last_brush_asset_from_brush_type()` (~line 368) looks up `active_brush_per_brush_type` for the new tool's brush type.
2. If found, `BKE_paint_brush_set()` resolves and activates that brush.
3. For the main (non-type-specific) tool, `toolsystem_main_brush_binding_update_from_active()` (~line 255) syncs the main binding to the newly active brush.

Example:
```
Active tool: Draw (brush type DRAW) → active brush: "Chisel"
Switch to:   Mask  (brush type MASK) → active brush: "Mask" (restored from bindings)
Switch back: Draw  (brush type DRAW) → active brush: "Chisel" (restored from bindings)
```

---

## Essentials Library Defaults

Each paint mode maps to a `.blend` file in the Essentials asset library (files live under `assets/brushes/`):

| Mode | File |
|------|------|
| Sculpt | `essentials_brushes-mesh_sculpt.blend` |
| Vertex Paint | `essentials_brushes-mesh_vertex.blend` |
| Weight Paint | `essentials_brushes-mesh_weight.blend` |
| Texture Paint | `essentials_brushes-mesh_texture.blend` |
| Grease Pencil Draw | `essentials_brushes-gp_draw.blend` |
| Grease Pencil Sculpt | `essentials_brushes-gp_sculpt.blend` |
| Grease Pencil Weight | `essentials_brushes-gp_weight.blend` |
| Grease Pencil Vertex | `essentials_brushes-gp_vertex.blend` |
| Curves Sculpt | `essentials_brushes-curve_sculpt.blend` |

`BKE_paint_brush_type_default_reference()` (`paint.cc`, ~line 1042) returns an `AssetWeakReference` pointing into one of these files (the filename mapping is in the helper at ~line 761). The `relative_asset_identifier` looks like `"brushes/<file>/Brush/<name>"`.

---

## Serialization

### Save (`BKE_paint_blend_write()`, `paint.cc`, ~line 1748)

- `BKE_asset_weak_reference_write()` serializes each `AssetWeakReference`.
- `tool_brush_bindings` fields are written in full: main reference, then the per-type list with its string names.

### Load (`BKE_paint_blend_read_data()`, `paint.cc`, ~line 1798)

- `BLO_read_struct` and `BKE_asset_weak_reference_read()` reconstruct the structs.
- `paint->brush` is left `NULL`; actual resolution happens post-link via `paint_brush_update_from_asset_reference()`.

---

## Compatibility and Error Handling

- If a weak reference cannot be resolved (missing library, moved asset), the reference is deleted and the system falls back to the Essentials default.
- If the resolved brush's `ob_mode` doesn't match the current paint mode, the reference is also discarded (~line 663 in `paint.cc`).
- The system therefore degrades gracefully: the user loses their custom brush choice but never sees a crash or invalid state.

---

## Key Entry Points

| Function | File | Purpose |
|----------|------|---------|
| `WM_toolsystem_activate_brush_and_tool()` | `wm_toolsystem.cc` ~301 | Top-level brush+tool activation |
| `BKE_paint_brush_set(Main*, Paint*, const AssetWeakReference&)` | `paint.cc` ~702 | Set brush from asset reference |
| `BKE_paint_brush_set(Paint*, Brush*)` | `paint.cc` ~737 | Set brush from direct pointer |
| `paint_brush_update_from_asset_reference()` | `paint.cc` ~643 | Post-load resolution |
| `WM_toolsystem_last_brush_asset_from_brush_type()` | `wm_toolsystem.cc` ~368 | Query remembered brush for a tool type |
| `toolsystem_brush_type_binding_update()` | `wm_toolsystem.cc` ~270 | Update per-type binding after activation |
| `BKE_paint_brush_type_default_reference()` | `paint.cc` ~1042 | Essentials fallback reference |
| `brush_asset_activate_exec()` / `BRUSH_OT_asset_activate()` | `brush_asset_ops.cc` ~53 / ~127 | Operator: user activates a brush |

---

See the memory index at **[../README.md](../README.md)** and the coding standards at **[../../CLAUDE.md](../../CLAUDE.md)**.
