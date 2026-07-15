# Design: Addon-Registrable Object Modes (Custom Sculpt Mode from Python)

*Investigation and design proposal, 2026-07-02. Based on a survey of the
current `main` codebase (mode wiring, undo system architecture, and existing
Python-registrable type patterns).*

## Problem Statement

An addon should be able to register a new object mode — e.g. an alternative
sculpt mode — that behaves like a first-class mode: it appears in the mode
dropdown, has its own keymap/tools/panels, gets a proper enter/exit lifecycle
(including when the user switches objects or workspaces behind its back),
converts to/from `Mesh` data efficiently, and participates correctly in the
undo system.

This document maps what currently prevents that, and proposes the minimal
API surface to make it possible.

## TL;DR

A first-class addon-defined mode is blocked by exactly five hardwired points,
all of them lookup tables rather than deep architecture — the mode enter/exit
machinery is already indirection-based (mode → operator-name string → toggle
operator). The minimal design is:

1. **One generic `OB_MODE_CUSTOM` DNA bit + an idname string** on `Object`.
2. **A RenderEngine-style registrable `bpy.types.ObjectModeType`** with
   `enter` / `exit` / `flush` / `refresh` callbacks.
3. **A single C-implemented undo type that wraps opaque Python state** —
   never exposing the raw `UndoType` contract.

Conversion rides the existing bulk-transfer paths (`foreach_get/set`,
attributes API), with the invariant that *persistent state always lives in
the Mesh ID*. That invariant is what makes save, memfile undo, and forward
compatibility all fall out for free.

Total scope: roughly 1–2k lines of C/C++. Tier 1 (everything except the
custom undo type) is self-contained and already yields a real mode.

---

## Part 1: What Hardwires the Set of Modes Today

### The five hard blockers

Everything else degrades gracefully for an unknown mode bit (treated as
object mode / no-op, no crashes). These five do not:

1. **`mode_compat_test()`** — `editors/object/object_modes.cc:103`.
   Hardwired `switch (ob->type)` returning an allowed-mode bitmask per object
   type. An unknown mode bit returns `false` for every object type, so an
   unknown mode can never be entered. This is the single biggest gate.

2. **`object_mode_op_string()`** — `object_modes.cc:62`.
   The central switchboard mapping each mode to its toggle operator idname
   (e.g. `OB_MODE_SCULPT` → `"SCULPT_OT_sculptmode_toggle"`). `mode_set_ex()`
   (`object_modes.cc:186`) resolves through this and calls the operator by
   name — so enter/exit is *already* indirection-based, not a giant switch.
   Unknown modes simply fall through with no operator to call.

3. **`rna_enum_object_mode_items[]`** — `makesrna/intern/rna_object.cc:36`.
   Static membership with hardwired icons/labels. The 3D-viewport header
   dropdown *filters* this list dynamically (`object_mode_set_itemf`,
   `object_edit.cc:2004`) but never adds to it. Worse, the header UI
   (`scripts/startup/bl_ui/space_view3d.py:846`) does
   `Object.bl_rna.properties["mode"].enum_items[object_mode]` — a mode value
   absent from the static enum raises `KeyError` rendering the header.

4. **`eContextObjectMode` + `data_mode_strings[]`** —
   `BKE_context.hh:132-161` and `context.cc:1501-1530`. Fixed-size,
   `BLI_STATIC_ASSERT`-checked, index-aligned arrays. Everything strings hang
   off flows through these: Python `context.mode`, panel `bl_context`
   matching, and the tool system's per-mode key
   (`WM_toolsystem_mode_from_spacetype`, `wm_toolsystem.cc:740`, which calls
   `CTX_data_mode_enum_ex`). The mapping function `CTX_data_mode_enum_ex()`
   (`context.cc:1405`) falls through to `CTX_MODE_OBJECT` for unknown modes —
   graceful but wrong.

5. **`ed_object_mode_generic_exit_ex()` + `OB_MODE_ALL_MODE_DATA`** —
   `object_modes.cc:264-345` and `DNA_object_enums.h:64`. The if/else chain
   that frees mode data when the user switches objects or workspaces. An
   unknown mode bit outside `OB_MODE_ALL_MODE_DATA` hits the final assert and
   its session data leaks. This is precisely the piece that modal-operator
   pseudo-modes can never get right.

### What degrades gracefully

- **Transform** (`transform_convert.cc:819`): unknown mode falls out of all
  branches → object-level transform. Not ideal, not a crash.
- **Statistics** (`info_stats.cc`): object-level stats or blank.
- **PBVH draw decision** (`BKE_sculptsession_use_pbvh_draw`, consulted at
  `draw_context.cc:682`): not taken → normal mesh drawing.
