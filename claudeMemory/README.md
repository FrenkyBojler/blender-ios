# claudeMemory — Index

Working memory and documentation for the **SculptCore integration** project
(branch `sculptcore`): integrating the `extern/sculptcore` sculpting engine
into Blender as a first-class sculpt-mode addon.

This directory is Claude's scaffolding for the project. It is **temporary** —
see the cleanup checklist in [../CLAUDE.md](../CLAUDE.md) (delete
`claudeMemory/`, restore the original `claudeDocs/` + `AGENTS.md`) before the
final upstream PR.

**Resuming?** Start at **[RESUME.md](./RESUME.md)** — current focus, the exact
next task, environment gotchas, and the validation harnesses.

For coding standards, commit conventions, `.blend` compatibility rules, and
the project strategy/working conventions, see [../CLAUDE.md](../CLAUDE.md).

Every codebase doc gives concrete `file:line` references. Line numbers drift —
re-verify before relying on one for a change. These docs were validated
against the source at the start of the project.

---

## Layout

| Directory | Contents |
|---|---|
| `codebase/` | Validated reference docs for the Blender codebase (repository map, subsystems). |
| `design/` | Design proposals for this integration. |
| `plans/` | Implementation plans. One file per plan. |
| `research/` | Research notes and investigations. |
| `scripts/` | Dev tooling: `bl_env.bat` (build wrapper), `remote_repl.py` (main-thread-safe TCP REPL for interactive Blender). |
| `tests/` | Headless regression suites. `addon_regression.py` runs the whole sculpt-mode addon vertical against a built Blender (see its docstring). |

---

## `codebase/` — Blender Reference

| Doc | Covers |
|---|---|
| [project-overview.md](./codebase/project-overview.md) | Repository map: `source/`, `intern/`, `scripts/`, build/test directories, module prefixes. **Start here.** |
| [dna-rna-data-system.md](./codebase/dna-rna-data-system.md) | DNA (serialized structs) vs. RNA (introspection): `makesdna`/`makesrna` codegen, `.blend` versioning, `IDProperty` custom data. |
| [depsgraph-and-operators.md](./codebase/depsgraph-and-operators.md) | Dependency graph (nodes, relations, evaluation, tagging) and the window manager (operators, keymaps, event/modal loop). |
| [paint-mode-system.md](./codebase/paint-mode-system.md) | All paint/sculpt modes: DNA structs, mode entry/exit, stroke pipeline, brush dispatch, Python UI layer. |
| [brush-asset-system.md](./codebase/brush-asset-system.md) | Brushes as weak asset references: resolution, activation, per-tool-type brush memory, Essentials defaults. |
| [python-bpy-integration.md](./codebase/python-bpy-integration.md) | `bpy.utils.register_class` mechanics, startup/add-on loading, and paint-mode tool registration on the Python side. |
| [gpu-draw-and-nodes.md](./codebase/gpu-draw-and-nodes.md) | GPU backend abstraction, Draw Manager / render-engine registration, node-tree architecture, multi-/lazy-function evaluation. |
| [build-and-testing.md](./codebase/build-and-testing.md) | CMake/make build system, unity builds, test suites, buildbot workflow. |

### Suggested reading order

1. [project-overview.md](./codebase/project-overview.md) — orient in the tree.
2. [dna-rna-data-system.md](./codebase/dna-rna-data-system.md) — the data model everything builds on.
3. [depsgraph-and-operators.md](./codebase/depsgraph-and-operators.md) — how change propagates and how user actions dispatch.
4. [paint-mode-system.md](./codebase/paint-mode-system.md) + [brush-asset-system.md](./codebase/brush-asset-system.md) — a full vertical slice through one major subsystem.
5. [python-bpy-integration.md](./codebase/python-bpy-integration.md) — how that subsystem is exposed to and driven from Python.
6. [gpu-draw-and-nodes.md](./codebase/gpu-draw-and-nodes.md), [build-and-testing.md](./codebase/build-and-testing.md) — as needed.

---

