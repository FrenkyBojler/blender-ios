# Brush Textures (P7 Phase 2)

Wire Blender brush textures (`Brush.texture` + `texture_slot`) into the
engine's existing texture machinery. The engine side is already complete for
sampling: `Brush.tex_width/tex_height/tex_pixels` (grayscale row-major
floats), `TexCoordSpace` (Global / ViewPlane / ViewRepeat / StrokeCurved /
Projected), `tex_repeat`, and `sampleTexBilinear` (clamp-to-edge; returns 1.0
when no texture is bound). The DRAW / DRAW_SHARP / LAYERDRAW kernels multiply
strength by `sampleBrushTex(v.co, surfaceNo)`.

## Gaps

1. **No bridge seam** — none of the texture members are reflected, so
   Python/TS cannot bind a texture.
2. **`renderMatrix`** (drives ViewPlane / ViewRepeat UV) lives on
   `CommandExecutor::ctx` and is only set by the debug app.
3. **Blender side** — nothing maps `Brush.texture` / `texture_slot.map_mode`
   to the engine.

## Design

### Engine seam (bindings only; no kernel/codegen changes)

- `Brush::setTexture(int width, int height, util::Vector<float> &pixels)` —
  copies row-major grayscale floats; bad dims/size clears instead.
- `Brush::clearTexture()`.
- Reflect `coord_space`, `tex_repeat`, `tex_width`, `tex_height` (the enum
  binds like `falloff_shape`).
- `CommandExecutor::setRenderMatrix(util::Vector<float> &m16)` — 16 flat
  floats copied into `ctx.renderMatrix`, same convention as the debug app's
  `set_render_matrix` verb.

### Addon

- New `texture.py`: bake a Blender `Texture` to an N×N grayscale array via
  `texture.evaluate((x, y, 0))` over `[-1, 1]²` (N = 128; RGB mean as
  intensity). Cache by texture name; the depsgraph handler invalidates on
  Texture datablock updates.
- `mapping.py`: `map_mode` → `TexCoordSpace`:
  | Blender `map_mode` | engine | note |
  |---|---|---|
  | `'3D'` | Global (0) | world XY, saturates outside the unit tile |
  | `'VIEW_PLANE'` | ViewPlane (1) | needs renderMatrix |
  | `'TILED'` | ViewRepeat (2) | needs renderMatrix; `tex_repeat` = 1 for now |
  | `'AREA_PLANE'` | Projected (4) | tangent-plane UV in world units |
  | `'RANDOM'`, `'STENCIL'` | unmapped | texture cleared; parity checklist |
- `stroke.py` invoke: per stroke, `apply_texture(...)` binds/clears the
  texture, and for ViewPlane/ViewRepeat pushes
  `region_data.perspective_matrix` (flattened) through `setRenderMatrix`.

### Known parity limits (documented, not blockers)

- Engine `Projected` UV is world-units on the tangent plane (not
  brush-radius-normalized like Blender AREA_PLANE); textures span one world
  unit rather than the brush circle. Radius-normalized UV would need a new
  coord space mirrored into the WGSL/CUDA emitters — deferred.
- Only DRAW-family kernels currently sample the texture (matches the
  engine's kernel set, not Blender's every-brush texturing).
- No brightness/contrast/invert (`mtex` extras) yet.

## Verification

`claudeMemory/tests/brush_texture_test.py` (run_sync):
1. BLEND (gradient) texture, `'3D'` mapping, DRAW strokes on a dense grid
   plane → displacement in the bright half ≫ dark half; control stroke with
   no texture moves both halves comparably.
2. Bake cache: second bake of the same texture is a cache hit; a fake
   depsgraph texture-update invalidates.
3. `'RANDOM'` map mode leaves the engine texture cleared (uniform stroke).

Engine rebuild: `cd extern/sculptcore && node make.mjs build python`.
