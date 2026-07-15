# Blender Codebase Documentation

Documentation set for navigating the Blender codebase, generated for use
alongside [CLAUDE.md](../CLAUDE.md) (coding standards and contribution
guidelines). Every doc gives concrete `file:line` references — verify they
still hold before relying on them for a change, since line numbers drift as
the codebase evolves.

## Index

| Doc | Covers |
|---|---|
| [project-overview.md](./project-overview.md) | Repository map: `source/`, `intern/`, `scripts/`, build/test directories, module prefixes. Start here. |
| [paint-mode-system.md](./paint-mode-system.md) | All paint/sculpt modes (Sculpt, Vertex/Weight/Texture Paint, Grease Pencil ×4, Curves Sculpt): DNA structs, mode entry/exit, stroke pipeline, brush dispatch, Python UI layer. |
| [brush-asset-system.md](./brush-asset-system.md) | How brushes are stored as weak asset references, resolved, and activated; per-tool-type brush memory; Essentials library defaults. |
| [python-bpy-integration.md](./python-bpy-integration.md) | `bpy.utils.register_class` mechanics, startup-script/add-on loading, and a deep dive into **paint-mode tool registration on the Python side** (tool system, brush-type enums, keymaps, asset shelves). |
| [dna-rna-data-system.md](./dna-rna-data-system.md) | DNA (serialized structs) vs. RNA (introspection layer): `makesdna`/`makesrna` codegen, `.blend` versioning, `IDProperty` custom data. |
| [depsgraph-and-operators.md](./depsgraph-and-operators.md) | Dependency graph (nodes, relations, evaluation, tagging) and the window manager (operator registration, keymaps, the event/modal loop). |
| [gpu-draw-and-nodes.md](./gpu-draw-and-nodes.md) | GPU backend abstraction (GL/Vulkan/Metal), Draw Manager and render engine registration, node-tree architecture, multi-function/lazy-function evaluation. |
| [build-and-testing.md](./build-and-testing.md) | CMake/make build system, unity builds, test suites, buildbot workflow. |
| [design-addon-custom-modes.md](./design-addon-custom-modes.md) | Design proposal: making object modes registrable from Python addons (custom sculpt mode). Maps the five hardwired blockers, proposes `ObjectModeType` + `OB_MODE_CUSTOM` + a wrapped undo type. |

## Suggested Reading Order

1. [project-overview.md](./project-overview.md) — orient yourself in the tree.
2. [dna-rna-data-system.md](./dna-rna-data-system.md) — the data model everything else builds on.
3. [depsgraph-and-operators.md](./depsgraph-and-operators.md) — how change propagates and how user actions are dispatched.
4. [paint-mode-system.md](./paint-mode-system.md) + [brush-asset-system.md](./brush-asset-system.md) — a full vertical slice through one major subsystem.
5. [python-bpy-integration.md](./python-bpy-integration.md) — how that subsystem (and everything else) is exposed to and driven from Python.
6. [gpu-draw-and-nodes.md](./gpu-draw-and-nodes.md), [build-and-testing.md](./build-and-testing.md) — as needed.

## Related Root-Level Docs

- [CLAUDE.md](../CLAUDE.md) — coding standards, commit conventions, `.blend`
  compatibility rules, release process. Overrides default behavior; read
  before making changes.
- [AGENTS.md](../AGENTS.md) — agent-facing guidance (if different from
  CLAUDE.md, check both).
