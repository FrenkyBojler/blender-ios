# Shared SculptCore Checkout (webgl-app-framework)

Run the sister DCC's test suite (`C:\dev\webgl-app-framework`) against **our**
`extern/sculptcore` working tree — including uncommitted / unpushed changes —
so a sculptcore change can be validated through a second, battle-tested
integration before it lands.

## Goal

The webgl app consumes sculptcore as a git submodule at
`webgl-app-framework/sculptcore` (its own checkout, gitlink `7249743`). We want
that path to instead *be* our tree at `blender/main/extern/sculptcore` (which
carries the unpushed merge `9c33f9f` + litestl `e1642e9`), via an **NTFS
junction**, so both repos share one working copy and one build.

Direction is fixed: **webgl → ours**. Our merge commits live only in
`blender/main/.git/modules/extern/sculptcore`; junctioning the other way would
point our superproject at an object store that lacks them.

## Feasibility (verified 2026-07-19)

- **Git resolves through the junction.** Our submodule `.git` pointer is
  *relative* (`gitdir: ../../.git/modules/extern/sculptcore`, depth 2) while
  webgl's `sculptcore/` sits at depth 1 — a naive resolve would hit a
  nonexistent `C:\dev\.git\...`. In practice git canonicalizes the junction to
  the real path first, so `git status/log` through
  `<junction>/sculptcore` correctly report our HEAD `9c33f9f`.
- **Nested `litestl` resolves too** (its pointer is four levels deep):
  `<junction>/sculptcore/source/litestl` → `e1642e9`.
- Tested with a throwaway junction in the scratchpad; both confirmed, then
  removed.

## Preconditions / notes about the current state

- Our sculptcore is `ahead 17` of origin, **unpushed** (merge `9c33f9f`);
  litestl is `ahead 2`, **unpushed** (`e1642e9`). The shared checkout therefore
  depends on commits that exist only locally — do not GC our submodule store,
  and do not commit a gitlink bump in webgl referencing them.
- Our tree has an uncommitted regenerated
  `typescript/sculptcore/brush/brushWgsl.ts` (+ line-ending-only churn in
  `source/brush/kernels/generated/*.gen.h`). webgl imports
  `@sculptcore/api/sculptcore/brush/brushWgsl` directly, so its tests will
  exercise that file immediately. **Commit `brushWgsl.ts` before the first
  run** so the shared tree isn't validated against an uncommitted generated
  artifact.
- webgl's current `sculptcore/` holds real build output (`sculptcore.wasm`,
  `native/`, `native-msvc/`, `native-node/`, `wgsl_ts/`). The junction parks
  all of it; our tree has no WASM / node-addon build. CMake caches are keyed to
  absolute paths, so those artifacts can't be copied across — the first shared
  run needs a full rebuild (incl. emsdk + wgpu-native fetch).

## Config differences to reconcile

| Knob | webgl | ours |
|------|-------|------|
| `local-build-options.mjs` | present | **absent** (defaults) |
| `SBRUSH_BACKEND_WGSL` | **ON** | OFF |
| `SBRUSH_WEBGPU_COMPUTE` | OFF | OFF |
| `WITH_NATIVE_MSVC` | false | — |

Sharing the checkout shares whichever `local-build-options.mjs` is present.
Adopting webgl's (WGSL=ON) is strictly better for us: it fixes 4 of our 6
pre-existing native-test failures (`test_live_stroke`, `test_bsmooth`,
`test_automask_gpu`, `test_dyntopo_multistep_gpu` are all cpp-vs-WGSL A/B
tests that cannot pass with the backend off).

## The one genuine unknown: pnpm

sculptcore is **also a pnpm workspace member** of webgl (`pnpm-workspace.yaml`
lists six packages *inside* it: `sculptcore`, `sculptcore/typescript`,
`sculptcore/tests`, `sculptcore/tests/testViewer3D`,
`sculptcore/source/litestl/binding/typescriptRuntime`,
`sculptcore/source/litestl/tests`). Our sculptcore is itself a pnpm workspace
**root** with its own `node_modules`. Two parents managing overlapping
`node_modules` trees may thrash. This cannot be predicted — it is the item to
prove in the trial run.

