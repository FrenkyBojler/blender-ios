# Plan — Custom Object Mode Infrastructure (Blender C side, Tier 1)

**Goal.** Implement the minimal Blender modifications that let a Python addon
register a first-class object mode: `bpy.types.ObjectModeType` +
`OB_MODE_CUSTOM`, per the design in
[../design/addon-custom-modes.md](../design/addon-custom-modes.md). This plan
covers **Tier 1 only** — everything except the wrapped undo type (that is
[undo-integration.md](./undo-integration.md)) and the draw hook (that is
[draw-integration.md](./draw-integration.md)).

**Outcome.** An addon can register a mode that: appears in the mode dropdown
with its own label/icon, enters/exits through `enter`/`exit` callbacks
(including forced exit on object/workspace switches and file close), owns a
keymap and toolbar context, exposes `context.mode` == its idname for panel
`bl_context` matching, and gets its `flush` callback invoked before memfile
undo encode / file save / render.

**Dependencies.** None (this is pure Blender C/C++ work; it can proceed in
parallel with [python-bindings.md](./python-bindings.md)).

---

## 1. Background (validated 2026-07-15)

The five hardwired blockers and the registrable-type precedent are mapped in
the design doc; key anchors:

- `mode_compat_test()` / `object_mode_op_string()` /
  `ed_object_mode_generic_exit_ex()` — `editors/object/object_modes.cc:103`,
  `:62`, `:264-345`.
- `rna_enum_object_mode_items[]` — `makesrna/intern/rna_object.cc:36`;
  dynamic filter `object_mode_set_itemf` — `object_edit.cc:2004`; header UI
  KeyError hazard — `scripts/startup/bl_ui/space_view3d.py:846`.
- `eContextObjectMode` + `data_mode_strings[]` — `BKE_context.hh:132-161`,
  `context.cc:1501-1530`, mapping fallthrough `CTX_data_mode_enum_ex`
  (`context.cc:1405`).
- Flush seam — `ED_editors_flush_edits_for_object_ex` (`ed_util.cc:269-316`).
- Template for the registrable type — `rna_render.cc:303-385`
  (`RenderEngineType`, trampolines, `rna_ext.call`, GIL handling).

## 2. Change list

Order roughly = implementation order. Sizes: S <150, M 150–500, L 500+ lines.

### Phase A — identity + registry + registrable type

| # | File | Change | Size |
|---|---|---|---|
| A1 | `makesdna/DNA_object_enums.h` | `OB_MODE_CUSTOM = (1 << 13)`; OR into `OB_MODE_ALL_MODE_DATA`. | S |
| A2 | `makesdna/DNA_object_types.h` | `char custom_mode_id[64]` on `Object` (next to `mode`/`restore_mode`). | S |
| A3 | `blenkernel/BKE_object_modes.hh` (new) + `intern/object_modes_custom.cc` (new) | C type `ObjectModeType` (idname, label, icon, object-type mask, keymap name, flags, callback pointers, `rna_ext`); global registry (add/remove/find by idname, iterate); free-on-exit. | M |
| A4 | `makesrna/intern/rna_object_mode.cc` (new, modeled on `rna_render.cc`) | `bpy.types.ObjectModeType` registrable struct: `bl_idname`, `bl_label`, `bl_icon`, `bl_object_types`, `bl_keymap`, `bl_use_custom_undo`; register/unregister/instance callbacks; trampolines for `enter(context, ob)`, `exit(context, ob)`, `flush(ob)`, `refresh(context, ob)` with `have_function[]` validation. | L |
| A5 | Versioning (`blenloader/intern/versioning_*.cc` readfile sanitize) | On load: `OB_MODE_CUSTOM` with unregistered/empty idname → sanitize to `OB_MODE_OBJECT` (registration happens after file load, so sanitize must run at *mode-use* time too — see B1 poll). | S |

### Phase B — the five blocker branches

| # | File | Change | Size |
|---|---|---|---|
| B1 | `editors/object/object_modes.cc` | `mode_compat_test`: `OB_MODE_CUSTOM` → look up registered type by `ob->custom_mode_id` (or the pending idname), check `bl_object_types`. | S |
| B2 | `editors/object/object_modes.cc`, new operator in `object_edit.cc` | `object_mode_op_string`: `OB_MODE_CUSTOM` → `"OBJECT_OT_custom_mode_toggle"`. New generic toggle operator: sets/clears `ob->mode` + `custom_mode_id`, dispatches `enter`/`exit` trampolines, notifiers (`NC_SCENE|ND_MODE`), WM message-bus publish on `Object.mode`. | M |
| B3 | `editors/object/object_modes.cc` | `ed_object_mode_generic_exit_ex`: `OB_MODE_CUSTOM` branch → call `exit` trampoline, clear idname. Makes object/workspace switching and file close safe. | S |
| B4 | `makesrna/intern/rna_object.cc`, `object_edit.cc`, `scripts/startup/bl_ui/space_view3d.py` | One static `'CUSTOM'` enum item; `object_mode_set_itemf` appends registered modes (label/icon per type); read-only `Object.custom_mode` string prop; header UI special-cases `'CUSTOM'` to pull label/icon from the registered type (fixes the `enum_items[...]` KeyError). | M |
| B5 | `blenkernel/BKE_context.hh`, `intern/context.cc` | `CTX_MODE_CUSTOM` slot; `CTX_data_mode_enum_ex` maps `OB_MODE_CUSTOM` → it; `CTX_data_mode_string` returns the registered idname (sanitized: lowercase, addon-prefixed) so `context.mode` and panel `bl_context` matching work. Mind the `BLI_STATIC_ASSERT` array-size checks. | S |