- **Message bus**: keys on the RNA property (`Object.mode`), not the value.
- **Per-workspace mode memory** (`WorkSpace.object_mode`,
  `DNA_workspace_types.h:178`): raw value, round-trips unknown bits fine.

### What is already extensible-shaped

- **Keymap activation is poll-driven.** All mode keymaps are installed
  permanently once in `view3d_main_region_init()`
  (`space_view3d.cc:329-413`); gating is per-keymap `poll` callbacks (e.g.
  `paint_ops.cc:684-718`). An unknown mode just gets no keymap — adding one
  is easy.
- **Objects-in-mode filtering** (`base_is_in_mode()`, `layer.cc:2343`) is
  bitmask-based and fully generic.
- **DNA storage**: `Object.mode` / `Object.restore_mode`
  (`DNA_object_types.h:520-521`) are plain `eObjectMode` ints; unknown bits
  serialize and round-trip.

### The undo system cannot be exposed raw

The `UndoType` registration API (`BKE_undosys_type_append`,
`undo_system.cc:915`; callback contract at `BKE_undo_system.hh:112-173`) has
**no Python binding**, and the contract genuinely can't be exposed as-is:

- **Threading**: sculpt pushes undo nodes *concurrently from brush-eval
  threads* under a mutex (`sculpt_undo.cc:258`, `:1840-1922`). Python's GIL
  makes high-frequency per-node pushes impractical.
- **`Main` lifetime**: `step_decode` can free and reallocate `Main` mid-call
  (`memfile_undo.cc:204`, `undo_system.cc:211`); a Python callback holding
  stale `bpy` references across that boundary would crash.
- **Protocol subtlety**: correct interop with memfile undo requires the
  `is_memfile_undo_flush_needed` flag, `ED_undosys_stack_memfile_id_changed_tag`
  on affected IDs (`memfile_undo.cc:366-380`), name-based `UndoRefID`
  remapping (`step_foreach_ID_ref`), and forcing a memfile step at mode
  boundaries (`use_memfile_step`, cf. sculpt's `DyntopoEnd` at
  `sculpt_undo.cc:2204`). Order-sensitive and identical for every client —
  it belongs in C, written once.

How sculpt itself interoperates is the model to generalize: sculpt session
runtime data is **flushed back into the Mesh ID on demand**
(`needs_flush_to_id` → `ED_editors_flush_edits_for_object_ex`,
`ed_util.cc:269-299` → `BKE_sculptsession_bm_to_me` /
`multires_flush_sculpt_updates`) so memfile snapshots and file saves always
capture a consistent mesh. Sculpt's own undo steps are deltas against that
baseline.

### The registrable-type precedent

- **`RenderEngine` is the template** (`rna_render.cc:303-385`): a registrable
  RNA struct backed by a C type object (`RenderEngineType`), with
  `have_function[]` validation at register time, C trampolines wired only for
  callbacks the Python subclass defines, dispatch through `rna_ext.call` with
  GIL handling (`BPy_BEGIN/END_ALLOW_THREADS`), and a persistent
  `py_instance` so the same Python object lives across callbacks.
- **`GizmoGroup`** (`rna_wm_gizmo.cc:1366`) shows the
  `bl_space_type`/`bl_options`/poll registration-property pattern.
- **`WorkSpaceTool` is deliberately *not* an RNA-registrable type** — it's
  pure Python data (`ToolDef` namedtuples in `space_toolsystem_common.py`)
  keyed by context-mode strings, with a thin C runtime
  (`wm_toolsystem.cc`). Tool keymaps use `kc.keymaps.new(..., tool=True)`
  and dynamic keymap handlers — the same mechanism a custom mode's keymap
  should reuse.
- The generic registration entry point is `pyrna_register_class`
  (`bpy_rna.cc:10368`) via `RNA_def_struct_register_funcs`.

---

## Part 2: Proposed Design

### Identity: one bit + a string

DNA cannot hold dynamic enums, so don't try. Instead:

- Add `OB_MODE_CUSTOM = (1 << 13)` to `eObjectMode`
  (`DNA_object_enums.h`), OR'd into `OB_MODE_ALL_MODE_DATA`.
- Add `char custom_mode_id[64]` to `Object` (alongside `mode` /
  `restore_mode`).

Multiple registered custom modes are distinguished by idname; one custom mode
active per object at a time — which matches how `ob->mode` is used everywhere
anyway. On file load, if the idname isn't registered (addon disabled),
sanitize to `OB_MODE_OBJECT`; same failure mode as a missing addon's
operators. Older Blender versions opening such a file see an unknown bit that
`mode_compat_test` already rejects, so forward compatibility is a non-issue.

### The registrable type: `bpy.types.ObjectModeType`

Cloned structurally from `RenderEngine` (`rna_render.cc` is the exact
template).

