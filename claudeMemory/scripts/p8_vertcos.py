"""P8 B2: verify the dedup subdiv-vertex bake (multires_reshape_from_vert_positions)
reproduces a fed surface exactly (no seam tear).

Cube + multires at level L. Take the zero-displacement eval mesh's vertex
positions (subdiv-vertex order), displace them by a function of position, bake
via the vertcos RNA, re-evaluate, and compare the eval vertices to the fed
target. Exact reproduction => the dedup feed carries no seam-ordering hazard
(the per-grid feed tears here; see p8_pin.py).

Run: blender --factory-startup --python p8_vertcos.py  -> %TEMP%/p8_vertcos.txt
"""
import bpy, os, tempfile
import numpy as np

OUT = os.path.join(tempfile.gettempdir(), "p8_vertcos.txt")


def _verts(mesh):
    n = len(mesh.vertices)
    co = np.empty(n * 3, dtype=np.float64)
    mesh.vertices.foreach_get("co", co)
    return co.reshape(-1, 3)


def _max_edge(mesh, co):
    nc = len(mesh.loops)
    cv = np.empty(nc, dtype=np.int32)
    mesh.loops.foreach_get("vertex_index", cv)
    quads = cv.reshape(-1, 4)
    d = np.linalg.norm(co[quads] - co[quads[:, [1, 2, 3, 0]]], axis=2)
    return float(d.max())


def main():
    lines = ["P8 B2 vertcos dedup bake fidelity", ""]
    try:
        for level in (2, 3, 4):
            bpy.ops.object.select_all(action='SELECT'); bpy.ops.object.delete()
            bpy.ops.mesh.primitive_cube_add(size=2.0)
            ob = bpy.context.active_object
            ob.modifiers.new("Multires", 'MULTIRES')
            for _ in range(level):
                bpy.ops.object.multires_subdivide(modifier="Multires")

            depsgraph = bpy.context.evaluated_depsgraph_get()
            depsgraph.update()
            base = _verts(ob.evaluated_get(depsgraph).data)
            # Displace per subdivided vertex (dedup order == vertcos order).
            target = base.copy()
            target[:, 2] += 0.3 * np.sin(2.0 * base[:, 0]) * np.cos(3.0 * base[:, 1])

            flat = np.ascontiguousarray(target.reshape(-1), dtype=np.float32)
            ob.multires_reshape_from_vert_positions(depsgraph, flat)
            ob.data.update_tag(); ob.update_tag(); depsgraph.update()

            eval_mesh = ob.evaluated_get(depsgraph).data
            got = _verts(eval_mesh)
            # Same subdivided topology -> same vertex order as `target`.
            err = float(np.abs(got - target).max()) if got.shape == target.shape else -1.0
            intended_edge = _max_edge(eval_mesh, target)
            got_edge = _max_edge(eval_mesh, got)
            lines.append(
                "level {:d}: verts={:d} reproduce_err={:.3e}  intended_edge={:.3e} "
                "got_edge={:.3e}  {:s}".format(
                    level, len(target), err, intended_edge, got_edge,
                    "CLEAN" if err >= 0 and err < 1e-4 else "MISMATCH"))
        lines.append("")
        lines.append("(per-grid feed tore to ~0.68 here; dedup should reproduce exactly)")
    except Exception as ex:
        import traceback
        lines.append("ERROR: {!r}".format(ex))
        lines.append(traceback.format_exc())
    with open(OUT, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))


bpy.app.timers.register(lambda: (main(), None)[1], first_interval=1.0)