## Approach chosen: junction (not worktree)

- **Junction** — instant propagation of uncommitted edits; one shared build.
  Cost: both repos share one `build/` and one `local-build-options.mjs`.
- **Worktree** (`git -C extern/sculptcore worktree add --detach
  <webgl>/sculptcore master`) — independent build configs, shared object store.
  Cost: must commit-in-ours / checkout-in-webgl per change.

Given active iteration on sculptcore and non-conflicting configs (WGSL=ON is a
win), use the **junction**.

## Steps

1. **Commit the regenerated WGSL** in our tree first:
   `git -C extern/sculptcore add typescript/sculptcore/brush/brushWgsl.ts &&
   git -C extern/sculptcore commit` (discard the `.gen.h` line-ending churn —
   content-identical). Confirm litestl is on `master` (not detached).
2. **Park webgl's submodule tree** (recoverable — keeps its `.git/modules`
   store and build artifacts):
   `mv C:\dev\webgl-app-framework\sculptcore
   C:\dev\webgl-app-framework\sculptcore.parked`
3. **Create the junction** (PowerShell, `New-Item -ItemType Junction`, since
   `mklink` choked on long paths in testing):
   `New-Item -ItemType Junction -Path C:\dev\webgl-app-framework\sculptcore
   -Target C:\dev\blender\main\extern\sculptcore`
4. **Pin webgl's submodule to hands-off** so `git submodule update` can't
   check its recorded gitlink (`7249743`) into our shared tree:
   `git -C C:\dev\webgl-app-framework config submodule.sculptcore.update none`
5. **Carry the build options over** (we have none; this gives us WGSL=ON):
   copy `sculptcore.parked/local-build-options.mjs` into the shared tree
   (i.e. into our `extern/sculptcore/`). Decide deliberately whether we *want*
   WGSL=ON permanently in our tree or only for these runs.
6. **Install + build + test**, from webgl root:
   - `pnpm install` — **watch for node_modules thrash** (the unknown above).
   - `pnpm run build-all` (codegen + wasm + native + node-addon + esbuild);
     first run also needs `pnpm run setup-sculptcore` (emsdk, wgpu-native,
     tools) if those aren't already fetched in our tree.
   - `pnpm test` (turbo → jest across workspace packages).

## Verification

- `git -C C:\dev\webgl-app-framework\sculptcore rev-parse HEAD` == `9c33f9f`
  (webgl sees our tree, not its own gitlink).
- Nested: `.../sculptcore/source/litestl` HEAD == `e1642e9`.
- webgl's `git status` shows `sculptcore` gitlink dirty — **expected and
  cosmetic** (it points at our unpushed commits); never commit that bump.
- `pnpm test` in webgl runs; integration tests that load built WASM/native
  (`tests/integration/litemesh_attr_render.test.ts`,
  `litemesh_quad_remesh.test.ts`, `native_testprint.test.ts`) pass against our
  engine.
- Our own sculptcore native suite, re-run with WGSL=ON, drops from 6→2
  pre-existing failures (the 2 residual: `test_debug_script`,
  `test_spatial_update_split`; `test_multires` remains an upstream exit-time
  flake).

## Teardown / revert

1. Remove the junction (deletes the link, **not** the target):
   `(Get-Item C:\dev\webgl-app-framework\sculptcore).Delete()`.
2. Restore webgl's own tree: `mv sculptcore.parked sculptcore`.
3. `git -C C:\dev\webgl-app-framework config --unset submodule.sculptcore.update`.

## Risks

- **Accidental `git submodule update` / `git checkout` in webgl** overwriting
  our working tree — mitigated by step 4, but a `--force` or a recursive
  superproject checkout can still bite. Our unpushed commits are the only
  copy; keep them backed by a branch.
- **pnpm node_modules contention** between the two workspace roots (unproven).
- **Shared `local-build-options.mjs` / `build/`** — a config change for one
  repo's run silently affects the other.
- **Absolute-path CMake caches** — moving/renaming either repo root
  invalidates the shared `build/`.