### Phase C — keymap, tools, flush

| # | File | Change | Size |
|---|---|---|---|
| C1 | `editors/space_view3d/space_view3d.cc`, `scripts/modules/bl_keymap_utils/keymap_hierarchy.py` | One `WM_event_add_keymap_handler_dynamic` in `view3d_main_region_init()` resolving to the active custom mode's `bl_keymap`; one generic hierarchy entry. Keymap ensured at type registration (tool-style dynamic keymap path). | M |
| C2 | `windowmanager/intern/wm_toolsystem.cc`, `scripts/startup/bl_ui/space_toolsystem_common.py` | Verify the tool system keys off `CTX_data_mode_string` for custom modes (it flows through `WM_toolsystem_mode_from_spacetype` → `CTX_data_mode_enum_ex`); fix any spot that switches on the enum instead of the string. Expected: no or tiny changes. | S |
| C3 | `editors/util/ed_util.cc` | `ED_editors_flush_edits_for_object_ex`: `OB_MODE_CUSTOM` branch → call `flush` trampoline (mirrors sculpt's `needs_flush_to_id` path; runs before memfile encode, save, render). Add a dirty flag on the mode runtime so clean sessions skip the Python call. | S |
| C4 | `editors/undo/ed_undo.cc` | Call `refresh` trampoline from the `undo_post` timing for objects in `OB_MODE_CUSTOM` (memfile decode replaced `Main`; session must re-sync). Tier-1 undo is memfile-only; the full custom undo step is [undo-integration.md](./undo-integration.md). | S |

## 3. Order of work

1. A1–A3 (DNA + registry) — compiles standalone.
2. A4 (RNA type) — register/unregister from Python works, callbacks stored.
3. B1–B3 — mode can actually be entered/exited/switched safely.
4. B4–B5 — UI + context string; panels/tools/keymap become possible.
5. C1–C2 — keymap + toolbar.
6. C3–C4 — flush/refresh; memfile undo correct end-to-end.
7. A5 — versioning/sanitize last (needs the final persistence decision:
   design doc recommends **persisting** `custom_mode_id`).

## 4. Verification

- **Unit-ish:** a minimal test addon (`tests/` scratch, not shipped)
  registering a trivial mode that logs callbacks; drive with
  `claudeMemory/scripts/remote_repl.py`.
- Enter/exit via dropdown, `bpy.ops.object.mode_set`, and Python API; verify
  callback order and `context.mode` value.
- Switch active object while in the mode → `exit` fires (B3); switch
  workspace → same; close file → no leaks (ASAN build).
- Undo while in the mode (memfile): edit → undo → `refresh` fires, object
  still in mode, no stale pointers.
- Save with unflushed state → `flush` fires before write; reload → state
  present, mode restored (or sanitized if addon disabled).
- Header UI renders with the custom mode active (no KeyError); panels with
  `bl_context` = idname appear; keymap active only in-mode.
- Disable the addon while an object is in the mode → forced exit, no crash.
- `make format`; build with `WITH_UNITY_BUILD=OFF` clean build (new files +
  header changes).

## 5. Risks / notes

- `data_mode_strings` is `BLI_STATIC_ASSERT`-checked and index-aligned with
  the enum — the `CTX_MODE_CUSTOM` slot must keep every consumer in sync
  (grep all `CTX_MODE_` users).
- RNA register-time callbacks run without `Main` context in some paths;
  follow `rna_render.cc` exactly for instance lifetime + GIL.
- The message bus keys on the RNA property, so `Object.mode` publishes work
  unchanged — but anything subscribing to specific enum *values* needs a
  check.
- Multi-object mode entry (`FOREACH_OBJECT_IN_MODE`) uses bitmask matching —
  two different custom modes share one bit; matchers that need exactness must
  also compare `custom_mode_id`.
- Do not touch `ED_undo_is_legacy_compatible_for_property` (`ed_undo.cc:446`)
  — its sculpt special case keys on `OB_MODE_SCULPT` and custom modes should
  not inherit it.
