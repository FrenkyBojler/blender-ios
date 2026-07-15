# Dependency Graph and Window Manager / Operator System

Two subsystems that sit under nearly every interactive feature: the
**dependency graph** (evaluation order and change propagation) and the
**window manager** (operators, keymaps, the event/modal loop).

---

## Part A — Dependency Graph (`source/blender/depsgraph/`, prefix `DEG_`)

### Core concepts

The graph is built from three node kinds:

- `struct Node` — base type, `source/blender/depsgraph/intern/node/deg_node.hh:159`.
  `enum class NodeClass` (line 34) distinguishes:
  - `COMPONENT` — an "outer" node representing one aspect of an ID
    (transform, geometry, animation, …).
  - `OPERATION` — an "inner" node: a single scheduled callback.
  `enum class NodeType` (line 49) enumerates concrete component kinds.
- `struct ComponentNode : public Node` — `intern/node/deg_node_component.hh:33`.
  Subclasses: `ParametersComponentNode` (232), `BoneComponentNode` (221),
  `AudioComponentNode` (247), etc.
- `struct OperationNode : public Node` — `intern/node/deg_node_operation.hh:257`.
  The actual function-pointer/callback nodes that get scheduled for execution.
- `struct IDNode : public Node` — `intern/node/deg_node_id.hh:38`. One per
  datablock; owns that datablock's `ComponentNode`s.

Edges are `struct Relation` (`intern/depsgraph_relation.hh:35`): `Node *from`,
`Node *to`, a `name`, and a `flag` bitmask (`RELATION_FLAG_*`, lines 25-31).
Convention: "B depends on A (A → B)".

**Graph construction**:
- `DepsgraphNodeBuilder` (`intern/builder/deg_builder_nodes.h:74`) builds
  ID/component/operation nodes.
- `DepsgraphRelationBuilder` (`intern/builder/deg_builder_relations.h:84`)
  wires `Relation`s between them.
- Both derive from `DepsgraphBuilder`; pipeline orchestration lives in
  `intern/builder/pipeline.h`.

**Evaluation**: `intern/depsgraph_eval.cc` calls
`deg::deg_evaluate_on_refresh(deg_graph)`, which schedules and executes
operation nodes; this is what the public `DEG_evaluate_on_refresh` wraps.

### Key entry points

| Function | Declared | Defined |
|---|---|---|
| `DEG_graph_build_from_view_layer` | `DEG_depsgraph_build.hh:38` | `intern/depsgraph_build.cc:283` |
| `DEG_evaluate_on_refresh` | `DEG_depsgraph.hh:201` | `intern/depsgraph_eval.cc:55` |
| `DEG_id_tag_update` | `DEG_depsgraph.hh:123` | `intern/depsgraph_tag.cc:843` |
| `DEG_id_tag_update_ex` (explicit `Main*`) | — | `intern/depsgraph_tag.cc:848` |
| `DEG_id_tag_update_for_side_effect_request` (explicit `Depsgraph*`) | — | `intern/depsgraph_tag.cc:857` |

`DEG_evaluate_on_refresh` syncs the current scene frame, tags the time
source if needed, flushes updates (`deg::deg_graph_flush_updates`, line 41),
then calls `deg::deg_evaluate_on_refresh` (line 44).

### Per-view-layer graphs and paint-mode tagging

`DEG_graph_build_from_view_layer` builds one graph per `Depsgraph` instance;
Blender keys these per `(main, scene, view_layer)`, so each view layer owns
its own graph, rebuilt via this entry point.

Sculpt/paint code does **not** manipulate graph structure directly — it tags
datablocks and lets the depsgraph recompute derived data on the next
refresh. Examples in `source/blender/editors/sculpt_paint/mesh/sculpt.cc`:

```cpp
DEG_id_tag_update(&ob.id, ID_RECALC_SHADING);
DEG_id_tag_update(&ob.id, ID_RECALC_GEOMETRY);
// sculpt.cc ~902/906, ~5393/5401, ~5698/5700
```

The same pattern recurs across ~41 files under `editors/sculpt_paint/`:
`mesh/paint_hide.cc:116,119`, `curves/sculpt_add.cc:254`,
`curves/sculpt_comb.cc:180`, `curves/sculpt_density.cc:300,615`,
`grease_pencil/draw_ops.cc:1780,1817` (`ID_RECALC_TRANSFORM | ID_RECALC_GEOMETRY`).
Generally once per stroke-apply/finish, marking geometry/shading dirty so
the next `DEG_evaluate_on_refresh` recomputes the evaluated mesh and draw
batches. See [paint-mode-system.md](./paint-mode-system.md) for the stroke
pipeline these tags are called from.

---

## Part B — Window Manager / Operators (`source/blender/windowmanager/`, prefix `WM_`)

### Operator registration

- `WM_operatortype_append(void (*opfunc)(wmOperatorType *ot))` — declared
  `WM_api.hh:1411`, defined `intern/wm_operator_type.cc:151`. Each operator
  module supplies a registration function that fills in a `wmOperatorType`.
- `struct wmOperatorType` — `WM_types.hh:1082`. Fields: `name`, `idname`,
  `description`, `undo_group`, and callback pointers — `exec` (1106),
  `check` (1114), `invoke` (1122), `cancel` (1130), `modal` (1138),
  `poll` (1146) — all returning `wmOperatorStatus`
  (`OPERATOR_RUNNING_MODAL`, `OPERATOR_FINISHED`, etc.).
