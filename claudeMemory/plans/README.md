# Plans

Implementation plans for the SculptCore integration. One file per plan
(`<short-kebab-name>.md`).

A plan should capture: the goal, the concrete change list (file → change →
size), the order of work, and how it will be verified. Derive plans from the
design in [../design/addon-custom-modes.md](../design/addon-custom-modes.md).

When implementing a plan:

- Prefix scaffolding/helper comments in the code with `CLAUDENOTE:` so they
  are greppable.
- Strip all `CLAUDENOTE:` comments before the plan is done.
- Audit every comment touched (not just `CLAUDENOTE:` ones) against the code
  and the Blender comment guidelines in [../../CLAUDE.md](../../CLAUDE.md).

See [../README.md](../README.md) for the full index.
