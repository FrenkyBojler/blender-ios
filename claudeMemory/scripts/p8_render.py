"""P8 verification tail: production-asset render comparison.

Renders a displaced multires asset (workbench, fixed camera/light), passes
the object through a full no-stroke mode session (enter -> exit via the real
custom-mode toggle, including modifier suppression/restore and the deferred
flush), renders again, and image-diffs the two renders. The round trip is
~1e-7 in geometry, so the renders must match to within a hair of
quantization noise.

Run: blender --factory-startup --python p8_render.py  -> %TEMP%/p8_render.txt
"""
import bpy, os, tempfile
import numpy as np

OUT = os.path.join(tempfile.gettempdir(), "p8_render.txt")
LEVEL = 3


def _eval_verts(ob, depsgraph):
    mesh = ob.evaluated_get(depsgraph).data
    co = np.empty(len(mesh.vertices) * 3, dtype=np.float64)
    mesh.vertices.foreach_get("co", co)
    return co.reshape(-1, 3)


def _render(path):
    scene = bpy.context.scene
    scene.render.engine = 'BLENDER_WORKBENCH'
    scene.render.resolution_x = 512
    scene.render.resolution_y = 512
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)


def _load_pixels(path):
    img = bpy.data.images.load(path)
    px = np.array(img.pixels[:], dtype=np.float32)
    bpy.data.images.remove(img)
    return px


def main():
    lines = ["P8 production-asset render comparison", ""]
    try:
        if "sculptcore_addon" not in bpy.context.preferences.addons:
            bpy.ops.preferences.addon_enable(module="sculptcore_addon")
        from sculptcore_addon import engine

        context = bpy.context
        for ob in list(bpy.data.objects):
            if ob.type == 'MESH':
                bpy.data.objects.remove(ob, do_unlink=True)
        bpy.ops.mesh.primitive_uv_sphere_add(segments=48, ring_count=24, radius=1.0)
        ob = context.active_object
        ob.modifiers.new("Multires", 'MULTIRES')
        for _ in range(LEVEL):
            bpy.ops.object.multires_subdivide(modifier="Multires")

        depsgraph = context.evaluated_depsgraph_get(); depsgraph.update()
        base = _eval_verts(ob, depsgraph)
        asset = base.copy()
        asset[:, 2] += 0.15 * np.sin(4.1 * base[:, 0]) * np.cos(3.3 * base[:, 1])
        asset[:, 0] += 0.08 * np.sin(5.7 * base[:, 1] + 1.0)
        ob.multires_reshape_from_vert_positions(
            depsgraph, np.ascontiguousarray(asset.reshape(-1), dtype=np.float32))
        ob.data.update_tag(); depsgraph.update()

        tmp = tempfile.gettempdir()
        path_a = os.path.join(tmp, "p8_render_a.png")
        path_b = os.path.join(tmp, "p8_render_b.png")
        _render(path_a)

        # Full no-stroke session through the real mode path.
        bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
        entered = ob.name in engine.sessions
        bpy.ops.object.custom_mode_toggle(mode_id="sculptcore.sculpt")
        lines.append("session entered/exited: {:s}".format("yes" if entered else "NO"))

        _render(path_b)

        a = _load_pixels(path_a)
        b = _load_pixels(path_b)
        max_diff = float(np.abs(a - b).max())
        mean_diff = float(np.abs(a - b).mean())
        ok = entered and max_diff < 0.02 and mean_diff < 0.001
        lines.append("render diff: max={:.5f} mean={:.6f} (0..1 scale)".format(
            max_diff, mean_diff))
        lines.append("")
        lines.append("ALL PASS" if ok else "SOME FAILED")
    except Exception as ex:
        import traceback
        lines.append("ERROR: {!r}".format(ex))
        lines.append(traceback.format_exc())
    with open(OUT, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    bpy.ops.wm.quit_blender()


bpy.app.timers.register(lambda: (main(), None)[1], first_interval=1.0)