- Invocation lives in `intern/wm_event_system.cc`: `wm_operator_invoke`
  (line 1643) and `wm_operator_call_internal` (declared 133, defined 1812)
  — dispatches to `exec`/`invoke` based on call context and, on
  `OPERATOR_RUNNING_MODAL`, registers a modal handler (lines 1743, 1907-1941).

Python-registered operators (`bpy.types.Operator` subclasses) go through the
**same** `WM_operatortype_append` registry via an RNA bridge — see
[python-bpy-integration.md](./python-bpy-integration.md) §1 for
`rna_Operator_register()` / `WM_operatortype_append_ptr()`.

### Keymaps

- `struct wmKeyMap` — `source/blender/makesdna/DNA_windowmanager_types.h:418`.
- `struct wmKeyMapItem` — same file, line 344.
- `WM_keymap_ensure(wmKeyConfig *keyconf, const char *idname, int spaceid, int regionid)`
  — declared `WM_keymap.hh:114`, defined `intern/wm_keymap.cc:911`.
- Python↔C bridge: `source/blender/makesrna/intern/rna_wm_api.cc` exposes
  `keymap.keymap_item_add` etc. to RNA/Python, calling `WM_keymap_add_item`
  (line 357) / `WM_keymap_add_item_copy` (387); `rna_wm_api.cc:518` calls
  `WM_keymap_ensure` when Python does `keyconfig.keymaps.new(...)`.
- Python keyconfig presets (`scripts/presets/keyconfig/Blender.py`,
  `Blender_27x.py`, `Industry_Compatible.py`, plus data under
  `scripts/presets/keyconfig/keymap_data/`) build the default keymaps by
  calling into this RNA API at startup, populating the same C
  `wmKeyMap`/`wmKeyMapItem` structures `wm_event_do_handlers` consumes.
- `intern/wm_keymap_utils.cc` provides lookup/builder helpers — e.g. the
  paint stroke modal keymap,
  `source/blender/editors/sculpt_paint/paint_stroke.cc:1103`
  (`paint_stroke_modal_keymap`).

### Event / modal loop

- `wm_event_do_handlers(bContext *C)` — declared `wm_event_system.hh:155`,
  defined `intern/wm_event_system.cc:4217`. Called from the main loop
  (`intern/wm.cc:611`) and from `intern/wm_window.cc:1961`. Walks windows,
  then per-area/region handler stacks via
  `wm_event_do_handlers_area_regions` (line 4192).
- Modal paint strokes: `PaintStroke::modal(bContext *C, wmOperator *op, const wmEvent *event)`
  (`source/blender/editors/sculpt_paint/paint_stroke.cc:1384`) returns
  `OPERATOR_RUNNING_MODAL` (lines 1407, 1626) each event until the stroke
  ends, dispatched by the handler chain set up in
  `wm_operator_call_internal`/`wm_operator_invoke` and driven per-event by
  `wm_event_do_handlers`. See [paint-mode-system.md](./paint-mode-system.md)
  for the full `PaintStroke` structure and callback set.

### `wmWindowManager` / `wmWindow` / areas & regions

`struct wmWindowManager` (`DNA_windowmanager_types.h:112`) is the top-level
singleton owning the list of `wmWindow`s, global keyconfigs, and
operator/notifier queues. `struct wmWindow` (`DNA_windowmanager_types.h:166`)
represents one OS window and owns a `bScreen`, which contains `ScrArea`s;
each area holds `ARegion`s (3D viewport, properties, header, …), each with
its own event-handler list — the unit `wm_event_do_handlers_area_regions`
routes input to.

---

## Key File:Line Reference Table

| Topic | File | Line |
|---|---|---|
| `Node` base / `NodeClass` / `NodeType` | `depsgraph/intern/node/deg_node.hh` | 159 / 34 / 49 |
| `ComponentNode` | `depsgraph/intern/node/deg_node_component.hh` | 33 |
| `OperationNode` | `depsgraph/intern/node/deg_node_operation.hh` | 257 |
| `IDNode` | `depsgraph/intern/node/deg_node_id.hh` | 38 |
| `Relation` | `depsgraph/intern/depsgraph_relation.hh` | 35 |
| `DepsgraphNodeBuilder` | `depsgraph/intern/builder/deg_builder_nodes.h` | 74 |
| `DepsgraphRelationBuilder` | `depsgraph/intern/builder/deg_builder_relations.h` | 84 |
| `DEG_graph_build_from_view_layer` | `depsgraph/intern/depsgraph_build.cc` | 283 |
| `DEG_evaluate_on_refresh` | `depsgraph/intern/depsgraph_eval.cc` | 55 |
| `DEG_id_tag_update` | `depsgraph/intern/depsgraph_tag.cc` | 843 |
| `WM_operatortype_append` | `windowmanager/intern/wm_operator_type.cc` | 151 |
| `wmOperatorType` struct | `windowmanager/WM_types.hh` | 1082 |
| `wm_operator_call_internal` | `windowmanager/intern/wm_event_system.cc` | 1812 |
| `wmKeyMap` / `wmKeyMapItem` | `makesdna/DNA_windowmanager_types.h` | 418 / 344 |
| `WM_keymap_ensure` | `windowmanager/intern/wm_keymap.cc` | 911 |
| `wm_event_do_handlers` | `windowmanager/intern/wm_event_system.cc` | 4217 |
| `PaintStroke::modal` | `editors/sculpt_paint/paint_stroke.cc` | 1384 |
| `wmWindowManager` / `wmWindow` | `makesdna/DNA_windowmanager_types.h` | 112 / 166 |