```python
class MySculptMode(bpy.types.ObjectModeType):
    bl_idname = "my_addon.sculpt2"
    bl_label = "Sculpt 2"
    bl_icon = 'SCULPTMODE_HLT'
    bl_object_types = {'MESH'}          # feeds mode_compat_test
    bl_keymap = "My Sculpt Mode"        # keymap ensured at register, tool-style
    bl_use_custom_undo = True           # opt into the wrapped undo type

    def enter(self, context, ob): ...   # build session data, read mesh
    def exit(self, context, ob): ...    # flush + free session data
    def flush(self, ob): ...            # write deferred state into the Mesh ID
    def refresh(self, context, ob): ... # re-sync session after undo/ID reload
    def undo_encode(self): ...          # optional: return opaque state object
    def undo_decode(self, state, direction): ...
```

Each of the five blockers gets a small generic branch:

- `mode_compat_test`: `OB_MODE_CUSTOM` → look up the registered type, check
  `bl_object_types`.
- `object_mode_op_string`: `OB_MODE_CUSTOM` → one new generic
  `OBJECT_OT_custom_mode_toggle` that dispatches the `enter`/`exit`
  trampolines.
- `ed_object_mode_generic_exit_ex`: an `OB_MODE_CUSTOM` branch calling
  `exit`. This is what makes object/workspace switching safe.
- RNA enum: one static `'CUSTOM'` item (membership satisfied);
  `object_mode_set_itemf` additionally appends registered modes with their
  own label/icon; the header UI (`space_view3d.py`) special-cases `'CUSTOM'`
  to pull label/icon from the registered type. A read-only
  `Object.custom_mode` string property exposes the idname.
- Context: one `CTX_MODE_CUSTOM` slot; `CTX_data_mode_string()` special-cased
  to return the registered idname so `context.mode` and panel `bl_context`
  matching work unchanged.

**Keymaps** are the easy part — activation is already poll-driven. At
registration, ensure a keymap named per `bl_keymap` (the `tool=True`-style
dynamic keymap path). In `view3d_main_region_init()`, install one
`WM_event_add_keymap_handler_dynamic` that resolves to the active custom
mode's keymap — the same mechanism tool keymaps already use. Add one generic
entry to `bl_keymap_utils/keymap_hierarchy.py`.

**Tools, gizmos, overlays come free** once the context string exists:
`WorkSpaceTool` is pure Python keyed by context-mode string, so
`tools_from_context` can serve per-idname toolbars; gizmos are registered
`GizmoGroup`s with a mode poll; overlay drawing uses the existing
`SpaceView3D.draw_handler_add`.

### Conversion to/from Mesh

No new API — a *contract*, not plumbing. The mode owns its runtime
representation entirely on the Python side (numpy arrays, a C-extension BVH,
whatever), built in `enter()` via `foreach_get` / the attributes API, which
are already the fast flat-buffer paths.

The invariant that makes everything else work: **anything that must survive
lives in the Mesh ID** (positions, named attributes, ID properties), and
`flush()` is the mode's promise to make that true on demand.

`flush()` is the sculpt pattern generalized. The one C hook needed:
`ED_editors_flush_edits_for_object_ex` (`ed_util.cc:269`) calls the `flush`
trampoline for `OB_MODE_CUSTOM` objects — the same place sculpt's
`needs_flush_to_id` path runs, i.e. before memfile undo encode and file save.
This lets the addon defer writes during a stroke (keeping interaction fast)
while save, undo, and render always see a consistent Mesh. A v1 addon can
skip deferral entirely and `foreach_set` positions per stroke-step — slow
past ~1M verts but correct, and the API doesn't change when the addon later
gets smarter.

### Undo: two tiers

**Tier 1 — free (memfile).** Because of the flush contract, the mode's state
is in the Mesh, so operators with `bl_options = {'UNDO'}` just work: memfile
encode triggers the flush, snapshots the mesh, done. The missing piece is the
other direction: memfile decode replaces `Main`, invalidating everything the
Python session holds. That's what `refresh()` is for — driven off the
existing `undo_post` timing (`ed_undo.cc:208`) but delivered as a mode
callback so the ordering guarantee (object still in the mode, IDs remapped,
depsgraph valid) is part of the API rather than the addon guessing from an
app handler. Note the `ED_undo_is_legacy_compatible_for_property` special
case that suppresses undo pushes during sculpt mode (`ed_undo.cc:446`) keys
on `OB_MODE_SCULPT` specifically — custom modes don't inherit that surprise.

**Tier 2 — the wrapped undo type.** Memfile snapshots of a dense mesh per
stroke are exactly why sculpt undo exists. The viable shape is **one
C-implemented `UndoType` ("CUSTOM_MODE"), registered before memfile, storing
an opaque `PyObject`**:

- `poll`: active object in `OB_MODE_CUSTOM` and the registered type declared
  `bl_use_custom_undo`.
- `step_encode`: call `undo_encode()` (main thread, GIL held); store whatever
  it returns — bytes, a numpy delta, anything.
- `step_decode`: call `undo_decode(state, direction)`.
- `step_free`: drop the reference (with GIL).

Critically, the C wrapper — not the addon — handles the entire
memfile-interop protocol: `ED_undosys_stack_memfile_id_changed_tag` on the
object and mesh at push time, setting `bmain->is_memfile_undo_flush_needed`
(which triggers `flush()` via the existing path), storing the object
reference as a name-based `UndoRefID` for `step_foreach_ID_ref`, and setting
`use_memfile_step` on the mode-exit step the way sculpt's `DyntopoEnd` does,
so mode boundaries get a full snapshot.

What this deliberately gives up: threaded per-BVH-node partial pushes and
partial redraw. Python-side encode of a whole-mesh delta per step is the
ceiling — fine for a v1 API, and probably fine forever for
non-mesh-density workloads (custom lattice/cage modes, annotation modes,
retopo modes would use the same machinery).

---

## Part 3: Concrete Change List

| Change | Where | Size |
|---|---|---|
| `OB_MODE_CUSTOM` bit + `ALL_MODE_DATA`; `Object.custom_mode_id`; do_version sanitize | `DNA_object_enums.h`, `DNA_object_types.h`, versioning | tiny |
| `ObjectModeType` registrable RNA type + global registry | new `rna_object_mode.cc`, modeled on `rna_render.cc` | the bulk, ~600 lines |
| Branches in `mode_compat_test`, `object_mode_op_string`, `ed_object_mode_generic_exit_ex`; generic toggle operator | `object_modes.cc`, `object_edit.cc` | small |
| `'CUSTOM'` enum item; dynamic items in `object_mode_set_itemf`; header UI label fix | `rna_object.cc`, `space_view3d.py` | small |
| `CTX_MODE_CUSTOM` + dynamic mode string | `BKE_context.hh`, `context.cc` | small |
| Dynamic keymap handler in region init; hierarchy entry | `space_view3d.cc`, `bl_keymap_utils/keymap_hierarchy.py` | small |
| `flush` trampoline call | `ed_util.cc` (`ED_editors_flush_edits_for_object_ex`) | tiny |
| Tier-2 wrapped undo type | new `custom_mode_undo.cc` + registration in `undo_system_types.cc` | ~300 lines |

Subsystems that switch on specific mode bits elsewhere — transform's convert
dispatch, viewport statistics, PBVH draw — all fall through harmlessly for
`OB_MODE_CUSTOM`, so nothing else *has* to change. The mode doesn't get
special transform behavior or overlay-engine support, which is the right v1
boundary.

## Open Design Decision

Whether `custom_mode_id` persists in files at all. Persisting it (with the
sanitize-on-load fallback) means files reopen in the mode when the addon is
present, matching sculpt-mode behavior; not persisting sidesteps every
compatibility question at the cost of UX. Recommendation: **persist** — the
sanitize path makes it safe, and it's what users will expect.

## Key File Reference

- `source/blender/makesdna/DNA_object_enums.h` — `eObjectMode`, mode masks
- `source/blender/editors/object/object_modes.cc` — `mode_compat_test`,
  `object_mode_op_string`, `mode_set_ex`, `ed_object_mode_generic_exit_ex`
- `source/blender/editors/object/object_edit.cc` — `OBJECT_OT_mode_set`,
  `object_mode_set_itemf`
- `source/blender/makesrna/intern/rna_object.cc` — `rna_enum_object_mode_items`
- `source/blender/makesrna/intern/rna_render.cc` — registrable-type template
- `source/blender/blenkernel/BKE_context.hh`, `intern/context.cc` —
  `eContextObjectMode`, `data_mode_strings`
- `source/blender/blenkernel/BKE_undo_system.hh`, `intern/undo_system.cc` —
  `UndoType` contract, type selection, memfile ordering
- `source/blender/editors/sculpt_paint/mesh/sculpt_undo.cc` — sculpt undo,
  memfile interop (`id_changed_tag`, `use_memfile_step`, flush flag)
- `source/blender/editors/undo/memfile_undo.cc`, `ed_undo.cc` — memfile
  poll/encode/decode, `undo_pre/post` timing
- `source/blender/editors/util/ed_util.cc` — flush-to-ID path
- `source/blender/editors/space_view3d/space_view3d.cc` — keymap installation
- `source/blender/windowmanager/intern/wm_toolsystem.cc`,
  `scripts/startup/bl_ui/space_toolsystem_common.py` — tool system keying
