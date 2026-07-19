# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Brush textures (brush-mapping Phase 2).

Bakes a Blender ``Texture`` datablock to the engine's grayscale texture and
binds it per stroke: ``Texture.evaluate`` sampled over an N x N grid on
``[-1, 1]^2`` (the intensity channel, matching Blender's own brush-texture
sampling), cached by texture name, invalidated when the depsgraph reports the
Texture changed. ``texture_slot.map_mode`` selects the engine
``TexCoordSpace``; the screen-pinned modes (Tiled / Stencil) additionally
need the stroke operator to push the region's perspective matrix
(``setRenderMatrix``) so the engine can perspective-project to viewport UV.
"""

import bpy

from . import engine

# Bake resolution: 128^2 = 16k RNA evaluate calls, well under stroke-start
# budget; the engine samples bilinearly so moderate resolution suffices.
BAKE_SIZE = 128

# Blender texture_slot.map_mode -> engine TexCoordSpace value (brush.h).
# Blender's View Plane is brush-centered (the texture follows the brush and
# scales with its radius), which matches the engine's normalized Projected
# space, not its screen-pinned ViewPlane — that one matches Stencil. RANDOM
# has no engine analogue yet (parity checklist).
_COORD_SPACE = {
    '3D': 0,          # Global
    'VIEW_PLANE': 4,  # Projected (brush-centered tangent plane)
    'AREA_PLANE': 4,  # Projected
    'TILED': 2,       # ViewRepeat (screen-pinned, tiled)
    'STENCIL': 1,     # ViewPlane (screen-pinned)
}

# Texture name -> flat row-major grayscale list (BAKE_SIZE^2 floats).
_cache = {}


def invalidate(name=None):
    """Drop the baked pixels for one texture (or all)."""
    if name is None:
        _cache.clear()
    else:
        _cache.pop(name, None)


def invalidate_from_depsgraph(depsgraph):
    """Called from the depsgraph handler: drop bakes of updated Textures."""
    for update in depsgraph.updates:
        if isinstance(update.id, bpy.types.Texture):
            _cache.pop(update.id.name, None)


def _bake(tex):
    """Row-major grayscale bake of `tex` over [-1, 1]^2 (z = 0). The
    intensity channel (`tin`, evaluate()[3]) matches what Blender's brush
    sampling uses for non-color textures."""
    pixels = _cache.get(tex.name)
    if pixels is not None:
        return pixels
    n = BAKE_SIZE
    evaluate = tex.evaluate
    pixels = [0.0] * (n * n)
    inv = 2.0 / (n - 1)
    idx = 0
    for j in range(n):
        y = j * inv - 1.0
        for i in range(n):
            pixels[idx] = evaluate((i * inv - 1.0, y, 0.0))[3]
            idx += 1
    _cache[tex.name] = pixels
    return pixels


def needs_render_matrix(bl_brush):
    """Whether the brush's texture mapping reads ctx.renderMatrix
    (view-pinned UV)."""
    slot = bl_brush.texture_slot if bl_brush else None
    return (bl_brush is not None and bl_brush.texture is not None
            and slot is not None and slot.map_mode in {'TILED', 'STENCIL'})


def apply_texture(bl_brush, sc_brush):
    """Bind (or clear) the engine brush texture for a stroke. Unmapped
    map modes clear so the kernels' sampleBrushTex is a no-op 1.0."""
    import sculptcore

    tex = bl_brush.texture if bl_brush else None
    coord_space = None
    if tex is not None and bl_brush.texture_slot is not None:
        coord_space = _COORD_SPACE.get(bl_brush.texture_slot.map_mode)
    if tex is None or coord_space is None:
        sc_brush.clearTexture()
        return

    mgr = engine.manager()
    pixels = _bake(tex)
    with sculptcore.construct_from_items(mgr, mgr.get("float"), pixels) as vec:
        sc_brush.setTexture(BAKE_SIZE, BAKE_SIZE, vec)
    sc_brush.coord_space = coord_space
    sc_brush.tex_repeat = 1.0


def apply_render_matrix(context, executor):
    """Push the region's perspective matrix (world -> NDC) into the executor
    for ViewPlane/ViewRepeat UV. Flat 16 floats, row-major rows appended in
    order — the same element order the mat4 assignment consumes."""
    import sculptcore

    rv3d = context.region_data
    if rv3d is None:
        return
    mat = rv3d.perspective_matrix
    flat = [mat[r][c] for r in range(4) for c in range(4)]
    mgr = engine.manager()
    with sculptcore.construct_from_items(mgr, mgr.get("float"), flat) as vec:
        executor.setRenderMatrix(vec)