## `design/` — Integration Design

| Doc | Covers |
|---|---|
| [addon-custom-modes.md](./design/addon-custom-modes.md) | The core proposal: making object modes registrable from Python addons. Maps the five hardwired blockers; proposes `bpy.types.ObjectModeType` + `OB_MODE_CUSTOM` + a C-implemented wrapped undo type. **The blueprint for the whole project.** |

---

## `plans/` — Implementation Plans

Master tracker with dependencies and per-plan checklists:
**[plans/taskList.md](./plans/taskList.md)**.

| # | Plan | Covers |
|---|---|---|
| P1 | [python-bindings.md](./plans/python-bindings.md) | `ctypes` runtime over the `LSTL_*` ABI + `.pyi` stub generator. |
| P2 | [mode-infra.md](./plans/mode-infra.md) | `bpy.types.ObjectModeType` + `OB_MODE_CUSTOM` (Tier 1 of the design doc). |
| P3 | [mesh-convert.md](./plans/mesh-convert.md) | Mesh ⇄ SculptCore bulk conversion, enter/exit/flush lifecycle. |
| P4 | [addon-skeleton.md](./plans/addon-skeleton.md) | The addon: mode class, session registry, stroke operator, keymap/UI. |
| P5 | [draw-integration.md](./plans/draw-integration.md) | Flush-to-Mesh fallback + external draw-provider seam for the viewport engines. |
| P6 | [undo-integration.md](./plans/undo-integration.md) | Wrapped `CUSTOM_MODE` undo type coupled to SculptCore's meshlog. |
| P7 | [brush-mapping.md](./plans/brush-mapping.md) | Reusing Blender `Brush` properties; engine-only params as addon custom props. |
| P8 | [multires-convert.md](./plans/multires-convert.md) | MDISPS ⇄ SculptCore displacement grids (multires modifier ignored at runtime). |
| P9 | [stroke-quality.md](./plans/stroke-quality.md) | Stroke quality & parity from the reference-app reports: neighbor cache, BSMOOTH, dyntopo cadence, spline smoothing, symmetry, anchored/drag-dot. Addon-only. |
| P10 | [ui-parity.md](./plans/ui-parity.md) | UI parity with vanilla sculpt mode: reuse vanilla panels via subclasses, engine-op mapping for menus/keymap, toolbar/asset shelf/properties, allowlist + regression lock-in. |

Dev tooling (outside the P1–P9 integration sequence):

| Plan | Covers |
|---|---|
| [shared-sculptcore-checkout.md](./plans/shared-sculptcore-checkout.md) | NTFS-junction `webgl-app-framework/sculptcore` to our `extern/sculptcore` so the sister app's tests run against our (unpushed) engine changes. |
| [ui-parity-tooling.md](./plans/ui-parity-tooling.md) | Debug tooling for the UI-parity effort: poll-matrix dump, layout introspection capture, keymap diff, screenshot harness, event-simulate hotkey runner. |

When implementing a plan, mark scaffolding comments with `CLAUDENOTE:` and
strip them (plus audit all touched comments) when the plan is complete — see
[../CLAUDE.md](../CLAUDE.md).

## `research/` — Research Notes

| Doc | Covers |
|---|---|
| [sculpt-modifier-coupling.md](./research/sculpt-modifier-coupling.md) | Where Blender couples sculpt mode to the modifier stack / geometry nodes; what the v1 deferral (no sculpting through active modifiers) skips and for how long. |
| [grid-correspondence.md](./research/grid-correspondence.md) | MDISPS ⇄ SculptCore grid-sample bijection (P8 P0): layouts, corner-anchor convention, discrete-vs-limit base offset. |
| [webgl-app-reports-insights.md](./research/webgl-app-reports-insights.md) | What the reference TypeScript app's integration/stroke-driver reports (`webgl-app-framework-reports/`) teach the plans: symmetry recipe, preview-dab stroke methods, `setNeighborMode`, BSMOOTH, dyntopo cadence, Catmull-Rom spacing. |
