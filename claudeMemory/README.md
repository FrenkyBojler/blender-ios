# claudeMemory — Index

Working memory and documentation for the **SculptCore integration** project
(branch `sculptcore`): integrating the `extern/sculptcore` sculpting engine
into Blender as a first-class sculpt-mode addon.

This directory is Claude's scaffolding for the project. It is **temporary** —
see the cleanup checklist in [../CLAUDE.md](../CLAUDE.md) (delete
`claudeMemory/`, restore the original `claudeDocs/` + `AGENTS.md`) before the
final upstream PR.

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

## `plans/` and `research/`

Implementation plans go in [plans/](./plans/); research notes go in
[research/](./research/). When implementing a plan, mark scaffolding comments
with `CLAUDENOTE:` and strip them (plus audit all touched comments) when the
plan is complete — see [../CLAUDE.md](../CLAUDE.md).
